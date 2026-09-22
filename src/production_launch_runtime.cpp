#include "hydra/production_launch_runtime.hpp"

#include "hydra/display_topology.hpp"
#include "hydra/production_compatibility_activation.hpp"
#include "hydra/seat_display_layout.hpp"
#include "hydra/window_placement.hpp"
#include "hydra/window_tracker.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace hydra::production {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr auto kWindowReadyTimeout = std::chrono::milliseconds(5000);
constexpr auto kAudioReadyTimeout = std::chrono::milliseconds(3000);
constexpr auto kProcessHandoffReadyTimeout = std::chrono::seconds(10);
// Do not publish a launcher root as durable process authority from a single
// observation. A short unchanged-authority window lets Job notifications and
// exact parent/descendant evidence converge before SeatGameLifecycle performs
// its immediate running() check.
constexpr auto kProcessAuthorityStabilityWindow = std::chrono::milliseconds(150);
constexpr auto kPollInterval = std::chrono::milliseconds(25);

std::string materializationSessionId(const runtime::RuntimeSessionId& sessionId) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(sessionId.bytes.size() * 2u);
    for (const auto value : sessionId.bytes) {
        encoded.push_back(kHex[(value >> 4u) & 0x0fu]);
        encoded.push_back(kHex[value & 0x0fu]);
    }
    return encoded;
}

class StableHash final {
public:
    void byte(std::uint8_t value) noexcept {
        value_ ^= static_cast<std::uint64_t>(value);
        value_ *= kFnvPrime;
    }
    void boolean(bool value) noexcept { byte(value ? 1u : 0u); }
    void u32(std::uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32u; shift += 8u) {
            byte(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void u64(std::uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64u; shift += 8u) {
            byte(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void text(std::string_view value) noexcept {
        u64(static_cast<std::uint64_t>(value.size()));
        for (const char ch : value) {
            byte(static_cast<std::uint8_t>(static_cast<unsigned char>(ch)));
        }
    }
    void wide(std::wstring_view value) noexcept {
        u64(static_cast<std::uint64_t>(value.size()));
        for (const wchar_t ch : value) u32(static_cast<std::uint32_t>(ch));
    }
    std::uint64_t value() const noexcept { return value_; }

private:
    std::uint64_t value_{kFnvOffset};
};

void hashCompatibility(
    StableHash& hash,
    const std::optional<profile::CompatibilityReference>& value) noexcept {
    hash.boolean(value.has_value());
    if (!value) return;
    hash.text(value->recordId);
    hash.text(value->provenance);
    hash.u32(value->evidenceRevision);
}

void hashRecipe(StableHash& hash,
                const std::optional<profile::InstanceRecipe>& recipe) noexcept {
    hash.boolean(recipe.has_value());
    if (!recipe) return;
    hash.u64(static_cast<std::uint64_t>(recipe->arguments.size()));
    for (const auto& argument : recipe->arguments) hash.wide(argument);
    hash.boolean(recipe->workingDirectory.has_value());
    if (recipe->workingDirectory) hash.wide(*recipe->workingDirectory);
    hash.boolean(recipe->dataRoot.has_value());
    if (recipe->dataRoot) hash.wide(*recipe->dataRoot);
}

void hashRequirements(StableHash& hash,
                      const launch::Requirements& value) noexcept {
    hash.boolean(value.display);
    hash.boolean(value.keyboard);
    hash.boolean(value.mouse);
    hash.boolean(value.controller);
    hash.boolean(value.audioOutput);
    hash.boolean(value.windowOwnership);
    hash.boolean(value.recovery);
    hash.boolean(value.highRisk);
}

void hashCapabilities(StableHash& hash,
                      const launch::Capabilities& value) noexcept {
    hash.boolean(value.process);
    hash.boolean(value.window);
    hash.boolean(value.display);
    hash.boolean(value.input);
    hash.boolean(value.controller);
    hash.boolean(value.audio);
    hash.boolean(value.recovery);
}

template <typename Range>
void hashSortedWideRange(StableHash& hash, const Range& range) {
    std::vector<std::wstring> values(range.begin(), range.end());
    std::sort(values.begin(), values.end());
    hash.u64(static_cast<std::uint64_t>(values.size()));
    for (const auto& value : values) hash.wide(value);
}

bool boundedText(std::string_view value, std::size_t maximum = 2048u,
                 bool allowEmpty = false) noexcept {
    return (allowEmpty || !value.empty()) && value.size() <= maximum &&
           value.find('\0') == std::string_view::npos;
}

bool boundedWide(std::wstring_view value, std::size_t maximum = 2048u,
                 bool allowEmpty = false) noexcept {
    return (allowEmpty || !value.empty()) && value.size() <= maximum &&
           value.find(L'\0') == std::wstring_view::npos;
}

std::optional<std::string> asciiStableId(std::wstring_view value) {
    if (value.empty() || value.size() > 2048u) return std::nullopt;
    std::string result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch <= 0 || ch > 0x7f) return std::nullopt;
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

bool validateProviderSeat(const plan::SeatProviderLaunchPlan& seat,
                          std::string& error) {
    if (seat.seatId == 0 || !boundedText(seat.playerId, 256u) ||
        !boundedText(seat.gameId, 256u) || seat.requirementRevision == 0 ||
        seat.hardwareFingerprint == 0) {
        error = "provider Seat plan has invalid bounded identity/revision fields";
        return false;
    }
    if (seat.setupId && !boundedText(*seat.setupId, 256u)) {
        error = "provider Seat setup identity is malformed";
        return false;
    }
    const auto& request = seat.launchRequest;
    if (!boundedText(request.providerId, 256u) ||
        request.gameId != seat.gameId || request.metadataRevision == 0 ||
        !boundedWide(request.target, 2048u) ||
        request.arguments.size() > launch::kMaximumLaunchArguments ||
        (request.providerAppId && !boundedText(*request.providerAppId, 256u)) ||
        (request.accountRef && !boundedText(*request.accountRef, 256u)) ||
        !boundedText(request.launchCorrelationId, 256u)) {
        error = "provider launch request is malformed or exceeds production bounds";
        return false;
    }
    for (const auto& argument : request.arguments) {
        if (!boundedWide(argument, 2048u, true)) {
            error = "provider launch argument exceeds production bounds";
            return false;
        }
    }
    if (request.workingDirectory &&
        !boundedWide(*request.workingDirectory, 2048u)) {
        error = "provider working directory exceeds production bounds";
        return false;
    }
    if (seat.requirements.highRisk) {
        // ProviderAwareLaunchPlan v1 does not carry the compiler's explicit
        // high-risk approval bit. The host must not reconstruct approval from
        // absence of an error, so v1 production installation rejects it.
        error = "provider plan v1 cannot carry authoritative high-risk approval";
        return false;
    }
    if (!seat.capabilities.process ||
        (seat.requirements.windowOwnership && !seat.capabilities.window) ||
        (seat.requirements.display && !seat.capabilities.display) ||
        ((seat.requirements.keyboard || seat.requirements.mouse) &&
         !seat.capabilities.input) ||
        (seat.requirements.controller && !seat.capabilities.controller) ||
        (seat.requirements.audioOutput && !seat.capabilities.audio) ||
        (seat.requirements.recovery && !seat.capabilities.recovery)) {
        error = "provider Seat plan requires a capability it does not declare";
        return false;
    }
    return true;
}

const SeatConfig* findConfiguredSeat(std::span<const SeatConfig> seats,
                                     SeatId seatId) noexcept {
    const auto found = std::find_if(seats.begin(), seats.end(),
                                    [seatId](const SeatConfig& seat) {
                                        return seat.seatId == seatId;
                                    });
    return found == seats.end() ? nullptr : &*found;
}

const plan::SeatProviderLaunchPlan* findProviderSeat(
    const plan::ProviderAwareLaunchPlan& plan, SeatId seatId) noexcept {
    const auto found = std::find_if(plan.seats.begin(), plan.seats.end(),
                                    [seatId](const auto& seat) {
                                        return seat.seatId == seatId;
                                    });
    return found == plan.seats.end() ? nullptr : &*found;
}

const requirement::TrustedGameRuntimeAuthority* findTrustedAuthority(
    const requirement::TrustedRequirementSnapshot& snapshot,
    const plan::SeatProviderLaunchPlan& seatPlan) noexcept {
    const auto found = std::find_if(
        snapshot.authorities.begin(), snapshot.authorities.end(),
        [&](const requirement::TrustedGameRuntimeAuthority& authority) {
            return authority.requirement.gameId == seatPlan.gameId &&
                   authority.requirement.revision == seatPlan.requirementRevision &&
                   authority.providerId == seatPlan.launchRequest.providerId &&
                   authority.providerAppId == seatPlan.launchRequest.providerAppId &&
                   authority.providerMetadataRevision ==
                       seatPlan.launchRequest.metadataRevision;
        });
    return found == snapshot.authorities.end() ? nullptr : &*found;
}

std::vector<launch::ResourceKind> requiredResources(
    const launch::Requirements& requirements) {
    std::vector<launch::ResourceKind> resources;
    if (requirements.recovery) resources.push_back(launch::ResourceKind::Recovery);
    resources.push_back(launch::ResourceKind::Process);
    if (requirements.windowOwnership) resources.push_back(launch::ResourceKind::Window);
    if (requirements.display) resources.push_back(launch::ResourceKind::Display);
    if (requirements.keyboard || requirements.mouse) {
        resources.push_back(launch::ResourceKind::Input);
    }
    if (requirements.controller) resources.push_back(launch::ResourceKind::Controller);
    if (requirements.audioOutput) resources.push_back(launch::ResourceKind::Audio);
    return resources;
}

launch::SeatActivationPlan makeActivationPlan(
    const ProviderPlanRegistryEntry& entry,
    const plan::SeatProviderLaunchPlan& seatPlan,
    const SeatConfig& seatConfig) {
    launch::SeatActivationPlan result;
    result.seatId = entry.seatId;
    result.seat = seatConfig;
    result.target.gameId = seatPlan.gameId;
    result.target.process.seatId = entry.seatId;
    result.target.process.executablePath = seatPlan.launchRequest.target;
    result.target.process.arguments = seatPlan.launchRequest.arguments;
    if (seatPlan.launchRequest.workingDirectory) {
        result.target.process.workingDirectory =
            *seatPlan.launchRequest.workingDirectory;
    }
    result.target.process.architecture = process::ProcessArchitecture::Any;
    result.target.process.containment = process::ProcessContainmentPolicy::RequireJobObject;
    result.target.requirements = seatPlan.requirements;
    result.target.capabilities = seatPlan.capabilities;
    result.target.highRiskApproved = false;
    result.resources = requiredResources(result.target.requirements);
    StableHash hash;
    hash.u64(entry.planFingerprint);
    hash.u64(entry.planRevision);
    hash.u32(entry.seatId);
    hash.u64(entry.seatGameGeneration);
    result.fingerprint = hash.value();
    if (result.fingerprint == 0) result.fingerprint = 1u;
    return result;
}

class ProductionActivationContextAuthority final
    : public IProductionActivationContext {
public:
    explicit ProductionActivationContextAuthority(ProductionActivationEpoch epoch)
        : epoch_(std::move(epoch)) {}

    ProductionActivationContextSnapshot snapshot() const override {
        ProductionActivationContextSnapshot result;
        result.epoch = epoch_;

        std::shared_ptr<process::SeatProcessGroup> group;
        std::uint64_t publicationRevision = 0;
        {
            std::lock_guard lock(mutex_);
            if (invalidated_) {
                result.stage = ProductionActivationContextStage::ProcessInvalidated;
                return result;
            }
            if (!published_) return result;
            group = processGroup_.lock();
            publicationRevision = publicationRevision_;
        }

        if (!group) {
            result.stage = ProductionActivationContextStage::ProcessUnverifiable;
            return result;
        }

        process::ProcessHandoffSnapshot handoff;
        try {
            handoff = group->handoffSnapshot();
        } catch (...) {
            result.stage = ProductionActivationContextStage::ProcessUnverifiable;
            return result;
        }

        result.handoffState = handoff.state;
        result.handoffGeneration = handoff.handoffGeneration;
        result.treeSequence = handoff.treeSequence;
        if (handoff.seatId != epoch_.seatId) {
            result.stage = ProductionActivationContextStage::ProcessUnverifiable;
            return result;
        }

        switch (handoff.state) {
            case process::ProcessHandoffState::RootActive:
            case process::ProcessHandoffState::DescendantActive:
                if (!handoff.active() ||
                    !group->ownsExactIdentity(handoff.authoritativeProcess)) {
                    result.stage = ProductionActivationContextStage::ProcessUnverifiable;
                    break;
                }
                result.stage = ProductionActivationContextStage::ProcessActive;
                result.process = ProductionProcessActivatedContext{
                    epoch_, handoff.authoritativeProcess, handoff.handoffGeneration,
                    handoff.treeSequence, handoff.state};
                break;
            case process::ProcessHandoffState::HandoffPending:
                result.stage = ProductionActivationContextStage::HandoffPending;
                break;
            case process::ProcessHandoffState::TreeExited:
                result.stage = ProductionActivationContextStage::ProcessExited;
                break;
            case process::ProcessHandoffState::Unverifiable:
                result.stage = ProductionActivationContextStage::ProcessUnverifiable;
                break;
            case process::ProcessHandoffState::UnsupportedContainment:
                result.stage = ProductionActivationContextStage::UnsupportedContainment;
                break;
        }

        // A concurrent rollback or rebind must never return a process snapshot
        // captured immediately before invalidation.
        {
            std::lock_guard lock(mutex_);
            if (invalidated_ || !published_ ||
                publicationRevision != publicationRevision_) {
                result.process.reset();
                result.handoffState.reset();
                result.handoffGeneration = 0;
                result.treeSequence = 0;
                result.stage = invalidated_
                    ? ProductionActivationContextStage::ProcessInvalidated
                    : ProductionActivationContextStage::PreProcess;
            }
        }
        return result;
    }

    bool validatesEpoch(const ProductionActivationEpoch& epoch) const noexcept override {
        std::lock_guard lock(mutex_);
        return !invalidated_ && epoch_.valid() && epoch == epoch_;
    }

    bool validatesCurrentProcess(
        const ProductionProcessActivatedContext& processContext) const noexcept override {
        if (!processContext.valid() || processContext.epoch != epoch_) return false;
        try {
            const auto current = snapshot();
            return current.stage == ProductionActivationContextStage::ProcessActive &&
                   current.process && current.process->epoch == processContext.epoch &&
                   current.process->authoritativeProcess ==
                       processContext.authoritativeProcess &&
                   current.process->handoffGeneration ==
                       processContext.handoffGeneration &&
                   current.process->handoffState == processContext.handoffState;
        } catch (...) {
            return false;
        }
    }

    bool publishVerifiedProcess(
        const std::shared_ptr<process::SeatProcessGroup>& group,
        const process::ProcessHandoffSnapshot& verifiedHandoff,
        std::string& error) {
        if (!group || !epoch_.valid() || group->seatId() != epoch_.seatId) {
            error = "activation process publication has wrong Seat/epoch ownership";
            return false;
        }
        if (verifiedHandoff.seatId != epoch_.seatId || !verifiedHandoff.active() ||
            !group->ownsExactIdentity(verifiedHandoff.authoritativeProcess)) {
            error = "activation process publication requires verified exact handoff ownership";
            return false;
        }

        std::lock_guard lock(mutex_);
        if (invalidated_) {
            error = "activation context was invalidated before process publication";
            return false;
        }
        if (published_) {
            const auto existing = processGroup_.lock();
            if (existing != group) {
                error = "activation context cannot be rebound to another process group";
                return false;
            }
            error.clear();
            return true;
        }
        processGroup_ = group;
        published_ = true;
        ++publicationRevision_;
        if (publicationRevision_ == 0) ++publicationRevision_;
        error.clear();
        return true;
    }

    void invalidateProcess() noexcept {
        std::lock_guard lock(mutex_);
        processGroup_.reset();
        published_ = false;
        invalidated_ = true;
        ++publicationRevision_;
        if (publicationRevision_ == 0) ++publicationRevision_;
    }

    bool reusableFor(const ProductionActivationEpoch& epoch) const noexcept {
        std::lock_guard lock(mutex_);
        return !invalidated_ && epoch == epoch_;
    }

private:
    const ProductionActivationEpoch epoch_;
    mutable std::mutex mutex_;
    std::weak_ptr<process::SeatProcessGroup> processGroup_;
    std::uint64_t publicationRevision_{0};
    bool published_{false};
    bool invalidated_{false};
};

struct SeatResourceContext {
    SeatResourceContext(launch::SeatActivationPlan activation,
                        ProductionActivationEpoch epoch)
        : plan(std::move(activation)),
          activationAuthority(
              std::make_shared<ProductionActivationContextAuthority>(std::move(epoch))) {}

    launch::SeatActivationPlan plan;
    std::shared_ptr<ProductionActivationContextAuthority> activationAuthority;
    std::mutex mutex;
    std::vector<std::wstring> trustedHandoffExecutables;
    std::shared_ptr<process::SeatProcessGroup> processGroup;
    std::optional<process::ProcessIdentity> rootIdentity;
    std::unique_ptr<windowing::WindowTracker> windowTracker;
    std::optional<display::SeatDisplayGroup> displayGroup;
    std::vector<windowing::WindowRestoreState> windowRestoreStates;
    std::shared_ptr<controller::PollWorker> controllerWorker;
    std::unique_ptr<controller::SeatControllerRuntime> controllerRuntime;
    std::optional<audio::RouteTransaction> audioTransaction;
};

bool samePlan(const launch::SeatActivationPlan& left,
              const launch::SeatActivationPlan& right) noexcept {
    return left.seatId == right.seatId && left.fingerprint == right.fingerprint &&
           left.target.gameId == right.target.gameId;
}

class ProcessResource final : public launch::ISeatActivationResource {
public:
    explicit ProcessResource(std::shared_ptr<SeatResourceContext> context)
        : context_(std::move(context)) {}

    launch::ResourceKind kind() const noexcept override {
        return launch::ResourceKind::Process;
    }
    bool prepare(const launch::SeatActivationPlan& plan,
                 const runtime::SeatGameBinding& binding,
                 std::string& error) override {
        if (!samePlan(plan, context_->plan) || binding.gameId != plan.target.gameId ||
            plan.target.process.seatId != plan.seatId ||
            plan.target.process.executablePath.empty() ||
            context_->trustedHandoffExecutables.empty() ||
            context_->trustedHandoffExecutables.size() >
                process::kMaximumTrustedHandoffExecutables) {
            error = "process resource plan/binding or trusted handoff evidence mismatch";
            return false;
        }
        prepared_ = true;
        error.clear();
        return true;
    }
    bool activate(std::string& error) override {
        if (!prepared_) {
            error = "process resource was not prepared";
            return false;
        }
        auto launched = process::ProcessLauncher::launch(context_->plan.target.process,
                                                         &error);
        if (!launched.group || !launched.root.valid()) {
            if (error.empty()) error = "ProcessLauncher did not return exact ownership";
            return false;
        }
        std::lock_guard lock(context_->mutex);
        context_->rootIdentity = launched.root;
        context_->processGroup = std::move(launched.group);
        std::string evidenceError;
        if (!context_->processGroup->configureTrustedHandoffExecutables(
                context_->trustedHandoffExecutables, &evidenceError)) {
            error = "ProcessLauncher ownership could not be bound to trusted handoff evidence";
            if (!evidenceError.empty()) error += ": " + evidenceError;
            return false;
        }
        error.clear();
        return true;
    }
    bool verifyActive(std::string& error) override {
        std::lock_guard lock(context_->mutex);
        if (!context_->processGroup) {
            error = "owned process group is absent";
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              kProcessHandoffReadyTimeout;
        std::optional<process::ProcessIdentity> stableAuthority;
        std::uint64_t stableHandoffGeneration = 0u;
        std::chrono::steady_clock::time_point stableSince{};
        for (;;) {
            const auto handoff = context_->processGroup->handoffSnapshot();
            if (handoff.seatId != context_->plan.seatId) {
                error = "owned process lineage belongs to the wrong Seat";
                return false;
            }
            const auto now = std::chrono::steady_clock::now();
            if (handoff.active()) {
                if (!stableAuthority ||
                    !stableAuthority->sameInstance(handoff.authoritativeProcess) ||
                    stableHandoffGeneration != handoff.handoffGeneration) {
                    stableAuthority = handoff.authoritativeProcess;
                    stableHandoffGeneration = handoff.handoffGeneration;
                    stableSince = now;
                } else if (now - stableSince >= kProcessAuthorityStabilityWindow) {
                    if (!context_->activationAuthority->publishVerifiedProcess(
                            context_->processGroup, handoff, error)) {
                        if (error.empty()) {
                            error = "verified process handoff could not be published to activation context";
                        }
                        return false;
                    }
                    error.clear();
                    return true;
                }
            } else if (handoff.state == process::ProcessHandoffState::HandoffPending) {
                stableAuthority.reset();
                stableHandoffGeneration = 0u;
            } else {
                error = "owned process lineage is not verifiably active (" +
                        std::string(process::processHandoffStateName(handoff.state)) + ")";
                return false;
            }
            if (now >= deadline) {
                error = "owned process authority did not stabilize before the bounded handoff timeout (" +
                        std::string(process::processHandoffStateName(handoff.state)) + ")";
                return false;
            }
            std::this_thread::sleep_for(kPollInterval);
        }
    }
    bool rollback(std::string& error) noexcept override {
        std::lock_guard lock(context_->mutex);
        // Invalidate consumer-visible authority before process teardown begins.
        // A failed cleanup may retain the Job for RecoveryRequired, but it must
        // never leave a stale process attachment eligible for new mutations.
        context_->activationAuthority->invalidateProcess();
        if (!context_->processGroup) {
            error.clear();
            return true;
        }
        process::ProcessStopPolicy policy;
        const bool stopped = context_->processGroup->stop(policy, &error) &&
                             context_->processGroup->waitForEmpty(0u);
        if (!stopped) {
            if (error.empty()) error = "owned process tree cleanup could not be verified";
            return false;
        }
        context_->processGroup.reset();
        error.clear();
        return true;
    }
    bool verifySafe(std::string& error) noexcept override {
        std::lock_guard lock(context_->mutex);
        if (context_->processGroup && !context_->processGroup->waitForEmpty(0u)) {
            error = "owned process tree cleanup remains live or unverifiable";
            return false;
        }
        error.clear();
        return true;
    }
    bool active() const noexcept override {
        std::lock_guard lock(context_->mutex);
        if (!context_->processGroup) return false;
        try {
            const auto handoff = context_->processGroup->handoffSnapshot();
            return handoff.seatId == context_->plan.seatId &&
                   (handoff.active() ||
                    handoff.state == process::ProcessHandoffState::HandoffPending);
        } catch (...) {
            return false;
        }
    }

private:
    std::shared_ptr<SeatResourceContext> context_;
    bool prepared_{false};
};

class WindowResource final : public launch::ISeatActivationResource {
public:
    explicit WindowResource(std::shared_ptr<SeatResourceContext> context)
        : context_(std::move(context)) {}

    launch::ResourceKind kind() const noexcept override {
        return launch::ResourceKind::Window;
    }
    bool prepare(const launch::SeatActivationPlan& plan,
                 const runtime::SeatGameBinding& binding,
                 std::string& error) override {
        if (!samePlan(plan, context_->plan) || binding.gameId != plan.target.gameId) {
            error = "window resource plan/binding mismatch";
            return false;
        }
        prepared_ = true;
        error.clear();
        return true;
    }
    bool activate(std::string& error) override {
        if (!prepared_) {
            error = "window resource was not prepared";
            return false;
        }
        std::lock_guard lock(context_->mutex);
        if (!context_->processGroup) {
            error = "window ownership requires an active exact process tree";
            return false;
        }
        const auto handoff = context_->processGroup->handoffSnapshot();
        if (handoff.seatId != context_->plan.seatId || !handoff.active()) {
            error = "window ownership requires a verified Seat process handoff";
            return false;
        }
        context_->windowTracker = std::make_unique<windowing::WindowTracker>();
        context_->windowTracker->setProcessTrees({context_->processGroup->snapshot()});
        if (!context_->windowTracker->start(&error)) {
            context_->windowTracker.reset();
            return false;
        }
        error.clear();
        return true;
    }
    bool verifyActive(std::string& error) override {
        const auto deadline = std::chrono::steady_clock::now() + kWindowReadyTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(context_->mutex);
                if (!context_->windowTracker || !context_->processGroup) {
                    error = "window tracker lost process ownership";
                    return false;
                }
                const auto handoff = context_->processGroup->handoffSnapshot();
                if (handoff.seatId != context_->plan.seatId || !handoff.active()) {
                    error = "window tracker lost verified Seat process handoff";
                    return false;
                }
                context_->windowTracker->setProcessTrees(
                    {context_->processGroup->snapshot()});
                const auto snapshot = context_->windowTracker->snapshot();
                if (std::any_of(snapshot.windows.begin(), snapshot.windows.end(),
                                [&](const auto& window) {
                                    return window.seatId == context_->plan.seatId &&
                                           context_->windowTracker->validateIdentity(
                                               window.identity);
                                })) {
                    error.clear();
                    return true;
                }
            }
            std::this_thread::sleep_for(kPollInterval);
        }
        error = "no exact owned game window appeared before bounded timeout";
        return false;
    }
    bool rollback(std::string& error) noexcept override {
        std::lock_guard lock(context_->mutex);
        if (context_->windowTracker) context_->windowTracker->stop();
        context_->windowTracker.reset();
        error.clear();
        return true;
    }
    bool verifySafe(std::string& error) noexcept override {
        std::lock_guard lock(context_->mutex);
        if (context_->windowTracker && context_->windowTracker->running()) {
            error = "window tracker remains active";
            return false;
        }
        error.clear();
        return true;
    }
    bool active() const noexcept override {
        std::lock_guard lock(context_->mutex);
        return context_->windowTracker && context_->windowTracker->running();
    }

private:
    std::shared_ptr<SeatResourceContext> context_;
    bool prepared_{false};
};

class DisplayResource final : public launch::ISeatActivationResource {
public:
    explicit DisplayResource(std::shared_ptr<SeatResourceContext> context)
        : context_(std::move(context)) {}

    launch::ResourceKind kind() const noexcept override {
        return launch::ResourceKind::Display;
    }
    bool prepare(const launch::SeatActivationPlan& plan,
                 const runtime::SeatGameBinding& binding,
                 std::string& error) override {
        if (!samePlan(plan, context_->plan) || binding.gameId != plan.target.gameId ||
            plan.seat.displayIds.empty() || !plan.seat.primaryDisplayId) {
            error = "display resource requires an exact Seat display group";
            return false;
        }
        prepared_ = true;
        error.clear();
        return true;
    }
    bool activate(std::string& error) override {
        if (!prepared_) {
            error = "display resource was not prepared";
            return false;
        }
        display::SeatDisplayRequest request;
        request.seatId = context_->plan.seatId;
        for (const auto& displayId : context_->plan.seat.displayIds) {
            const auto stable = asciiStableId(displayId);
            if (!stable) {
                error = "Seat display identity is not a bounded stable ASCII key";
                return false;
            }
            request.outputs.push_back({*stable, true, false});
        }
        const auto primary = asciiStableId(*context_->plan.seat.primaryDisplayId);
        if (!primary) {
            error = "Seat primary display identity is not a bounded stable ASCII key";
            return false;
        }
        request.primaryOutputId = *primary;

        display::DisplayTopologyInventory inventory;
        const auto topology = inventory.refresh();
        if (!topology.querySucceeded) {
            error = "production display topology query failed";
            return false;
        }
        const auto layouts = display::buildSeatDisplayLayouts(topology, {request});
        if (!layouts.valid || layouts.groups.size() != 1u || layouts.groups.front().degraded) {
            error = layouts.errors.empty() ? "Seat display layout is unavailable"
                                           : layouts.errors.front();
            return false;
        }

        std::lock_guard lock(context_->mutex);
        context_->displayGroup = layouts.groups.front();
        context_->windowRestoreStates.clear();
        if (context_->windowTracker) {
            windowing::WindowPlacementEngine engine(*context_->windowTracker);
            windowing::WindowPlacementPolicy policy;
            policy.mode = windowing::WindowPlacementMode::PlaceOnPrimaryOutput;
            const auto windows = context_->windowTracker->snapshot().windows;
            for (const auto& window : windows) {
                if (window.seatId != context_->plan.seatId ||
                    window.role != windowing::WindowRole::PrimaryGame) {
                    continue;
                }
                const auto placed = engine.apply(window, *context_->displayGroup, policy);
                if (placed.status == windowing::WindowPlacementStatus::Rejected) {
                    error = placed.diagnostics.empty()
                        ? "owned game window placement was rejected"
                        : placed.diagnostics.front();
                    return false;
                }
                if (placed.restoreState.valid) {
                    context_->windowRestoreStates.push_back(placed.restoreState);
                }
            }
        }
        active_ = true;
        error.clear();
        return true;
    }
    bool verifyActive(std::string& error) override {
        std::lock_guard lock(context_->mutex);
        if (!active_ || !context_->displayGroup || context_->displayGroup->degraded) {
            error = "Seat display group is not in a verified active state";
            return false;
        }
        error.clear();
        return true;
    }
    bool rollback(std::string& error) noexcept override {
        std::lock_guard lock(context_->mutex);
        bool success = true;
        std::string firstError;
        if (context_->windowTracker) {
            windowing::WindowPlacementEngine engine(*context_->windowTracker);
            for (auto it = context_->windowRestoreStates.rbegin();
                 it != context_->windowRestoreStates.rend(); ++it) {
                std::string local;
                if (!engine.rollback(*it, &local)) {
                    success = false;
                    if (firstError.empty()) firstError = std::move(local);
                }
            }
        }
        context_->windowRestoreStates.clear();
        context_->displayGroup.reset();
        active_ = false;
        error = std::move(firstError);
        return success;
    }
    bool verifySafe(std::string& error) noexcept override {
        std::lock_guard lock(context_->mutex);
        if (active_ || !context_->windowRestoreStates.empty()) {
            error = "display/window restoration state remains active";
            return false;
        }
        error.clear();
        return true;
    }
    bool active() const noexcept override { return active_; }

private:
    std::shared_ptr<SeatResourceContext> context_;
    bool prepared_{false};
    bool active_{false};
};

class ControllerResource final : public launch::ISeatActivationResource {
public:
    ControllerResource(std::shared_ptr<SeatResourceContext> context,
                       std::shared_ptr<controller::SourceBackend> backend)
        : context_(std::move(context)), backend_(std::move(backend)) {}

    launch::ResourceKind kind() const noexcept override {
        return launch::ResourceKind::Controller;
    }
    bool prepare(const launch::SeatActivationPlan& plan,
                 const runtime::SeatGameBinding& binding,
                 std::string& error) override {
        if (!samePlan(plan, context_->plan) || binding.gameId != plan.target.gameId ||
            plan.seat.controllerIds.size() != 1u || !backend_) {
            error = "production controller path requires exactly one configured controller";
            return false;
        }
        const auto& id = plan.seat.controllerIds.front();
        controller::SeatBindingRequest request;
        request.seatId = plan.seatId;
        if (id.starts_with(L"directinput:")) {
            request.api = controller::ApiSurface::DirectInput;
            request.persistentControllerId = id;
        } else if (id.starts_with(L"xinput-slot:")) {
            error = "runtime XInput slot hints cannot be persisted as Seat controller identity";
            return false;
        } else {
            request.api = controller::ApiSurface::XInput;
            request.persistentControllerId = id;
        }
        request_ = std::move(request);
        prepared_ = true;
        error.clear();
        return true;
    }
    bool activate(std::string& error) override {
        if (!prepared_ || !request_) {
            error = "controller resource was not prepared";
            return false;
        }
        auto worker = std::make_shared<controller::PollWorker>(backend_);
        auto runtime = std::make_unique<controller::SeatControllerRuntime>(worker, backend_);
        const std::array<controller::SeatBindingRequest, 1> requests{*request_};
        if (!runtime->configure(requests, &error)) return false;
        std::lock_guard lock(context_->mutex);
        context_->controllerWorker = std::move(worker);
        context_->controllerRuntime = std::move(runtime);
        active_ = true;
        error.clear();
        return true;
    }
    bool verifyActive(std::string& error) override {
        std::lock_guard lock(context_->mutex);
        if (!active_ || !context_->controllerRuntime ||
            !context_->controllerRuntime->binding(context_->plan.seatId)) {
            error = "Seat controller binding is not verified";
            return false;
        }
        error.clear();
        return true;
    }
    bool rollback(std::string& error) noexcept override {
        std::lock_guard lock(context_->mutex);
        if (context_->controllerWorker) context_->controllerWorker->stop();
        context_->controllerRuntime.reset();
        context_->controllerWorker.reset();
        active_ = false;
        error.clear();
        return true;
    }
    bool verifySafe(std::string& error) noexcept override {
        std::lock_guard lock(context_->mutex);
        if (active_ || context_->controllerRuntime || context_->controllerWorker) {
            error = "Seat controller runtime remains owned after rollback";
            return false;
        }
        error.clear();
        return true;
    }
    bool active() const noexcept override { return active_; }

private:
    std::shared_ptr<SeatResourceContext> context_;
    std::shared_ptr<controller::SourceBackend> backend_;
    std::optional<controller::SeatBindingRequest> request_;
    bool prepared_{false};
    bool active_{false};
};

class AudioResource final : public launch::ISeatActivationResource {
public:
    AudioResource(std::shared_ptr<SeatResourceContext> context,
                  std::shared_ptr<audio::EndpointSource> endpoints,
                  std::shared_ptr<audio::SessionSource> sessions,
                  std::shared_ptr<audio::RouteBackend> backend)
        : context_(std::move(context)), endpointSource_(std::move(endpoints)),
          sessionSource_(std::move(sessions)), backend_(std::move(backend)) {}

    launch::ResourceKind kind() const noexcept override {
        return launch::ResourceKind::Audio;
    }
    bool prepare(const launch::SeatActivationPlan& plan,
                 const runtime::SeatGameBinding& binding,
                 std::string& error) override {
        if (!samePlan(plan, context_->plan) || binding.gameId != plan.target.gameId ||
            !plan.seat.audioOutputEndpointId || !endpointSource_ ||
            !sessionSource_ || !backend_) {
            error = "production audio resource is missing endpoint/session/backend state";
            return false;
        }
        prepared_ = true;
        error.clear();
        return true;
    }
    bool activate(std::string& error) override {
        if (!prepared_) {
            error = "audio resource was not prepared";
            return false;
        }
        process::ProcessTreeSnapshot tree;
        {
            std::lock_guard lock(context_->mutex);
            if (!context_->processGroup) {
                error = "audio routing requires an active exact process tree";
                return false;
            }
            const auto handoff = context_->processGroup->handoffSnapshot();
            if (handoff.seatId != context_->plan.seatId || !handoff.active()) {
                error = "audio routing requires a verified Seat process handoff";
                return false;
            }
            tree = context_->processGroup->snapshot();
        }
        auto endpointInventory = std::make_shared<audio::EndpointInventory>(endpointSource_);
        if (!endpointInventory->refresh(&error) || !endpointInventory->current()) return false;
        auto sessionInventory = std::make_shared<audio::SessionInventory>(sessionSource_);
        audio::RouteRequest request;
        request.seatId = context_->plan.seatId;
        request.processTree = std::move(tree);
        request.targetEndpointId = *context_->plan.seat.audioOutputEndpointId;
        audio::RouteTransaction transaction(std::move(request), sessionInventory, backend_);
        auto status = transaction.attempt(*endpointInventory->current(), &error);
        if (status.phase == audio::RoutePhase::Unsupported ||
            status.phase == audio::RoutePhase::Failed ||
            status.phase == audio::RoutePhase::RecoveryRequired) {
            return false;
        }
        std::lock_guard lock(context_->mutex);
        context_->audioTransaction.emplace(std::move(transaction));
        error.clear();
        return true;
    }
    bool verifyActive(std::string& error) override {
        const auto deadline = std::chrono::steady_clock::now() + kAudioReadyTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::lock_guard lock(context_->mutex);
            if (!context_->audioTransaction || !context_->processGroup) {
                error = "audio route transaction or process ownership is absent";
                return false;
            }
            const auto handoff = context_->processGroup->handoffSnapshot();
            if (handoff.seatId != context_->plan.seatId || !handoff.active()) {
                error = "audio route lost verified Seat process handoff";
                return false;
            }
            audio::EndpointInventory endpoints(endpointSource_);
            if (!endpoints.refresh(&error) || !endpoints.current()) return false;
            const auto status = context_->audioTransaction->attempt(*endpoints.current(), &error);
            if (status.phase == audio::RoutePhase::Satisfied ||
                status.phase == audio::RoutePhase::Applied) {
                error.clear();
                return true;
            }
            if (status.phase == audio::RoutePhase::Unsupported ||
                status.phase == audio::RoutePhase::Failed ||
                status.phase == audio::RoutePhase::RecoveryRequired) {
                return false;
            }
            std::this_thread::sleep_for(kPollInterval);
        }
        error = "owned audio session did not reach the selected endpoint before bounded timeout";
        return false;
    }
    bool rollback(std::string& error) noexcept override {
        std::lock_guard lock(context_->mutex);
        if (!context_->audioTransaction) {
            error.clear();
            return true;
        }
        audio::EndpointInventory endpoints(endpointSource_);
        if (!endpoints.refresh(&error) || !endpoints.current()) return false;
        const auto status = context_->audioTransaction->rollback(*endpoints.current(), &error);
        if (status.phase == audio::RoutePhase::RecoveryRequired || status.mutated) return false;
        context_->audioTransaction.reset();
        error.clear();
        return true;
    }
    bool verifySafe(std::string& error) noexcept override {
        std::lock_guard lock(context_->mutex);
        if (context_->audioTransaction && context_->audioTransaction->status().mutated) {
            error = "audio route mutation remains owned after rollback";
            return false;
        }
        error.clear();
        return true;
    }
    bool active() const noexcept override {
        std::lock_guard lock(context_->mutex);
        if (!context_->audioTransaction) return false;
        const auto phase = context_->audioTransaction->status().phase;
        return phase == audio::RoutePhase::Satisfied || phase == audio::RoutePhase::Applied;
    }

private:
    std::shared_ptr<SeatResourceContext> context_;
    std::shared_ptr<audio::EndpointSource> endpointSource_;
    std::shared_ptr<audio::SessionSource> sessionSource_;
    std::shared_ptr<audio::RouteBackend> backend_;
    bool prepared_{false};
};

class ProductionSeatActivationResourceFactory final
    : public IProductionSeatActivationResourceFactory {
public:
    explicit ProductionSeatActivationResourceFactory(ProductionLaunchServices services)
        : services_(std::move(services)) {
        if (!services_.controllerBackend) {
            services_.controllerBackend = controller::makeNativeControllerSourceBackend();
        }
        if (!services_.endpointSource) {
            services_.endpointSource = audio::makeNativeEndpointSource();
        }
        if (!services_.sessionSource) {
            services_.sessionSource = audio::makeNativeSessionSource();
        }
        if (!services_.audioRouteBackend) {
            services_.audioRouteBackend = std::make_shared<audio::ObserveOnlyRouteBackend>();
        }
    }

    bool bindActivationEpoch(const launch::SeatActivationPlan& plan,
                             const ProductionActivationEpoch& epoch,
                             std::string& error) override {
        if (plan.seatId == 0 || plan.fingerprint == 0 || !epoch.valid() ||
            epoch.seatId != plan.seatId ||
            epoch.activationFingerprint != plan.fingerprint) {
            error = "production activation epoch does not match immutable Seat plan";
            return false;
        }

        const auto key = std::make_pair(plan.seatId, plan.fingerprint);
        std::lock_guard lock(mutex_);
        for (auto iterator = contexts_.begin(); iterator != contexts_.end();) {
            if (iterator->first.first == plan.seatId && iterator->first != key) {
                if (iterator->second && iterator->second->activationAuthority) {
                    iterator->second->activationAuthority->invalidateProcess();
                }
                iterator = contexts_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto iterator = activationEpochs_.begin();
             iterator != activationEpochs_.end();) {
            if (iterator->first.first == plan.seatId && iterator->first != key) {
                iterator = activationEpochs_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        if (const auto existing = activationEpochs_.find(key);
            existing != activationEpochs_.end() && existing->second != epoch) {
            if (const auto context = contexts_.find(key); context != contexts_.end()) {
                if (context->second && context->second->activationAuthority) {
                    context->second->activationAuthority->invalidateProcess();
                }
                contexts_.erase(context);
            }
        }
        activationEpochs_[key] = epoch;
        error.clear();
        return true;
    }

    bool bindTrustedHandoffExecutables(
        const launch::SeatActivationPlan& plan,
        std::vector<std::wstring> executablePaths,
        std::string& error) override {
        if (plan.seatId == 0 || plan.fingerprint == 0 ||
            executablePaths.empty() ||
            executablePaths.size() > process::kMaximumTrustedHandoffExecutables ||
            std::find(executablePaths.begin(), executablePaths.end(),
                      plan.target.process.executablePath) == executablePaths.end()) {
            error = "trusted handoff executable evidence does not match the immutable launch target";
            return false;
        }
        const auto key = std::make_pair(plan.seatId, plan.fingerprint);
        std::lock_guard lock(mutex_);
        for (auto iterator = handoffExecutables_.begin();
             iterator != handoffExecutables_.end();) {
            if (iterator->first.first == plan.seatId && iterator->first != key) {
                iterator = handoffExecutables_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        handoffExecutables_[key] = std::move(executablePaths);
        if (const auto found = contexts_.find(key);
            found != contexts_.end() && found->second) {
            std::lock_guard contextLock(found->second->mutex);
            found->second->trustedHandoffExecutables = handoffExecutables_[key];
        }
        error.clear();
        return true;
    }

    std::unique_ptr<launch::ISeatActivationResource> create(
        launch::ResourceKind kind, const launch::SeatActivationPlan& plan,
        std::string& error) override {
        if (plan.seatId == 0 || plan.fingerprint == 0) {
            error = "production resource factory requires an immutable Seat plan";
            return {};
        }
        const auto context = contextFor(plan, error);
        if (!context || !context->activationAuthority) return {};
        ProductionActivationContextHandle bridgeContext = context->activationAuthority;

        if (kind == launch::ResourceKind::Recovery) {
            if (!services_.recoveryBridge) {
                error = "verified production recovery activation bridge is not installed";
                return {};
            }
            auto resource = services_.recoveryBridge->createRecoveryResource(
                plan, std::move(bridgeContext), error);
            if (!resource || resource->kind() != launch::ResourceKind::Recovery) {
                context->activationAuthority->invalidateProcess();
                if (error.empty()) {
                    error = "recovery activation bridge returned an invalid resource";
                }
                return {};
            }
            return resource;
        }
        if (kind == launch::ResourceKind::Input) {
            if (!services_.inputBridge) {
                error = "verified production Gate-C input activation bridge is not installed";
                return {};
            }
            auto resource = services_.inputBridge->createInputResource(
                plan, std::move(bridgeContext), error);
            if (!resource || resource->kind() != launch::ResourceKind::Input) {
                context->activationAuthority->invalidateProcess();
                if (error.empty()) error = "input activation bridge returned an invalid resource";
                return {};
            }
            return resource;
        }

        switch (kind) {
            case launch::ResourceKind::Recovery:
                break;
            case launch::ResourceKind::Process:
                return std::make_unique<ProcessResource>(context);
            case launch::ResourceKind::Window:
                return std::make_unique<WindowResource>(context);
            case launch::ResourceKind::Display:
                return std::make_unique<DisplayResource>(context);
            case launch::ResourceKind::Controller:
                return std::make_unique<ControllerResource>(context,
                                                            services_.controllerBackend);
            case launch::ResourceKind::Audio:
                return std::make_unique<AudioResource>(context, services_.endpointSource,
                                                       services_.sessionSource,
                                                       services_.audioRouteBackend);
            case launch::ResourceKind::Input:
                break;
        }
        error = "unknown production Seat activation resource";
        return {};
    }

private:
    std::shared_ptr<SeatResourceContext> contextFor(
        const launch::SeatActivationPlan& plan,
        std::string& error) {
        std::lock_guard lock(mutex_);
        const auto key = std::make_pair(plan.seatId, plan.fingerprint);
        const auto epoch = activationEpochs_.find(key);
        if (epoch == activationEpochs_.end() || !epoch->second.valid()) {
            error = "production activation epoch is not bound to this immutable Seat plan";
            return {};
        }
        if (const auto found = contexts_.find(key); found != contexts_.end()) {
            if (found->second && found->second->activationAuthority &&
                found->second->activationAuthority->reusableFor(epoch->second)) {
                error.clear();
                return found->second;
            }
            contexts_.erase(found);
        }
        auto created = std::make_shared<SeatResourceContext>(plan, epoch->second);
        if (const auto evidence = handoffExecutables_.find(key);
            evidence != handoffExecutables_.end()) {
            created->trustedHandoffExecutables = evidence->second;
        }
        contexts_[key] = created;
        error.clear();
        return created;
    }

    ProductionLaunchServices services_;
    std::mutex mutex_;
    std::map<std::pair<SeatId, std::uint64_t>, ProductionActivationEpoch>
        activationEpochs_;
    std::map<std::pair<SeatId, std::uint64_t>, std::vector<std::wstring>>
        handoffExecutables_;
    std::map<std::pair<SeatId, std::uint64_t>,
             std::shared_ptr<SeatResourceContext>> contexts_;
};

ProviderPlanInstallResult resultWith(
    ProviderPlanInstallCode code, ProviderPlanRegistrySnapshot snapshot,
    std::string diagnostic) {
    ProviderPlanInstallResult result;
    result.code = code;
    result.registry = std::move(snapshot);
    result.diagnostic = std::move(diagnostic);
    if (result.diagnostic.size() > 2048u) result.diagnostic.resize(2048u);
    return result;
}

} // namespace

std::uint64_t providerPlanFingerprint(
    const plan::ProviderAwareLaunchPlan& value) noexcept {
    StableHash hash;
    hash.u32(value.schemaVersion);
    std::vector<plan::SeatProviderLaunchPlan> seats = value.seats;
    std::sort(seats.begin(), seats.end(), [](const auto& left, const auto& right) {
        return left.seatId < right.seatId;
    });
    hash.u64(static_cast<std::uint64_t>(seats.size()));
    for (const auto& seat : seats) {
        hash.u32(seat.seatId);
        hash.text(seat.playerId);
        hash.text(seat.gameId);
        hash.boolean(seat.setupId.has_value());
        if (seat.setupId) hash.text(*seat.setupId);
        hash.u32(seat.instanceIndex);
        hash.u64(seat.requirementRevision);
        hashCompatibility(hash, seat.compatibility);
        hashRecipe(hash, seat.instanceRecipe);
        hash.u64(seat.hardwareFingerprint);
        hashRequirements(hash, seat.requirements);
        hashCapabilities(hash, seat.capabilities);
        const auto& request = seat.launchRequest;
        hash.text(request.providerId);
        hash.text(request.gameId);
        hash.boolean(request.providerAppId.has_value());
        if (request.providerAppId) hash.text(*request.providerAppId);
        hash.boolean(request.accountRef.has_value());
        if (request.accountRef) hash.text(*request.accountRef);
        hash.u64(request.metadataRevision);
        hash.u32(static_cast<std::uint32_t>(request.targetKind));
        hash.wide(request.target);
        hash.u64(static_cast<std::uint64_t>(request.arguments.size()));
        for (const auto& argument : request.arguments) hash.wide(argument);
        hash.boolean(request.workingDirectory.has_value());
        if (request.workingDirectory) hash.wide(*request.workingDirectory);
        hash.text(request.launchCorrelationId);
    }
    return hash.value();
}

std::uint64_t providerPlanRevision(
    const plan::SeatProviderLaunchPlan& seat) noexcept {
    StableHash hash;
    hash.u32(kProviderPlanInstallSchemaVersion);
    hash.u32(seat.seatId);
    hash.u64(seat.requirementRevision);
    hash.u64(seat.launchRequest.metadataRevision);
    hash.boolean(seat.compatibility.has_value());
    if (seat.compatibility) hash.u32(seat.compatibility->evidenceRevision);
    hash.boolean(seat.setupId.has_value());
    if (seat.setupId) hash.text(*seat.setupId);
    hash.u32(seat.instanceIndex);
    const auto value = hash.value();
    return value == 0 ? 1u : value;
}

std::uint64_t seatHardwareFingerprint(const SeatConfig& seat) noexcept {
    StableHash hash;
    hash.u32(seat.seatId);
    hashSortedWideRange(hash, seat.displayIds);
    hash.boolean(seat.primaryDisplayId.has_value());
    if (seat.primaryDisplayId) hash.wide(*seat.primaryDisplayId);
    hashSortedWideRange(hash, seat.keyboardIds);
    hashSortedWideRange(hash, seat.mouseIds);
    hashSortedWideRange(hash, seat.controllerIds);
    hash.boolean(seat.audioOutputEndpointId.has_value());
    if (seat.audioOutputEndpointId) hash.wide(*seat.audioOutputEndpointId);
    hash.boolean(seat.audioInputEndpointId.has_value());
    if (seat.audioInputEndpointId) hash.wide(*seat.audioInputEndpointId);
    return hash.value();
}

std::shared_ptr<IProductionSeatActivationResourceFactory>
makeProductionSeatActivationResourceFactory(ProductionLaunchServices services) {
    return std::make_shared<ProductionSeatActivationResourceFactory>(std::move(services));
}

HostProviderPlanRegistry::HostProviderPlanRegistry(
    std::shared_ptr<IProductionSeatActivationResourceFactory> resources,
    std::shared_ptr<requirement::ITrustedRequirementSource> trustedRequirements,
    std::shared_ptr<materialization::ITrustedMaterializationDecisionSource>
        trustedMaterializations,
    std::filesystem::path materializationInstancesRoot)
    : resources_(std::move(resources)),
      trustedRequirements_(std::move(trustedRequirements)),
      trustedMaterializations_(std::move(trustedMaterializations)),
      materializationInstancesRoot_(std::move(materializationInstancesRoot)) {
    if (!resources_) {
        throw std::invalid_argument(
            "production activation resource factory must be provided explicitly");
    }
    if (trustedRequirements_) {
        if (!trustedMaterializations_) {
            trustedMaterializations_ =
                trustedRequirements_->trustedMaterializationDecisionSource();
        }
        if (materializationInstancesRoot_.empty()) {
            materializationInstancesRoot_ =
                trustedRequirements_->trustedMaterializationInstancesRoot();
        }
    }
}

HostProviderPlanRegistry::HostProviderPlanRegistry(
    ProductionLaunchServices services,
    std::shared_ptr<requirement::ITrustedRequirementSource> trustedRequirements,
    std::shared_ptr<materialization::ITrustedMaterializationDecisionSource>
        trustedMaterializations,
    std::filesystem::path materializationInstancesRoot)
    : HostProviderPlanRegistry(
          makeProductionSeatActivationResourceFactory(std::move(services)),
          std::move(trustedRequirements), std::move(trustedMaterializations),
          std::move(materializationInstancesRoot)) {}

void HostProviderPlanRegistry::resetContext(
    std::uint64_t profileFingerprint,
    const runtime::RuntimeSessionId& sessionId,
    std::uint64_t sessionGeneration,
    std::span<const SeatConfig> configuredSeats) {
    std::lock_guard lock(mutex_);
    profileFingerprint_ = profileFingerprint;
    sessionId_ = sessionId;
    sessionGeneration_ = sessionGeneration;
    configuredSeats_.assign(configuredSeats.begin(), configuredSeats.end());
    std::sort(configuredSeats_.begin(), configuredSeats_.end(),
              [](const auto& left, const auto& right) {
                  return left.seatId < right.seatId;
              });
    plans_.clear();
    ++registryRevision_;
    if (registryRevision_ == 0) ++registryRevision_;
}

ProviderPlanRegistrySnapshot HostProviderPlanRegistry::snapshotLocked() const {
    ProviderPlanRegistrySnapshot snapshot;
    snapshot.registryRevision = registryRevision_;
    snapshot.profileFingerprint = profileFingerprint_;
    snapshot.sessionId = sessionId_;
    snapshot.sessionGeneration = sessionGeneration_;
    snapshot.entries.reserve(plans_.size());
    for (const auto& plan : plans_) snapshot.entries.push_back(plan.entry);
    std::sort(snapshot.entries.begin(), snapshot.entries.end(),
              [](const auto& left, const auto& right) {
                  return left.seatId < right.seatId;
              });
    return snapshot;
}

ProviderPlanRegistrySnapshot HostProviderPlanRegistry::registrySnapshot() const {
    std::lock_guard lock(mutex_);
    return snapshotLocked();
}

ProviderPlanInstallResult HostProviderPlanRegistry::install(
    const ProviderPlanInstallRequest& request) {
    requirement::TrustedRequirementSnapshot trustedSnapshot;
    requirement::RequirementSnapshotDiagnostic resolved;
    if (!trustedRequirements_) {
        std::lock_guard lock(mutex_);
        return resultWith(
            ProviderPlanInstallCode::InvalidPlan, snapshotLocked(),
            "trusted runtime requirement source is unavailable; provider plan installation is denied");
    }
    try {
        resolved = trustedRequirements_->resolveCurrent(trustedSnapshot);
    } catch (const std::exception& exception) {
        std::lock_guard lock(mutex_);
        return resultWith(
            ProviderPlanInstallCode::InvalidPlan, snapshotLocked(),
            std::string("trusted runtime requirement resolution threw: ") + exception.what());
    } catch (...) {
        std::lock_guard lock(mutex_);
        return resultWith(
            ProviderPlanInstallCode::InvalidPlan, snapshotLocked(),
            "trusted runtime requirement resolution failed unexpectedly");
    }
    if (!resolved.succeeded()) {
        std::string diagnostic = "trusted runtime requirement resolution failed (code=";
        diagnostic += std::to_string(static_cast<unsigned int>(resolved.code));
        diagnostic += ')';
        if (!resolved.message.empty()) diagnostic += ": " + resolved.message;
        std::lock_guard lock(mutex_);
        return resultWith(ProviderPlanInstallCode::InvalidPlan, snapshotLocked(),
                          std::move(diagnostic));
    }
    const auto trustedPlan =
        requirement::validateProviderAwareLaunchPlanAgainstTrustedRequirements(
            request.plan, trustedSnapshot, true);
    if (!trustedPlan.succeeded()) {
        std::string diagnostic = "trusted runtime requirement rejected provider plan (code=";
        diagnostic += std::to_string(static_cast<unsigned int>(trustedPlan.code));
        diagnostic += ')';
        if (!trustedPlan.message.empty()) diagnostic += ": " + trustedPlan.message;
        std::lock_guard lock(mutex_);
        return resultWith(ProviderPlanInstallCode::InvalidPlan, snapshotLocked(),
                          std::move(diagnostic));
    }

    std::lock_guard lock(mutex_);
    auto fail = [&](ProviderPlanInstallCode code, std::string diagnostic) {
        return resultWith(code, snapshotLocked(), std::move(diagnostic));
    };
    if (request.schemaVersion != kProviderPlanInstallSchemaVersion ||
        request.plan.schemaVersion != plan::kProviderLaunchPlanSchemaVersion) {
        return fail(ProviderPlanInstallCode::InvalidVersion,
                    "unsupported provider plan install/schema version");
    }
    if (request.expectedRegistryRevision != registryRevision_) {
        return fail(ProviderPlanInstallCode::StaleRegistryRevision,
                    "provider plan registry revision changed; resnapshot required");
    }
    if (request.profileFingerprint == 0 ||
        request.profileFingerprint != profileFingerprint_) {
        return fail(ProviderPlanInstallCode::InvalidProfile,
                    "provider plan profile fingerprint does not match host authority");
    }
    if (request.sessionId.empty() || request.sessionId != sessionId_ ||
        request.sessionGeneration == 0 ||
        request.sessionGeneration != sessionGeneration_) {
        return fail(ProviderPlanInstallCode::InvalidSession,
                    "provider plan session identity/generation is stale or foreign");
    }
    if (request.seatId == 0 || request.seatGameGeneration == 0) {
        return fail(ProviderPlanInstallCode::InvalidSeat,
                    "provider plan Seat/generation is invalid");
    }
    if (request.plan.seats.empty() ||
        request.plan.seats.size() > runtime::kV1MaximumActiveSeats) {
        return fail(ProviderPlanInstallCode::InvalidSeat,
                    "provider plan must contain one or two unique v1 Seats");
    }
    std::vector<SeatId> planSeatIds;
    for (const auto& seat : request.plan.seats) {
        if (seat.seatId == 0) {
            return fail(ProviderPlanInstallCode::InvalidSeat,
                        "provider plan contains a zero Seat identity");
        }
        planSeatIds.push_back(seat.seatId);
    }
    std::sort(planSeatIds.begin(), planSeatIds.end());
    if (std::adjacent_find(planSeatIds.begin(), planSeatIds.end()) !=
        planSeatIds.end()) {
        return fail(ProviderPlanInstallCode::InvalidSeat,
                    "provider plan contains duplicate Seat identities");
    }
    const auto computedFingerprint = providerPlanFingerprint(request.plan);
    if (request.planFingerprint == 0 ||
        request.planFingerprint != request.plan.fingerprint ||
        request.planFingerprint != computedFingerprint) {
        return fail(ProviderPlanInstallCode::InvalidPlan,
                    "provider plan fingerprint failed host-side recomputation");
    }
    const auto* selected = findProviderSeat(request.plan, request.seatId);
    if (!selected) {
        return fail(ProviderPlanInstallCode::InvalidSeat,
                    "install Seat is absent from immutable provider plan");
    }
    if (request.planRevision == 0 ||
        request.planRevision != providerPlanRevision(*selected)) {
        return fail(ProviderPlanInstallCode::InvalidPlan,
                    "provider plan revision failed host-side recomputation");
    }
    for (const auto& planSeat : request.plan.seats) {
        std::string validationError;
        if (!validateProviderSeat(planSeat, validationError)) {
            return fail(ProviderPlanInstallCode::InvalidPlan,
                        std::move(validationError));
        }
        const auto* configured = findConfiguredSeat(configuredSeats_, planSeat.seatId);
        if (!configured || !configured->active) {
            return fail(ProviderPlanInstallCode::InvalidSeat,
                        "provider plan references a Seat outside the active host profile");
        }
        if (seatHardwareFingerprint(*configured) != planSeat.hardwareFingerprint) {
            return fail(ProviderPlanInstallCode::InvalidProfile,
                        "provider plan hardware fingerprint does not match host profile");
        }
    }
    if (selected->launchRequest.targetKind != provider::LaunchTargetKind::Executable) {
        return fail(ProviderPlanInstallCode::Unsupported,
                    "production v1 requires an exact executable target for Job Object ownership");
    }
    const auto* configured = findConfiguredSeat(configuredSeats_, request.seatId);
    if (!configured) {
        return fail(ProviderPlanInstallCode::InvalidSeat,
                    "install Seat is not configured in the host profile");
    }

    const auto existing = std::find_if(plans_.begin(), plans_.end(),
                                       [&](const StoredPlan& stored) {
                                           return stored.entry.seatId == request.seatId;
                                       });
    if (existing != plans_.end() &&
        existing->entry.planFingerprint == request.planFingerprint &&
        existing->entry.planRevision == request.planRevision &&
        existing->entry.seatGameGeneration == request.seatGameGeneration) {
        return fail(ProviderPlanInstallCode::AlreadySatisfied,
                    "exact immutable provider plan is already installed");
    }

    StoredPlan stored;
    stored.providerPlan = request.plan;
    stored.seatPlan = *selected;
    stored.seatConfig = *configured;
    stored.entry.seatId = request.seatId;
    stored.entry.planFingerprint = request.planFingerprint;
    stored.entry.planRevision = request.planRevision;
    stored.entry.profileFingerprint = request.profileFingerprint;
    stored.entry.sessionId = request.sessionId;
    stored.entry.sessionGeneration = request.sessionGeneration;
    stored.entry.seatGameGeneration = request.seatGameGeneration;
    stored.entry.playerId = selected->playerId;
    stored.entry.gameId = selected->gameId;

    ++registryRevision_;
    if (registryRevision_ == 0) ++registryRevision_;
    stored.entry.installedRevision = registryRevision_;
    if (existing == plans_.end()) plans_.push_back(std::move(stored));
    else *existing = std::move(stored);
    return fail(ProviderPlanInstallCode::Ok,
                "immutable provider plan installed for exact host activation epoch");
}

ProviderPlanInstallResult HostProviderPlanRegistry::remove(
    const ProviderPlanRemoveRequest& request) {
    std::lock_guard lock(mutex_);
    auto fail = [&](ProviderPlanInstallCode code, std::string diagnostic) {
        return resultWith(code, snapshotLocked(), std::move(diagnostic));
    };
    if (request.schemaVersion != kProviderPlanInstallSchemaVersion) {
        return fail(ProviderPlanInstallCode::InvalidVersion,
                    "unsupported provider plan removal version");
    }
    if (request.expectedRegistryRevision != registryRevision_) {
        return fail(ProviderPlanInstallCode::StaleRegistryRevision,
                    "provider plan registry revision changed; resnapshot required");
    }
    if (request.profileFingerprint != profileFingerprint_) {
        return fail(ProviderPlanInstallCode::InvalidProfile,
                    "provider plan removal has wrong profile fingerprint");
    }
    if (request.sessionId != sessionId_ ||
        request.sessionGeneration != sessionGeneration_) {
        return fail(ProviderPlanInstallCode::InvalidSession,
                    "provider plan removal has stale/foreign session identity");
    }
    const auto found = std::find_if(plans_.begin(), plans_.end(),
                                    [&](const StoredPlan& stored) {
                                        return stored.entry.seatId == request.seatId;
                                    });
    if (found == plans_.end()) {
        return fail(ProviderPlanInstallCode::AlreadySatisfied,
                    "Seat has no installed provider plan");
    }
    if (found->entry.planFingerprint != request.planFingerprint ||
        found->entry.planRevision != request.planRevision ||
        found->entry.seatGameGeneration != request.seatGameGeneration) {
        return fail(ProviderPlanInstallCode::ReplayRejected,
                    "provider plan removal does not identify the exact installed epoch");
    }
    plans_.erase(found);
    ++registryRevision_;
    if (registryRevision_ == 0) ++registryRevision_;
    return fail(ProviderPlanInstallCode::Ok,
                "exact immutable provider plan removed");
}

std::unique_ptr<runtime::ISeatGameInstance> HostProviderPlanRegistry::create(
    SeatId, std::string& error) {
    error = "production provider registry requires binding-aware instance creation";
    return {};
}

std::unique_ptr<runtime::ISeatGameInstance>
HostProviderPlanRegistry::createForBinding(
    SeatId seatId, const runtime::SeatGameBinding& binding,
    std::uint64_t expectedGeneration, std::string& error) {
    requirement::TrustedRequirementSnapshot trustedSnapshot;
    if (!trustedRequirements_) {
        error = "trusted runtime requirement source is unavailable before Seat activation";
        return {};
    }
    requirement::RequirementSnapshotDiagnostic resolved;
    try {
        resolved = trustedRequirements_->resolveCurrent(trustedSnapshot);
    } catch (const std::exception& exception) {
        error = std::string("trusted runtime requirement revalidation threw: ") +
                exception.what();
        return {};
    } catch (...) {
        error = "trusted runtime requirement revalidation failed unexpectedly";
        return {};
    }
    if (!resolved.succeeded()) {
        error = "trusted runtime requirement revalidation failed (code=";
        error += std::to_string(static_cast<unsigned int>(resolved.code));
        error += ')';
        if (!resolved.message.empty()) error += ": " + resolved.message;
        return {};
    }

    std::lock_guard lock(mutex_);
    const auto found = std::find_if(plans_.begin(), plans_.end(),
                                    [&](const StoredPlan& stored) {
                                        return stored.entry.seatId == seatId;
                                    });
    if (found == plans_.end()) {
        error = "no immutable provider plan is installed for this Seat";
        return {};
    }
    if (found->entry.profileFingerprint != profileFingerprint_ ||
        found->entry.sessionId != sessionId_ ||
        found->entry.sessionGeneration != sessionGeneration_) {
        error = "installed provider plan belongs to a stale host context";
        return {};
    }
    if (found->entry.seatGameGeneration != expectedGeneration) {
        error = "installed provider plan belongs to a stale Seat game generation";
        return {};
    }
    if (binding.playerId != found->entry.playerId ||
        binding.gameId != found->entry.gameId) {
        error = "temporary Seat binding does not match installed provider plan";
        return {};
    }
    const auto trustedPlan =
        requirement::validateProviderAwareLaunchPlanAgainstTrustedRequirements(
            found->providerPlan, trustedSnapshot, true);
    if (!trustedPlan.succeeded()) {
        error = "installed provider plan failed fresh trusted requirement validation (code=";
        error += std::to_string(static_cast<unsigned int>(trustedPlan.code));
        error += ')';
        if (!trustedPlan.message.empty()) error += ": " + trustedPlan.message;
        return {};
    }
    const auto* authority = findTrustedAuthority(trustedSnapshot, found->seatPlan);
    if (!authority || authority->executableCandidates.empty()) {
        error = "fresh trusted runtime authority has no executable handoff evidence";
        return {};
    }
    auto activation = makeActivationPlan(found->entry, found->seatPlan,
                                         found->seatConfig);
    if (activation.target.process.executablePath.empty()) {
        error = "installed provider plan has no executable target";
        return {};
    }

    std::optional<materialization::LocalMaterializationDecision> localDecision;
    if (found->seatPlan.setupId && trustedMaterializations_) {
        materialization::MaterializationDecisionQuery query;
        query.setupId = *found->seatPlan.setupId;
        query.instanceIndex = found->seatPlan.instanceIndex;
        query.gameId = found->seatPlan.gameId;
        query.providerId = found->seatPlan.launchRequest.providerId;
        query.providerAppId = found->seatPlan.launchRequest.providerAppId;
        query.providerMetadataRevision = found->seatPlan.launchRequest.metadataRevision;
        query.requirementRevision = found->seatPlan.requirementRevision;
        query.compatibility = found->seatPlan.compatibility;

        materialization::LocalMaterializationDecision decision;
        materialization::TrustedMaterializationDecisionDiagnostic decisionResult;
        try {
            decisionResult = trustedMaterializations_->resolveCurrent(query, decision);
        } catch (const std::exception& exception) {
            error = std::string("fresh local materialization decision resolution threw: ") +
                    exception.what();
            return {};
        } catch (...) {
            error = "fresh local materialization decision resolution failed unexpectedly";
            return {};
        }
        if (!decisionResult.succeeded()) {
            error = "fresh local materialization decision rejected activation: ";
            error += decisionResult.message;
            return {};
        }
        if (decisionResult.found()) localDecision = std::move(decision);
    }

    std::unique_ptr<launch::ISeatActivationLifecycleHook> compatibilityHook;
    if (localDecision) {
        if (materializationInstancesRoot_.empty() ||
            !materializationInstancesRoot_.is_absolute()) {
            error = "trusted local materialization decision exists but product instance root is unavailable";
            return {};
        }

        materialization::CompatibilityRecipe recipe;
        recipe.seatId = found->entry.seatId;
        recipe.gameId = found->seatPlan.gameId;
        recipe.providerId = found->seatPlan.launchRequest.providerId;
        recipe.providerAppId = found->seatPlan.launchRequest.providerAppId;
        recipe.providerMetadataRevision = found->seatPlan.launchRequest.metadataRevision;
        recipe.requirementRevision = found->seatPlan.requirementRevision;
        recipe.compatibility = found->seatPlan.compatibility;
        recipe.steps = localDecision->steps;

        materialization::MaterializationContext materializationContext;
        materializationContext.instancesRoot = materializationInstancesRoot_;
        materializationContext.sessionId = materializationSessionId(found->entry.sessionId);

        materialization::InstanceMaterializationPlan materializationPlan;
        const auto compiled = materialization::compileInstanceMaterializationPlan(
            recipe, found->providerPlan, trustedSnapshot, materializationContext,
            materializationPlan);
        if (!compiled.succeeded()) {
            error = "trusted local materialization decision failed exact compilation: ";
            error += compiled.message;
            return {};
        }

        materializationPlan.setupId = localDecision->setupId;
        materializationPlan.localDecisionId = localDecision->decisionId;
        materializationPlan.localDecisionRevision = localDecision->revision;
        materializationPlan.sessionId = materializationContext.sessionId;
        materializationPlan.sessionGeneration = found->entry.sessionGeneration;
        materializationPlan.seatGameGeneration = found->entry.seatGameGeneration;
        materializationPlan.activationFingerprint = activation.fingerprint;

        CompatibilityActivationIdentity compatibilityIdentity;
        const auto bound = bindCompatibilityActivationIdentity(
            materializationPlan, found->providerPlan,
            materializationContext.sessionId, compatibilityIdentity);
        if (!bound.succeeded()) {
            error = "compiled materialization could not bind to current activation epoch: ";
            error += bound.message;
            return {};
        }
        compatibilityHook = makeProductionCompatibilityActivation(
            std::move(materializationPlan), std::move(compatibilityIdentity));
    }

    ProductionActivationEpoch activationEpoch;
    activationEpoch.seatId = found->entry.seatId;
    activationEpoch.sessionId = found->entry.sessionId;
    activationEpoch.sessionGeneration = found->entry.sessionGeneration;
    activationEpoch.seatGameGeneration = found->entry.seatGameGeneration;
    activationEpoch.activationFingerprint = activation.fingerprint;

    std::string contextError;
    if (!resources_->bindActivationEpoch(activation, activationEpoch, contextError)) {
        error = "authoritative host activation epoch could not be bound to production resources";
        if (!contextError.empty()) error += ": " + contextError;
        return {};
    }
    if (!resources_->bindTrustedHandoffExecutables(
            activation, authority->executableCandidates, contextError)) {
        error = "trusted process evidence could not be bound to activation";
        if (!contextError.empty()) error += ": " + contextError;
        return {};
    }
    return std::make_unique<launch::PlannedSeatGameInstance>(
        std::move(activation), resources_, std::move(compatibilityHook));
}

std::string_view providerPlanInstallCodeName(
    ProviderPlanInstallCode code) noexcept {
    switch (code) {
        case ProviderPlanInstallCode::Ok: return "ok";
        case ProviderPlanInstallCode::AlreadySatisfied: return "already-satisfied";
        case ProviderPlanInstallCode::InvalidVersion: return "invalid-version";
        case ProviderPlanInstallCode::InvalidPlan: return "invalid-plan";
        case ProviderPlanInstallCode::InvalidProfile: return "invalid-profile";
        case ProviderPlanInstallCode::InvalidSession: return "invalid-session";
        case ProviderPlanInstallCode::InvalidSeat: return "invalid-seat";
        case ProviderPlanInstallCode::InvalidSeatGeneration: return "invalid-seat-generation";
        case ProviderPlanInstallCode::StaleRegistryRevision: return "stale-registry-revision";
        case ProviderPlanInstallCode::ReplayRejected: return "replay-rejected";
        case ProviderPlanInstallCode::SeatNotIdle: return "seat-not-idle";
        case ProviderPlanInstallCode::Unsupported: return "unsupported";
        case ProviderPlanInstallCode::BackendFailure: return "backend-failure";
    }
    return "unknown";
}

} // namespace hydra::production
