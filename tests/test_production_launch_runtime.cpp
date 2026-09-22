#include "hydra/production_launch_installer.hpp"
#include "hydra/production_launch_runtime.hpp"
#include "hydra/runtime_host.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace hydra;
using namespace hydra::launch;
using namespace hydra::production;
using namespace hydra::runtime;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

SeatConfig makeSeat(SeatId seatId) {
    SeatConfig seat;
    seat.seatId = seatId;
    seat.name = L"Production Seat " + std::to_wstring(seatId);
    seat.displayIds = {L"DISPLAY-" + std::to_wstring(seatId)};
    seat.primaryDisplayId = seat.displayIds.front();
    seat.keyboardIds = {L"KEYBOARD-" + std::to_wstring(seatId)};
    seat.mouseIds = {L"MOUSE-" + std::to_wstring(seatId)};
    seat.controllerIds = {L"CONTROLLER-" + std::to_wstring(seatId)};
    seat.audioOutputEndpointId = L"AUDIO-" + std::to_wstring(seatId);
    seat.active = true;
    return seat;
}

Requirements allRequirements() {
    Requirements value;
    value.display = true;
    value.keyboard = true;
    value.mouse = true;
    value.controller = true;
    value.audioOutput = true;
    value.windowOwnership = true;
    value.recovery = true;
    value.highRisk = false;
    return value;
}

Requirements processOnlyRequirements() {
    Requirements value;
    value.display = false;
    value.keyboard = false;
    value.mouse = false;
    value.controller = false;
    value.audioOutput = false;
    value.windowOwnership = false;
    value.recovery = false;
    value.highRisk = false;
    return value;
}

Requirements processAndRecoveryRequirements() {
    auto value = processOnlyRequirements();
    value.recovery = true;
    return value;
}

Requirements processAndInputRequirements() {
    auto value = processOnlyRequirements();
    value.keyboard = true;
    value.mouse = true;
    return value;
}

Requirements processRecoveryInputRequirements() {
    auto value = processAndInputRequirements();
    value.recovery = true;
    return value;
}

plan::SeatProviderLaunchPlan makeProviderSeat(
    const SeatConfig& seat, std::string playerId, std::string gameId,
    Requirements requirements, std::wstring executable,
    std::vector<std::wstring> arguments = {}) {
    plan::SeatProviderLaunchPlan result;
    result.seatId = seat.seatId;
    result.playerId = std::move(playerId);
    result.gameId = std::move(gameId);
    result.setupId = "production-test-setup";
    result.instanceIndex = seat.seatId - 1u;
    result.requirementRevision = 100u + seat.seatId;
    result.hardwareFingerprint = seatHardwareFingerprint(seat);
    result.requirements = requirements;
    result.capabilities.process = true;
    result.capabilities.window = requirements.windowOwnership;
    result.capabilities.display = requirements.display;
    result.capabilities.input = requirements.keyboard || requirements.mouse;
    result.capabilities.controller = requirements.controller;
    result.capabilities.audio = requirements.audioOutput;
    result.capabilities.recovery = requirements.recovery;
    result.launchRequest.providerId = "manual-executable";
    result.launchRequest.gameId = result.gameId;
    result.launchRequest.providerAppId = "production-test-app-" +
                                         std::to_string(seat.seatId);
    result.launchRequest.metadataRevision = 200u + seat.seatId;
    result.launchRequest.targetKind = provider::LaunchTargetKind::Executable;
    result.launchRequest.target = std::move(executable);
    result.launchRequest.arguments = std::move(arguments);
    result.launchRequest.launchCorrelationId =
        "production-test-plan-" + std::to_string(seat.seatId);
    return result;
}

plan::ProviderAwareLaunchPlan makePlan(
    std::vector<plan::SeatProviderLaunchPlan> seats) {
    plan::ProviderAwareLaunchPlan result;
    result.seats = std::move(seats);
    std::sort(result.seats.begin(), result.seats.end(),
              [](const auto& left, const auto& right) {
                  return left.seatId < right.seatId;
              });
    result.fingerprint = providerPlanFingerprint(result);
    return result;
}

class TestTrustedRequirementSource final
    : public requirement::ITrustedRequirementSource {
public:
    void setPlan(const plan::ProviderAwareLaunchPlan& value) {
        plan_ = value;
    }

    requirement::RequirementSnapshotDiagnostic resolveCurrent(
        requirement::TrustedRequirementSnapshot& output) override {
        requirement::TrustedRequirementSnapshot snapshot;
        snapshot.referenceMonth = "2026-08";
        snapshot.staleAfterMonths = 6u;
        snapshot.trust = requirement::LocalEvidenceTrust::PhysicalOnly;
        for (const auto& seat : plan_.seats) {
            plan::GameRuntimeRequirement runtimeRequirement;
            runtimeRequirement.gameId = seat.gameId;
            runtimeRequirement.revision = seat.requirementRevision;
            runtimeRequirement.validatedSeatCount = 2u;
            runtimeRequirement.requirements = seat.requirements;
            runtimeRequirement.capabilities = seat.capabilities;
            runtimeRequirement.highRiskApproved = false;
            runtimeRequirement.compatibility = seat.compatibility;

            requirement::TrustedGameRuntimeAuthority authority;
            authority.requirement = runtimeRequirement;
            authority.providerId = seat.launchRequest.providerId;
            authority.providerAppId = seat.launchRequest.providerAppId;
            authority.providerMetadataRevision = seat.launchRequest.metadataRevision;
            authority.executableCandidates = {seat.launchRequest.target};
            authority.evidenceResultId = "production-test-evidence-" + seat.gameId;
            authority.evidenceProvenanceId = "production-test-local";
            authority.evidenceProvenanceRevision = 1u;
            authority.evidenceTimestampBucket = "2026-08";
            authority.evidenceOrigin = compat::ResultOrigin::Physical;
            snapshot.authorities.push_back(std::move(authority));
            snapshot.requirements.push_back(std::move(runtimeRequirement));
        }
        output = std::move(snapshot);
        return {};
    }

private:
    plan::ProviderAwareLaunchPlan plan_;
};

class TestTrustedMaterializationDecisionSource final
    : public materialization::ITrustedMaterializationDecisionSource {
public:
    void setDecision(materialization::LocalMaterializationDecision decision) {
        decision_ = std::move(decision);
    }

    void clearDecision() { decision_.reset(); }

    materialization::TrustedMaterializationDecisionDiagnostic resolveCurrent(
        const materialization::MaterializationDecisionQuery& query,
        materialization::LocalMaterializationDecision& output) override {
        ++resolveCount;
        lastQuery = query;
        if (!decision_) {
            return {materialization::TrustedMaterializationDecisionCode::NotRequired,
                    "no local materialization decision for this test Seat"};
        }
        const auto& decision = *decision_;
        if (decision.setupId != query.setupId ||
            decision.instanceIndex != query.instanceIndex) {
            return {materialization::TrustedMaterializationDecisionCode::NotRequired,
                    "different setup/instance is unchanged"};
        }
        if (decision.origin !=
            materialization::LocalMaterializationDecisionOrigin::LocalApproved) {
            return {materialization::TrustedMaterializationDecisionCode::UntrustedOrigin,
                    "community/imported descriptor is not local mutation authority"};
        }
        if (decision.gameId != query.gameId ||
            decision.providerId != query.providerId ||
            decision.providerAppId != query.providerAppId ||
            decision.providerMetadataRevision != query.providerMetadataRevision ||
            decision.requirementRevision != query.requirementRevision ||
            decision.compatibility != query.compatibility) {
            return {materialization::TrustedMaterializationDecisionCode::IdentityMismatch,
                    "local materialization decision is stale"};
        }
        output = decision;
        return {};
    }

    std::size_t resolveCount{0u};
    std::optional<materialization::MaterializationDecisionQuery> lastQuery;

private:
    std::optional<materialization::LocalMaterializationDecision> decision_;
};

materialization::LocalMaterializationDecision makeMaterializationDecision(
    const plan::SeatProviderLaunchPlan& seat,
    materialization::LocalMaterializationDecisionOrigin origin =
        materialization::LocalMaterializationDecisionOrigin::LocalApproved) {
    materialization::LocalMaterializationDecision decision;
    decision.decisionId = "production-local-materialization";
    decision.revision = 9u;
    decision.origin = origin;
    decision.setupId = seat.setupId.value_or("missing-setup");
    decision.instanceIndex = seat.instanceIndex;
    decision.gameId = seat.gameId;
    decision.providerId = seat.launchRequest.providerId;
    decision.providerAppId = seat.launchRequest.providerAppId;
    decision.providerMetadataRevision = seat.launchRequest.metadataRevision;
    decision.requirementRevision = seat.requirementRevision;
    decision.compatibility = seat.compatibility;

    materialization::CompatibilityRecipeStep step;
    step.stepId = "copy-production-seat-config";
    step.phase = setup::RecipeExecutionPhase::PreSpawn;
    step.scope = setup::MutationScope::SeatWritableInstance;
    step.files.push_back({L"defaults/pre.ini", L"config/player.ini", 4096u});
    decision.steps.push_back(std::move(step));
    return decision;
}

const SeatGameState* findGame(const HostRuntimeSnapshot& snapshot,
                              SeatId seatId) {
    const auto found = std::find_if(snapshot.seatGames.begin(),
                                    snapshot.seatGames.end(),
                                    [seatId](const SeatGameState& state) {
                                        return state.seatId == seatId;
                                    });
    return found == snapshot.seatGames.end() ? nullptr : &*found;
}

const plan::SeatProviderLaunchPlan* findPlanSeat(
    const plan::ProviderAwareLaunchPlan& value, SeatId seatId) {
    const auto found = std::find_if(value.seats.begin(), value.seats.end(),
                                    [seatId](const auto& seat) {
                                        return seat.seatId == seatId;
                                    });
    return found == value.seats.end() ? nullptr : &*found;
}

ProviderPlanInstallRequest makeInstallRequest(
    RuntimeHost& host, const plan::ProviderAwareLaunchPlan& value,
    SeatId seatId) {
    const auto snapshot = host.snapshot();
    const auto registry = host.providerPlanRegistrySnapshot();
    const auto* game = findGame(snapshot, seatId);
    const auto* selected = findPlanSeat(value, seatId);
    ProviderPlanInstallRequest request;
    request.seatId = seatId;
    request.expectedRegistryRevision = registry.registryRevision;
    request.planFingerprint = value.fingerprint;
    request.planRevision = selected ? providerPlanRevision(*selected) : 1u;
    request.profileFingerprint = runtimeProfileFingerprint(
        snapshot.configuredSeats, snapshot.managementSeatId);
    request.sessionId = snapshot.sessionId;
    request.sessionGeneration = snapshot.generation;
    request.seatGameGeneration = game ? game->generation + 1u : 1u;
    request.plan = value;
    return request;
}

void activateHost(RuntimeHost& host, const std::vector<SeatConfig>& profile,
                  std::uint64_t correlationBase) {
    check(host.loadProfile(profile, 1u, correlationBase).succeeded(),
          "host loads production test profile");
    check(host.plan(correlationBase + 1u).succeeded(),
          "host creates production session");
    check(host.prepare(correlationBase + 2u).succeeded(),
          "host prepares production session");
    check(host.start(correlationBase + 3u).succeeded(),
          "host activates production session");
}

std::string resourceKey(SeatId seatId, ResourceKind kind) {
    return std::to_string(seatId) + ":" +
           std::string(resourceKindName(kind));
}

struct ContextBridgeState {
    mutable std::mutex mutex;
    std::map<SeatId, ProductionActivationContextHandle> contexts;
    std::map<std::string, ProductionActivationContextSnapshot> observations;
    std::optional<std::pair<SeatId, ResourceKind>> requireProcessDuringPrepare;

    bool rememberContext(SeatId seatId, ProductionActivationContextHandle context) {
        std::lock_guard lock(mutex);
        const auto found = contexts.find(seatId);
        if (found != contexts.end()) return found->second == context;
        contexts.emplace(seatId, std::move(context));
        return true;
    }

    ProductionActivationContextHandle contextFor(SeatId seatId) const {
        std::lock_guard lock(mutex);
        const auto found = contexts.find(seatId);
        return found == contexts.end() ? ProductionActivationContextHandle{} : found->second;
    }

    void observe(std::string key, ProductionActivationContextSnapshot snapshot) {
        std::lock_guard lock(mutex);
        observations[std::move(key)] = std::move(snapshot);
    }

    std::optional<ProductionActivationContextSnapshot> observation(
        std::string_view key) const {
        std::lock_guard lock(mutex);
        const auto found = observations.find(std::string(key));
        if (found == observations.end()) return std::nullopt;
        return found->second;
    }
};

class ContextBridgeResource final : public ISeatActivationResource {
public:
    ContextBridgeResource(SeatId seatId, ResourceKind kind,
                          ProductionActivationContextHandle context,
                          std::shared_ptr<ContextBridgeState> state)
        : seatId_(seatId), kind_(kind), context_(std::move(context)),
          state_(std::move(state)) {}

    ResourceKind kind() const noexcept override { return kind_; }

    bool prepare(const SeatActivationPlan& plan,
                 const SeatGameBinding& binding,
                 std::string& error) override {
        if (!context_ || plan.seatId != seatId_ ||
            binding.gameId != plan.target.gameId) {
            error = "context bridge received wrong immutable plan/binding";
            return false;
        }
        const auto snapshot = context_->snapshot();
        state_->observe("prepare:" + resourceKey(seatId_, kind_), snapshot);
        if (!snapshot.epoch.valid() || snapshot.epoch.seatId != seatId_ ||
            snapshot.epoch.activationFingerprint != plan.fingerprint ||
            !context_->validatesEpoch(snapshot.epoch)) {
            error = "context bridge received an invalid production activation epoch";
            return false;
        }
        if (state_->requireProcessDuringPrepare ==
            std::optional<std::pair<SeatId, ResourceKind>>{{seatId_, kind_}}) {
            if (!snapshot.process ||
                !context_->validatesCurrentProcess(*snapshot.process)) {
                error = "process-bound context is not yet published";
                return false;
            }
        } else if (snapshot.stage != ProductionActivationContextStage::PreProcess ||
                   snapshot.process || snapshot.handoffState) {
            error = "bridge prepare observed process authority before Process verification";
            return false;
        }
        prepared_ = true;
        error.clear();
        return true;
    }

    bool activate(std::string& error) override {
        if (!prepared_) {
            error = "context bridge resource activated before prepare";
            return false;
        }
        const auto snapshot = context_->snapshot();
        state_->observe("activate:" + resourceKey(seatId_, kind_), snapshot);
        if (kind_ == ResourceKind::Recovery) {
            if (snapshot.stage != ProductionActivationContextStage::PreProcess ||
                snapshot.process || snapshot.handoffState) {
                error = "Recovery consumed process authority before Process verification";
                return false;
            }
        } else if (kind_ == ResourceKind::Input) {
            if (snapshot.stage != ProductionActivationContextStage::ProcessActive ||
                !snapshot.process ||
                !context_->validatesCurrentProcess(*snapshot.process)) {
                error = "Input did not receive current exact Process authority";
                return false;
            }
        }
        active_ = true;
        error.clear();
        return true;
    }

    bool verifyActive(std::string& error) override {
        if (!active_) {
            error = "context bridge resource is not active";
            return false;
        }
        const auto snapshot = context_->snapshot();
        if (kind_ == ResourceKind::Recovery &&
            snapshot.stage != ProductionActivationContextStage::PreProcess) {
            error = "Recovery process authority changed before Process activation";
            return false;
        }
        if (kind_ == ResourceKind::Input &&
            (snapshot.stage != ProductionActivationContextStage::ProcessActive ||
             !snapshot.process ||
             !context_->validatesCurrentProcess(*snapshot.process))) {
            error = "Input exact process authority is no longer current";
            return false;
        }
        error.clear();
        return true;
    }

    bool rollback(std::string& error) noexcept override {
        try {
            state_->observe("rollback:" + resourceKey(seatId_, kind_),
                            context_->snapshot());
        } catch (...) {
        }
        active_ = false;
        error.clear();
        return true;
    }

    bool verifySafe(std::string& error) noexcept override {
        if (active_) {
            error = "context bridge resource remains active";
            return false;
        }
        error.clear();
        return true;
    }

    bool active() const noexcept override { return active_; }

private:
    SeatId seatId_{0};
    ResourceKind kind_{ResourceKind::Recovery};
    ProductionActivationContextHandle context_;
    std::shared_ptr<ContextBridgeState> state_;
    bool prepared_{false};
    bool active_{false};
};

class ContextRecordingBridge final : public IProductionRecoveryResourceBridge,
                                     public IProductionInputResourceBridge {
public:
    explicit ContextRecordingBridge(std::shared_ptr<ContextBridgeState> state)
        : state_(std::move(state)) {}

    std::unique_ptr<ISeatActivationResource> createRecoveryResource(
        const SeatActivationPlan& plan, ProductionActivationContextHandle context,
        std::string& error) override {
        return create(ResourceKind::Recovery, plan, std::move(context), error);
    }

    std::unique_ptr<ISeatActivationResource> createInputResource(
        const SeatActivationPlan& plan, ProductionActivationContextHandle context,
        std::string& error) override {
        return create(ResourceKind::Input, plan, std::move(context), error);
    }

private:
    std::unique_ptr<ISeatActivationResource> create(
        ResourceKind kind, const SeatActivationPlan& plan,
        ProductionActivationContextHandle context, std::string& error) {
        if (!context || !state_->rememberContext(plan.seatId, context)) {
            error = "production bridges did not receive one Seat-local activation authority";
            return {};
        }
        const auto snapshot = context->snapshot();
        if (snapshot.stage != ProductionActivationContextStage::PreProcess ||
            snapshot.process || !context->validatesEpoch(snapshot.epoch)) {
            error = "production bridge creation requires immutable PreProcess context";
            return {};
        }
        error.clear();
        return std::make_unique<ContextBridgeResource>(
            plan.seatId, kind, std::move(context), state_);
    }

    std::shared_ptr<ContextBridgeState> state_;
};

struct RecordingState {
    std::map<std::string, bool> active;
    std::vector<std::string> log;
    std::optional<std::pair<SeatId, ResourceKind>> failAfterMutation;
    std::optional<std::pair<SeatId, ResourceKind>> failRollback;
};

class RecordingResource final : public ISeatActivationResource {
public:
    RecordingResource(SeatId seatId, ResourceKind kind,
                      std::shared_ptr<RecordingState> state)
        : seatId_(seatId), kind_(kind), state_(std::move(state)) {}

    ResourceKind kind() const noexcept override { return kind_; }

    bool prepare(const SeatActivationPlan& plan,
                 const SeatGameBinding& binding,
                 std::string& error) override {
        state_->log.push_back("prepare:" + resourceKey(seatId_, kind_));
        if (plan.seatId != seatId_ || binding.gameId != plan.target.gameId) {
            error = "recording resource received wrong immutable plan/binding";
            return false;
        }
        prepared_ = true;
        error.clear();
        return true;
    }

    bool activate(std::string& error) override {
        state_->log.push_back("activate:" + resourceKey(seatId_, kind_));
        if (!prepared_) {
            error = "recording resource activated before prepare";
            return false;
        }
        state_->active[resourceKey(seatId_, kind_)] = true;
        if (state_->failAfterMutation &&
            state_->failAfterMutation->first == seatId_ &&
            state_->failAfterMutation->second == kind_) {
            error = "injected failure after resource mutation";
            return false;
        }
        error.clear();
        return true;
    }

    bool verifyActive(std::string& error) override {
        state_->log.push_back("verify-active:" + resourceKey(seatId_, kind_));
        if (!active()) {
            error = "recording resource is not active";
            return false;
        }
        error.clear();
        return true;
    }

    bool rollback(std::string& error) noexcept override {
        state_->log.push_back("rollback:" + resourceKey(seatId_, kind_));
        if (state_->failRollback && state_->failRollback->first == seatId_ &&
            state_->failRollback->second == kind_) {
            error = "injected cleanup uncertainty";
            return false;
        }
        state_->active[resourceKey(seatId_, kind_)] = false;
        error.clear();
        return true;
    }

    bool verifySafe(std::string& error) noexcept override {
        state_->log.push_back("verify-safe:" + resourceKey(seatId_, kind_));
        if (active()) {
            error = "recording resource remains active after rollback";
            return false;
        }
        error.clear();
        return true;
    }

    bool active() const noexcept override {
        const auto found = state_->active.find(resourceKey(seatId_, kind_));
        return found != state_->active.end() && found->second;
    }

private:
    SeatId seatId_{0};
    ResourceKind kind_{ResourceKind::Recovery};
    std::shared_ptr<RecordingState> state_;
    bool prepared_{false};
};

class RecordingFactory final : public IProductionSeatActivationResourceFactory {
public:
    explicit RecordingFactory(std::shared_ptr<RecordingState> state)
        : state_(std::move(state)) {}

    bool bindActivationEpoch(const SeatActivationPlan& plan,
                             const ProductionActivationEpoch& epoch,
                             std::string& error) override {
        if (!epoch.valid() || epoch.seatId != plan.seatId ||
            epoch.activationFingerprint != plan.fingerprint) {
            error = "recording factory received a mismatched activation epoch";
            return false;
        }
        error.clear();
        return true;
    }

    bool bindTrustedHandoffExecutables(
        const SeatActivationPlan& plan,
        std::vector<std::wstring> executablePaths,
        std::string& error) override {
        if (std::find(executablePaths.begin(), executablePaths.end(),
                      plan.target.process.executablePath) == executablePaths.end()) {
            error = "recording factory did not receive the launch executable";
            return false;
        }
        error.clear();
        return true;
    }

    std::unique_ptr<ISeatActivationResource> create(
        ResourceKind kind, const SeatActivationPlan& plan,
        std::string& error) override {
        error.clear();
        return std::make_unique<RecordingResource>(plan.seatId, kind, state_);
    }

private:
    std::shared_ptr<RecordingState> state_;
};

constexpr std::array<ResourceKind, 7> kAllResources{
    ResourceKind::Recovery,
    ResourceKind::Process,
    ResourceKind::Window,
    ResourceKind::Display,
    ResourceKind::Input,
    ResourceKind::Controller,
    ResourceKind::Audio,
};

bool allResourcesInState(const RecordingState& state, SeatId seatId,
                         bool expected) {
    return std::all_of(kAllResources.begin(), kAllResources.end(),
                       [&](ResourceKind kind) {
                           const auto found = state.active.find(resourceKey(seatId, kind));
                           const bool active = found != state.active.end() && found->second;
                           return active == expected;
                       });
}

std::optional<std::filesystem::path> findMaterializedPlayerConfig(
    const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error) return std::nullopt;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    for (; !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && !error &&
            iterator->path().filename() == L"player.ini") {
            return iterator->path();
        }
    }
    return std::nullopt;
}

void testProductionMaterializationAuthorityWiring() {
    static std::uint64_t serial = 0u;
    const auto root = std::filesystem::temp_directory_path() /
                      ("HydraSeatProductionMaterialization-" +
                       std::to_string(++serial));
    const auto sourceRoot = root / "source";
    const auto instancesRoot = root / "instances";
    std::error_code errorCode;
    std::filesystem::remove_all(root, errorCode);
    std::filesystem::create_directories(sourceRoot / "defaults", errorCode);
    check(!errorCode, "production materialization test source root is created");
    {
        std::ofstream executableOne(sourceRoot / "game-one.exe", std::ios::binary);
        executableOne << "fixture-executable-one";
        std::ofstream executableTwo(sourceRoot / "game-two.exe", std::ios::binary);
        executableTwo << "fixture-executable-two";
        std::ofstream defaults(sourceRoot / "defaults" / "pre.ini", std::ios::binary);
        defaults << "seat-config-v1\n";
    }

    const std::vector<SeatConfig> profile{makeSeat(1), makeSeat(2)};
    const auto providerPlan = makePlan({
        makeProviderSeat(profile[0], "materialization-player", "materialization-game",
                         allRequirements(), (sourceRoot / "game-one.exe").wstring()),
        makeProviderSeat(profile[1], "other-player", "other-game",
                         allRequirements(), (sourceRoot / "game-two.exe").wstring()),
    });
    const auto* seatPlan = findPlanSeat(providerPlan, 1u);
    check(seatPlan != nullptr, "materialization fixture contains Seat 1 provider authority");
    if (!seatPlan) {
        std::filesystem::remove_all(root, errorCode);
        return;
    }

    auto trusted = std::make_shared<TestTrustedRequirementSource>();
    trusted->setPlan(providerPlan);
    auto localAuthority =
        std::make_shared<TestTrustedMaterializationDecisionSource>();
    localAuthority->setDecision(makeMaterializationDecision(*seatPlan));
    auto recordingState = std::make_shared<RecordingState>();
    auto registry = std::make_shared<HostProviderPlanRegistry>(
        std::make_shared<RecordingFactory>(recordingState), trusted,
        localAuthority, instancesRoot);
    RuntimeHost host({}, registry);
    activateHost(host, profile, 5000u);

    auto install = makeInstallRequest(host, providerPlan, 1u);
    check(host.installProviderPlan(install).code == ProviderPlanInstallCode::Ok,
          "locally approved materialization plan installs against exact runtime authority");
    check(host.assignSeatGame(
              1u, {"materialization-player", "materialization-game"}, 5010u).succeeded(),
          "materialization Seat binds before production activation");
    const auto started = host.startSeatGame(1u, 5011u);
    check(started.succeeded(),
          "real PlannedSeatGameInstance runs the locally trusted compatibility lifecycle hook");
    check(localAuthority->resolveCount == 1u && localAuthority->lastQuery &&
              localAuthority->lastQuery->setupId == seatPlan->setupId.value_or("") &&
              localAuthority->lastQuery->requirementRevision == seatPlan->requirementRevision,
          "production runtime re-resolves exact setup/provider/requirement materialization authority at start");

    const auto materialized = findMaterializedPlayerConfig(instancesRoot);
    check(materialized.has_value(),
          "PreSpawn compatibility hook materializes only the product-owned Seat instance");
    if (materialized) {
        std::ifstream input(*materialized, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
        check(bytes == "seat-config-v1\n",
              "materialized Seat config bytes come from the exact trusted executable source root");
    }
    check(host.stopSeatGame(1u, 5012u).succeeded() &&
              !findMaterializedPlayerConfig(instancesRoot).has_value(),
          "production lifecycle rollback removes the exact writable instance materialization");

    const auto communityInstancesRoot = root / "community-instances";
    auto communityAuthority =
        std::make_shared<TestTrustedMaterializationDecisionSource>();
    communityAuthority->setDecision(makeMaterializationDecision(
        *seatPlan, materialization::LocalMaterializationDecisionOrigin::ImportedCommunity));
    auto communityState = std::make_shared<RecordingState>();
    auto communityRegistry = std::make_shared<HostProviderPlanRegistry>(
        std::make_shared<RecordingFactory>(communityState), trusted,
        communityAuthority, communityInstancesRoot);
    RuntimeHost communityHost({}, communityRegistry);
    activateHost(communityHost, profile, 5100u);
    auto communityInstall = makeInstallRequest(communityHost, providerPlan, 1u);
    check(communityHost.installProviderPlan(communityInstall).code == ProviderPlanInstallCode::Ok,
          "community-origin negative fixture still installs immutable provider authority");
    check(communityHost.assignSeatGame(
              1u, {"materialization-player", "materialization-game"}, 5110u).succeeded(),
          "community-origin negative fixture binds the Seat before the runtime trust gate");
    const auto communityStart = communityHost.startSeatGame(1u, 5111u);
    check(communityStart.code == SeatGameResultCode::BackendFailure &&
              communityAuthority->resolveCount == 1u &&
              !std::filesystem::exists(communityInstancesRoot),
          "community-only materialization authority fails before any filesystem mutation");
    check(allResourcesInState(*communityState, 1u, false),
          "community-only rejection happens before production resource activation");

    const auto unchangedInstancesRoot = root / "unchanged-instances";
    auto noDecisionAuthority =
        std::make_shared<TestTrustedMaterializationDecisionSource>();
    auto unchangedState = std::make_shared<RecordingState>();
    auto unchangedRegistry = std::make_shared<HostProviderPlanRegistry>(
        std::make_shared<RecordingFactory>(unchangedState), trusted,
        noDecisionAuthority, unchangedInstancesRoot);
    RuntimeHost unchangedHost({}, unchangedRegistry);
    activateHost(unchangedHost, profile, 5200u);
    auto unchangedInstall = makeInstallRequest(unchangedHost, providerPlan, 1u);
    check(unchangedHost.installProviderPlan(unchangedInstall).code == ProviderPlanInstallCode::Ok,
          "no-materialization fixture installs the same provider plan");
    check(unchangedHost.assignSeatGame(
              1u, {"materialization-player", "materialization-game"}, 5210u).succeeded() &&
              unchangedHost.startSeatGame(1u, 5211u).succeeded(),
          "plans with no local materialization decision retain the existing activation path");
    check(noDecisionAuthority->resolveCount == 1u &&
              !std::filesystem::exists(unchangedInstancesRoot),
          "no-materialization path performs no compatibility filesystem mutation");
    check(unchangedHost.stopSeatGame(1u, 5212u).succeeded(),
          "unchanged no-materialization activation remains normally stoppable");

    std::filesystem::remove_all(root, errorCode);
}

void testRegistryAndFailureIndexIsolation() {
    const std::vector<SeatConfig> profile{makeSeat(1), makeSeat(2)};
    auto state = std::make_shared<RecordingState>();
    auto trusted = std::make_shared<TestTrustedRequirementSource>();
    auto registry = std::make_shared<HostProviderPlanRegistry>(
        std::make_shared<RecordingFactory>(state), trusted);
    RuntimeHost host({}, registry);
    activateHost(host, profile, 1000u);

    const auto plan = makePlan({
        makeProviderSeat(profile[0], "player-one", "game-one",
                         allRequirements(), L"C:\\HydraSeat\\fake-one.exe"),
        makeProviderSeat(profile[1], "player-two", "game-two",
                         allRequirements(), L"C:\\HydraSeat\\fake-two.exe"),
    });
    trusted->setPlan(plan);

    auto firstRequest = makeInstallRequest(host, plan, 1);
    const auto firstInstall = host.installProviderPlan(firstRequest);
    check(firstInstall.code == ProviderPlanInstallCode::Ok,
          "Seat 1 immutable provider plan installs");

    const auto staleReplay = host.installProviderPlan(firstRequest);
    check(staleReplay.code == ProviderPlanInstallCode::StaleRegistryRevision,
          "stale registry revision rejects replayed provider plan install");

    auto idempotentRequest = makeInstallRequest(host, plan, 1);
    check(host.installProviderPlan(idempotentRequest).code ==
              ProviderPlanInstallCode::AlreadySatisfied,
          "exact resnapshotted provider plan is idempotent");

    auto secondRequest = makeInstallRequest(host, plan, 2);
    check(host.installProviderPlan(secondRequest).code == ProviderPlanInstallCode::Ok,
          "Seat 2 immutable provider plan installs independently");

    auto wrongProfile = makeInstallRequest(host, plan, 1);
    ++wrongProfile.profileFingerprint;
    check(host.installProviderPlan(wrongProfile).code ==
              ProviderPlanInstallCode::InvalidProfile,
          "wrong-profile provider plan is rejected");

    auto wrongSession = makeInstallRequest(host, plan, 1);
    ++wrongSession.sessionGeneration;
    check(host.installProviderPlan(wrongSession).code ==
              ProviderPlanInstallCode::InvalidSession,
          "wrong-session provider plan is rejected");

    auto wrongGeneration = makeInstallRequest(host, plan, 1);
    ++wrongGeneration.seatGameGeneration;
    check(host.installProviderPlan(wrongGeneration).code ==
              ProviderPlanInstallCode::InvalidSeatGeneration,
          "stale/future Seat generation is rejected");

    auto wrongFingerprint = makeInstallRequest(host, plan, 1);
    ++wrongFingerprint.planFingerprint;
    wrongFingerprint.plan.fingerprint = wrongFingerprint.planFingerprint;
    check(host.installProviderPlan(wrongFingerprint).code ==
              ProviderPlanInstallCode::InvalidPlan,
          "forged provider plan fingerprint is recomputed and rejected");

    auto thirdSeat = makeInstallRequest(host, plan, 1);
    thirdSeat.seatId = 3;
    thirdSeat.seatGameGeneration = 1;
    check(host.installProviderPlan(thirdSeat).code ==
              ProviderPlanInstallCode::InvalidSeat,
          "foreign third Seat target is rejected");

    auto threeSeatPlan = plan;
    const auto thirdConfig = makeSeat(3);
    threeSeatPlan.seats.push_back(
        makeProviderSeat(thirdConfig, "player-three", "game-three",
                         allRequirements(), L"C:\\HydraSeat\\fake-three.exe"));
    threeSeatPlan.fingerprint = providerPlanFingerprint(threeSeatPlan);
    trusted->setPlan(threeSeatPlan);
    auto threeSeatRequest = makeInstallRequest(host, threeSeatPlan, 1);
    check(host.installProviderPlan(threeSeatRequest).code ==
              ProviderPlanInstallCode::InvalidSeat,
          "provider plan carrying a third active v1 Seat is rejected");
    trusted->setPlan(plan);

    check(host.assignSeatGame(2, {"player-two", "game-two"}, 1100u).succeeded(),
          "Seat 2 assignment succeeds");
    check(host.startSeatGame(2, 1101u).succeeded(),
          "Seat 2 starts through installed plan");
    check(allResourcesInState(*state, 2, true),
          "Seat 2 owns all fake production resources after start");

    for (std::size_t index = 0; index < kAllResources.size(); ++index) {
        state->failAfterMutation = std::pair{SeatId{1}, kAllResources[index]};
        const auto assign = host.assignSeatGame(
            1, {"player-one", "game-one"}, 1200u + index * 2u);
        check(assign.succeeded(), "Seat 1 assignment succeeds before injected failure");
        const auto started = host.startSeatGame(1, 1201u + index * 2u);
        check(started.code == SeatGameResultCode::BackendFailure,
              "each production activation failure index returns BackendFailure after safe rollback");
        const auto snapshot = host.snapshot();
        const auto* seat1 = findGame(snapshot, 1);
        const auto* seat2 = findGame(snapshot, 2);
        check(seat1 && seat1->phase == SeatGamePhase::Idle && !seat1->binding,
              "failed Seat returns to authoritative Idle");
        check(seat2 && seat2->phase == SeatGamePhase::Playing,
              "other Seat remains Playing across every failure index");
        check(allResourcesInState(*state, 1, false),
              "failed Seat has zero retained fake resources after rollback");
        check(allResourcesInState(*state, 2, true),
              "other Seat resources are untouched by rollback");
    }

    state->failAfterMutation.reset();
    check(host.assignSeatGame(1, {"player-one", "game-one"}, 1400u).succeeded(),
          "Seat 1 reassigns after failure-index campaign");
    check(host.startSeatGame(1, 1401u).succeeded(),
          "Seat 1 starts after failure-index campaign");
    check(allResourcesInState(*state, 1, true) &&
              allResourcesInState(*state, 2, true),
          "both Seats own independent resources concurrently");

    auto replaceWhilePlaying = makeInstallRequest(host, plan, 2);
    check(host.installProviderPlan(replaceWhilePlaying).code ==
              ProviderPlanInstallCode::SeatNotIdle,
          "provider plan replacement is rejected while target Seat is Playing");

    state->failRollback = std::pair{SeatId{1}, ResourceKind::Process};
    const auto uncertainStop = host.stopSeatGame(1, 1500u);
    check(uncertainStop.code == SeatGameResultCode::RecoveryRequired,
          "unverifiable process cleanup makes the Seat recovery-required");
    const auto uncertainSnapshot = host.snapshot();
    const auto* uncertainSeat1 = findGame(uncertainSnapshot, 1);
    const auto* unaffectedSeat2 = findGame(uncertainSnapshot, 2);
    check(uncertainSeat1 && uncertainSeat1->phase == SeatGamePhase::RecoveryRequired,
          "cleanup uncertainty retains exact Seat-local recovery state");
    check(unaffectedSeat2 && unaffectedSeat2->phase == SeatGamePhase::Playing &&
              allResourcesInState(*state, 2, true),
          "Seat A cleanup uncertainty does not disturb Seat B");

    state->failRollback.reset();
    check(host.stopSeatGame(1, 1501u).succeeded(),
          "retained Seat-local process ownership can be cleaned on recovery retry");
    check(host.stopSeatGame(2, 1502u).succeeded(), "Seat 2 stop succeeds");
    check(allResourcesInState(*state, 1, false) &&
              allResourcesInState(*state, 2, false),
          "both Seats verify zero retained fake resources after stop");
}

#if defined(_WIN32)

struct ProcessProbe {
    DWORD processId{0};
    std::uint64_t creationTime100ns{0};
};

std::uint64_t fileTimeValue(const FILETIME& value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32u) |
           static_cast<std::uint64_t>(value.dwLowDateTime);
}

std::optional<ProcessProbe> probeProcess(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                 FALSE, processId);
    if (process == nullptr) return std::nullopt;
    FILETIME creation{}, exit{}, kernel{}, user{};
    const BOOL read = GetProcessTimes(process, &creation, &exit, &kernel, &user);
    CloseHandle(process);
    if (read == FALSE) return std::nullopt;
    return ProcessProbe{processId, fileTimeValue(creation)};
}

bool sameProcessStillRunning(const ProcessProbe& probe) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                 FALSE, probe.processId);
    if (process == nullptr) return false;
    FILETIME creation{}, exit{}, kernel{}, user{};
    const BOOL read = GetProcessTimes(process, &creation, &exit, &kernel, &user);
    const DWORD wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return read != FALSE && fileTimeValue(creation) == probe.creationTime100ns &&
           wait == WAIT_TIMEOUT;
}

bool terminateExactProcess(const ProcessProbe& probe) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE |
                                     SYNCHRONIZE,
                                 FALSE, probe.processId);
    if (process == nullptr) return false;
    FILETIME creation{}, exit{}, kernel{}, user{};
    const bool exact = GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE &&
                       fileTimeValue(creation) == probe.creationTime100ns &&
                       WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    const bool terminated = exact && TerminateProcess(process, 0x48594354u) != FALSE;
    if (terminated) (void)WaitForSingleObject(process, 3000u);
    CloseHandle(process);
    return terminated;
}

std::vector<DWORD> readPids(const std::filesystem::path& path) {
    std::vector<DWORD> result;
    std::ifstream input(path);
    std::uint64_t value = 0;
    while (input >> value) {
        if (value > 0 && value <= static_cast<std::uint64_t>(MAXDWORD)) {
            result.push_back(static_cast<DWORD>(value));
        }
    }
    return result;
}

std::vector<DWORD> waitForPids(const std::filesystem::path& path,
                               std::size_t minimumCount,
                               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto pids = readPids(path);
        if (pids.size() >= minimumCount) return pids;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return readPids(path);
}

bool waitForNoExactProcesses(std::span<const ProcessProbe> probes,
                             std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::none_of(probes.begin(), probes.end(), sameProcessStillRunning)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::none_of(probes.begin(), probes.end(), sameProcessStillRunning);
}

bool fixturePidRunning(DWORD processId) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}

bool waitForFixturePidsToExit(std::span<const DWORD> processIds,
                              std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::none_of(processIds.begin(), processIds.end(), fixturePidRunning)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::none_of(processIds.begin(), processIds.end(), fixturePidRunning);
}

std::filesystem::path temporaryPidFile(std::string_view suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("hydraseat-production-" + std::string(suffix) + "-" +
            std::to_string(stamp) + ".pids");
}

std::vector<std::wstring> processFixtureArguments(
    const std::filesystem::path& pidFile) {
    return {L"--depth", L"1", L"--sleep-ms", L"30000",
            L"--descendant-sleep-ms", L"30000", L"--pid-file",
            pidFile.wstring()};
}

std::vector<std::wstring> processHandoffFixtureArguments(
    const std::filesystem::path& pidFile) {
    return {L"--depth", L"2", L"--sleep-ms", L"30000",
            L"--exit-after-spawn", L"--pid-file", pidFile.wstring()};
}

void testLiveProductionIpcInstallerAndOrphanZero(
    const std::filesystem::path& fixture) {
    const std::vector<SeatConfig> profile{makeSeat(1), makeSeat(2)};
    auto trusted = std::make_shared<TestTrustedRequirementSource>();
    auto registry = std::make_shared<HostProviderPlanRegistry>(
        ProductionLaunchServices{}, trusted);
    RuntimeHost host({}, registry);
    hostipc::HostControlServer server(host);

    const auto initialRegistry = host.providerPlanRegistrySnapshot();
    check(initialRegistry.profileFingerprint == 0u,
          "HostControlServer registers production registry before profile load");

    activateHost(host, profile, 2000u);
    check(host.providerPlanRegistrySnapshot().profileFingerprint ==
              runtimeProfileFingerprint(profile, 1u),
          "production registry is rebound to exact active profile/session");

    std::string serverError;
    bool serverResult = false;
    std::thread serverThread([&] {
        serverResult = server.serve(&serverError);
    });

    hostipc::HostControlClient seatClient;
    std::string error;
    bool connected = false;
    for (int attempt = 0; attempt < 100 && !connected; ++attempt) {
        connected = seatClient.connectForSeat(
            hostipc::ClientRole::SeatControl, 1u, 100u, &error);
        if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(connected, "SeatControl client connects to production host");
    if (!connected) {
        server.requestStop();
        serverThread.join();
        return;
    }

    const auto pidFile = temporaryPidFile("success");
    std::error_code ignored;
    std::filesystem::remove(pidFile, ignored);
    auto plan = makePlan({
        makeProviderSeat(profile[0], "player-one", "process-game-one",
                         processOnlyRequirements(), fixture.wstring(),
                         processFixtureArguments(pidFile)),
        makeProviderSeat(profile[1], "player-two", "process-game-two",
                         processOnlyRequirements(), fixture.wstring(),
                         {L"--depth", L"0", L"--sleep-ms", L"30000"}),
    });
    trusted->setPlan(plan);

    const auto registryBefore = seatClient.providerPlanRegistry(
        hostipc::kDefaultHostIpcTimeoutMs, &error);
    check(registryBefore.has_value(),
          "SeatControl can read its filtered provider-plan registry metadata");

    auto wrongSeatRequest = makeInstallRequest(host, plan, 2);
    std::optional<hostipc::ErrorPayload> protocolError;
    const auto wrongSeatInstall = seatClient.installProviderPlan(
        wrongSeatRequest, hostipc::kDefaultHostIpcTimeoutMs, &error, &protocolError);
    check(!wrongSeatInstall && protocolError &&
              protocolError->code == hostipc::ErrorCode::PermissionDenied,
          "SeatControl cannot install another Seat provider plan");

    HostProviderPlanInstaller installer(seatClient);
    const auto* selected = findPlanSeat(plan, 1);
    check(selected != nullptr && installer.install(1, plan, *selected, error),
          "production installer binds exact provider plan through host IPC");

    if (registryBefore) {
        auto stale = makeInstallRequest(host, plan, 1);
        stale.expectedRegistryRevision = registryBefore->registryRevision;
        protocolError.reset();
        const auto replay = seatClient.installProviderPlan(
            stale, hostipc::kDefaultHostIpcTimeoutMs, &error, &protocolError);
        check(replay && replay->code == ProviderPlanInstallCode::StaleRegistryRevision,
              "new-correlation replay with stale registry revision is rejected");
    }

    const runtime::SeatGameBinding binding{"player-one", "process-game-one"};
    const auto assigned = seatClient.seatGameCommand(
        hostipc::MessageType::AssignSeatGame,
        hostipc::SeatGameCommandPayload{1u, binding},
        hostipc::kDefaultHostIpcTimeoutMs, &error);
    check(assigned && assigned->succeeded(),
          "production IPC assigns exact temporary Seat binding");
    const auto started = seatClient.seatGameCommand(
        hostipc::MessageType::StartSeatGame,
        hostipc::SeatGameCommandPayload{1u, std::nullopt},
        hostipc::kDefaultHostIpcTimeoutMs, &error);
    check(started && started->succeeded(),
          "Game -> Seat -> Play reaches real ProcessLauncher ownership");

    const auto pids = waitForPids(pidFile, 2u, std::chrono::seconds(5));
    check(pids.size() >= 2u,
          "production ProcessLauncher fixture reports root and descendant PIDs");
    std::vector<ProcessProbe> probes;
    for (const DWORD pid : pids) {
        if (const auto probe = probeProcess(pid)) probes.push_back(*probe);
    }
    check(probes.size() >= 2u,
          "exact root/descendant process identities are observable before stop");

    const auto stopped = seatClient.seatGameCommand(
        hostipc::MessageType::StopSeatGame,
        hostipc::SeatGameCommandPayload{1u, std::nullopt},
        hostipc::kDefaultHostIpcTimeoutMs, &error);
    if (!(stopped && stopped->succeeded())) {
        std::cerr << "production stop diagnostic: "
                  << (stopped ? stopped->diagnostic : error) << '\n';
    }
    check(stopped && stopped->succeeded(),
          "production Seat stop succeeds through authoritative lifecycle");
    check(waitForNoExactProcesses(probes, std::chrono::seconds(5)),
          "production Seat stop proves orphan=0 for exact owned root/descendant instances");
    check(installer.rollback(1u, error),
          "production installer removes exact idle Seat plan after stop");

    const auto recoveryPidFile = temporaryPidFile("missing-recovery");
    std::filesystem::remove(recoveryPidFile, ignored);
    auto recoveryPlan = makePlan({
        makeProviderSeat(profile[0], "player-one", "recovery-required-game",
                         processAndRecoveryRequirements(), fixture.wstring(),
                         processFixtureArguments(recoveryPidFile)),
        makeProviderSeat(profile[1], "player-two", "process-game-two",
                         processOnlyRequirements(), fixture.wstring(),
                         {L"--depth", L"0", L"--sleep-ms", L"30000"}),
    });
    trusted->setPlan(recoveryPlan);
    const auto* recoverySeat = findPlanSeat(recoveryPlan, 1);
    check(recoverySeat && installer.install(1u, recoveryPlan, *recoverySeat, error),
          "recovery-required provider plan can be installed immutably");
    const auto recoveryAssigned = seatClient.seatGameCommand(
        hostipc::MessageType::AssignSeatGame,
        hostipc::SeatGameCommandPayload{
            1u, runtime::SeatGameBinding{"player-one", "recovery-required-game"}},
        hostipc::kDefaultHostIpcTimeoutMs, &error);
    check(recoveryAssigned && recoveryAssigned->succeeded(),
          "recovery-required Seat binds before activation");
    const auto recoveryStart = seatClient.seatGameCommand(
        hostipc::MessageType::StartSeatGame,
        hostipc::SeatGameCommandPayload{1u, std::nullopt},
        hostipc::kDefaultHostIpcTimeoutMs, &error);
    check(recoveryStart && recoveryStart->code == SeatGameResultCode::BackendFailure,
          "missing verified recovery bridge rejects activation");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(readPids(recoveryPidFile).empty(),
          "unsupported recovery plan is rejected before process mutation");
    check(installer.rollback(1u, error),
          "failed recovery-required plan is removable from Idle Seat");

    const auto inputPidFile = temporaryPidFile("missing-input");
    std::filesystem::remove(inputPidFile, ignored);
    auto inputPlan = makePlan({
        makeProviderSeat(profile[0], "player-one", "input-required-game",
                         processAndInputRequirements(), fixture.wstring(),
                         processFixtureArguments(inputPidFile)),
        makeProviderSeat(profile[1], "player-two", "process-game-two",
                         processOnlyRequirements(), fixture.wstring(),
                         {L"--depth", L"0", L"--sleep-ms", L"30000"}),
    });
    trusted->setPlan(inputPlan);
    const auto* inputSeat = findPlanSeat(inputPlan, 1);
    check(inputSeat && installer.install(1u, inputPlan, *inputSeat, error),
          "input-required provider plan installs immutably");
    const auto inputAssigned = seatClient.seatGameCommand(
        hostipc::MessageType::AssignSeatGame,
        hostipc::SeatGameCommandPayload{
            1u, runtime::SeatGameBinding{"player-one", "input-required-game"}},
        hostipc::kDefaultHostIpcTimeoutMs, &error);
    check(inputAssigned && inputAssigned->succeeded(),
          "input-required Seat binds before activation");
    const auto inputStart = seatClient.seatGameCommand(
        hostipc::MessageType::StartSeatGame,
        hostipc::SeatGameCommandPayload{1u, std::nullopt},
        hostipc::kDefaultHostIpcTimeoutMs, &error);
    check(inputStart && inputStart->code == SeatGameResultCode::BackendFailure,
          "missing Gate-C input bridge rejects activation");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(readPids(inputPidFile).empty(),
          "missing input bridge fails during resource creation before process activation");
    check(installer.rollback(1u, error),
          "failed input-required plan is removable from Idle Seat");

    hostipc::HostControlClient readOnly;
    check(readOnly.connect(hostipc::ClientRole::ReadOnly, 1000u, &error),
          "read-only client connects for permission check");
    protocolError.reset();
    const auto hiddenRegistry = readOnly.providerPlanRegistry(
        hostipc::kDefaultHostIpcTimeoutMs, &error, &protocolError);
    check(!hiddenRegistry && protocolError &&
              protocolError->code == hostipc::ErrorCode::PermissionDenied,
          "read-only clients cannot inspect temporary player/game plan bindings");
    readOnly.close();

    seatClient.close();
    server.requestStop();
    serverThread.join();
    check(serverResult, serverError.empty()
                            ? "production HostControlServer exits cleanly"
                            : serverError);

    std::filesystem::remove(pidFile, ignored);
    std::filesystem::remove(recoveryPidFile, ignored);
    std::filesystem::remove(inputPidFile, ignored);
}

void testLiveProductionTwoSeatHandoff(const std::filesystem::path& fixture) {
    const std::vector<SeatConfig> profile{makeSeat(1), makeSeat(2)};
    auto trusted = std::make_shared<TestTrustedRequirementSource>();
    auto registry = std::make_shared<HostProviderPlanRegistry>(
        ProductionLaunchServices{}, trusted);
    RuntimeHost host({}, registry);
    activateHost(host, profile, 3000u);

    const auto seatOnePidFile = temporaryPidFile("handoff-seat-one");
    const auto seatTwoPidFile = temporaryPidFile("handoff-seat-two");
    std::error_code ignored;
    std::filesystem::remove(seatOnePidFile, ignored);
    std::filesystem::remove(seatTwoPidFile, ignored);

    auto plan = makePlan({
        makeProviderSeat(profile[0], "player-one", "handoff-game-one",
                         processOnlyRequirements(), fixture.wstring(),
                         processHandoffFixtureArguments(seatOnePidFile)),
        makeProviderSeat(profile[1], "player-two", "handoff-game-two",
                         processOnlyRequirements(), fixture.wstring(),
                         {L"--depth", L"0", L"--sleep-ms", L"30000",
                          L"--pid-file", seatTwoPidFile.wstring()}),
    });
    trusted->setPlan(plan);

    auto seatOneInstall = makeInstallRequest(host, plan, 1u);
    check(host.installProviderPlan(seatOneInstall).code == ProviderPlanInstallCode::Ok,
          "Seat 1 production handoff plan installs against exact profile/session generation");
    auto seatTwoInstall = makeInstallRequest(host, plan, 2u);
    check(host.installProviderPlan(seatTwoInstall).code == ProviderPlanInstallCode::Ok,
          "Seat 2 independent production plan installs after registry resnapshot");

    check(host.assignSeatGame(2u, {"player-two", "handoff-game-two"}, 3100u).succeeded() &&
              host.startSeatGame(2u, 3101u).succeeded(),
          "Seat 2 starts before Seat 1 handoff campaign");
    const auto seatTwoPids = waitForPids(seatTwoPidFile, 1u, std::chrono::seconds(3));
    std::optional<ProcessProbe> seatTwoProbe;
    if (!seatTwoPids.empty()) seatTwoProbe = probeProcess(seatTwoPids.front());
    check(seatTwoProbe && sameProcessStillRunning(*seatTwoProbe),
          "Seat 2 exact process identity is live before Seat 1 handoff");

    const auto seatOneAssigned =
        host.assignSeatGame(1u, {"player-one", "handoff-game-one"}, 3102u);
    const auto seatOneStarted = seatOneAssigned.succeeded()
        ? host.startSeatGame(1u, 3103u)
        : SeatGameCommandResult{SeatGameResultCode::BackendFailure, {}, false,
                                "Seat 1 assignment failed: " + seatOneAssigned.diagnostic};
    check(seatOneAssigned.succeeded() && seatOneStarted.succeeded(),
          std::string("Seat 1 starts through authoritative production ProcessResource: ") +
              seatOneStarted.diagnostic);
    const auto seatOnePids = waitForPids(seatOnePidFile, 3u, std::chrono::seconds(4));
    check(seatOnePids.size() >= 3u,
          "production fixture creates launcher, loader, and final game processes");

    std::vector<DWORD> launcherPids;
    if (seatOnePids.size() >= 3u) {
        launcherPids.assign(seatOnePids.begin(), seatOnePids.begin() + 2);
    }
    check(launcherPids.size() == 2u &&
              waitForFixturePidsToExit(launcherPids, std::chrono::seconds(3)),
          "launcher and loader really exit before runtime reconciliation");

    std::optional<ProcessProbe> finalGameProbe;
    if (seatOnePids.size() >= 3u) finalGameProbe = probeProcess(seatOnePids.back());
    check(finalGameProbe && sameProcessStillRunning(*finalGameProbe),
          "final descendant game remains live after launcher/loader exit");

    const auto reconciled = host.reconcileSeatGames(3104u);
    check(reconciled.succeeded(),
          "authoritative runtime consumes exact descendant handoff instead of treating launcher exit as game exit");
    const auto playingSnapshot = host.snapshot();
    const auto* seatOne = findGame(playingSnapshot, 1u);
    const auto* seatTwo = findGame(playingSnapshot, 2u);
    check(seatOne && seatOne->phase == SeatGamePhase::Playing && seatOne->binding &&
              seatOne->binding->playerId == "player-one" &&
              seatOne->binding->gameId == "handoff-game-one",
          "Seat 1 preserves exact temporary player/game identity across process handoff");
    check(seatTwo && seatTwo->phase == SeatGamePhase::Playing && seatTwo->binding &&
              seatTwo->binding->playerId == "player-two" &&
              seatTwo->binding->gameId == "handoff-game-two",
          "Seat 2 remains independently Playing during Seat 1 handoff");

    check(host.stopSeatGame(1u, 3105u).succeeded(),
          "Seat 1 stop cleans the post-handoff owned Job tree");
    if (finalGameProbe) {
        check(waitForNoExactProcesses(std::span<const ProcessProbe>(&*finalGameProbe, 1u),
                                      std::chrono::seconds(4)),
              "post-handoff Seat 1 cleanup proves final exact game orphan=0");
    }
    check(seatTwoProbe && sameProcessStillRunning(*seatTwoProbe),
          "Seat 1 handoff cleanup does not terminate Seat 2's same-executable process");
    check(host.stopSeatGame(2u, 3106u).succeeded(),
          "Seat 2 remains independently stoppable after Seat 1 cleanup");
    if (seatTwoProbe) {
        check(waitForNoExactProcesses(std::span<const ProcessProbe>(&*seatTwoProbe, 1u),
                                      std::chrono::seconds(4)),
              "Seat 2 exact process also reaches orphan=0");
    }

    std::filesystem::remove(seatOnePidFile, ignored);
    std::filesystem::remove(seatTwoPidFile, ignored);
}

void testLiveProductionActivationContextAuthority(
    const std::filesystem::path& fixture) {
    const std::vector<SeatConfig> profile{makeSeat(1), makeSeat(2)};
    auto trusted = std::make_shared<TestTrustedRequirementSource>();
    auto bridgeState = std::make_shared<ContextBridgeState>();
    auto bridge = std::make_shared<ContextRecordingBridge>(bridgeState);
    ProductionLaunchServices services;
    services.recoveryBridge = bridge;
    services.inputBridge = bridge;
    auto registry = std::make_shared<HostProviderPlanRegistry>(services, trusted);
    RuntimeHost host({}, registry);
    activateHost(host, profile, 4000u);

    const auto seatOnePidFile = temporaryPidFile("context-seat-one");
    const auto seatTwoPidFile = temporaryPidFile("context-seat-two");
    std::error_code ignored;
    std::filesystem::remove(seatOnePidFile, ignored);
    std::filesystem::remove(seatTwoPidFile, ignored);

    auto plan = makePlan({
        makeProviderSeat(profile[0], "context-player-one", "context-game-one",
                         processRecoveryInputRequirements(), fixture.wstring(),
                         processFixtureArguments(seatOnePidFile)),
        makeProviderSeat(profile[1], "context-player-two", "context-game-two",
                         processRecoveryInputRequirements(), fixture.wstring(),
                         {L"--depth", L"0", L"--sleep-ms", L"30000",
                          L"--pid-file", seatTwoPidFile.wstring()}),
    });
    trusted->setPlan(plan);

    const auto seatOneInstall = makeInstallRequest(host, plan, 1u);
    check(host.installProviderPlan(seatOneInstall).code == ProviderPlanInstallCode::Ok,
          "activation-context Seat 1 plan installs against exact host epoch");
    const auto seatTwoInstall = makeInstallRequest(host, plan, 2u);
    check(host.installProviderPlan(seatTwoInstall).code == ProviderPlanInstallCode::Ok,
          "activation-context Seat 2 plan installs independently");

    check(host.assignSeatGame(1u, {"context-player-one", "context-game-one"},
                              4100u).succeeded(),
          "activation-context Seat 1 assignment succeeds");
    const auto seatOneStart = host.startSeatGame(1u, 4101u);
    check(seatOneStart.succeeded(),
          "activation-context Seat 1 starts with staged Recovery/Process/Input resources");

    const auto recoveryPrepare = bridgeState->observation("prepare:1:recovery");
    const auto recoveryActivate = bridgeState->observation("activate:1:recovery");
    const auto inputPrepare = bridgeState->observation("prepare:1:input");
    const auto inputActivate = bridgeState->observation("activate:1:input");
    check(recoveryPrepare &&
              recoveryPrepare->stage == ProductionActivationContextStage::PreProcess &&
              !recoveryPrepare->process && !recoveryPrepare->handoffState,
          "pre-process Recovery context has no process identity or handoff claim");
    check(inputPrepare &&
              inputPrepare->stage == ProductionActivationContextStage::PreProcess &&
              !inputPrepare->process && !inputPrepare->handoffState,
          "Input prepare cannot see process authority before Process verification");
    check(recoveryActivate &&
              recoveryActivate->stage == ProductionActivationContextStage::PreProcess &&
              !recoveryActivate->process,
          "Recovery activation remains explicitly pre-process");
    check(recoveryPrepare && recoveryPrepare->epoch.seatId == 1u &&
              recoveryPrepare->epoch.sessionId == seatOneInstall.sessionId &&
              recoveryPrepare->epoch.sessionGeneration ==
                  seatOneInstall.sessionGeneration &&
              recoveryPrepare->epoch.seatGameGeneration ==
                  seatOneInstall.seatGameGeneration &&
              recoveryPrepare->epoch.activationFingerprint != 0,
          "exact Seat/session/session-generation/Seat-game-generation exist before Process");
    check(inputActivate &&
              inputActivate->stage == ProductionActivationContextStage::ProcessActive &&
              inputActivate->process && inputActivate->process->valid(),
          "Process verification publishes exact process context before later Input activation");

    const auto seatOneContext = bridgeState->contextFor(1u);
    check(seatOneContext != nullptr,
          "Seat 1 bridges retain a read-only activation authority handle");
    auto seatOnePids = waitForPids(seatOnePidFile, 2u, std::chrono::seconds(4));
    check(seatOnePids.size() >= 2u,
          "activation-context fixture reports Seat 1 root and child PIDs");
    std::optional<ProcessProbe> seatOneRoot;
    std::optional<ProcessProbe> seatOneChild;
    if (seatOnePids.size() >= 2u) {
        seatOneRoot = probeProcess(seatOnePids[0]);
        seatOneChild = probeProcess(seatOnePids[1]);
    }

    ProductionActivationContextSnapshot seatOneActive;
    if (seatOneContext) seatOneActive = seatOneContext->snapshot();
    check(seatOneActive.stage == ProductionActivationContextStage::ProcessActive &&
              seatOneActive.process && seatOneRoot &&
              seatOneActive.process->authoritativeProcess.processId ==
                  seatOneRoot->processId &&
              seatOneActive.process->authoritativeProcess.creationTime100ns ==
                  seatOneRoot->creationTime100ns &&
              seatOneActive.process->handoffGeneration == 0u &&
              seatOneActive.process->handoffState ==
                  process::ProcessHandoffState::RootActive,
          "published process authority is exact root PID+creation identity");
    if (seatOneContext && seatOneActive.process) {
        check(seatOneContext->validatesCurrentProcess(*seatOneActive.process),
              "current exact process snapshot validates against internal ProcessGroup authority");

        auto wrongSeat = seatOneActive.process->epoch;
        wrongSeat.seatId = 2u;
        check(!seatOneContext->validatesEpoch(wrongSeat),
              "wrong Seat activation epoch is rejected");

        auto staleSession = seatOneActive.process->epoch;
        staleSession.sessionId.bytes[0] ^= 0x5au;
        check(!seatOneContext->validatesEpoch(staleSession),
              "stale/foreign Host session identity is rejected");
        staleSession = seatOneActive.process->epoch;
        ++staleSession.sessionGeneration;
        check(!seatOneContext->validatesEpoch(staleSession),
              "stale Host session generation is rejected");

        auto staleSeatGame = seatOneActive.process->epoch;
        ++staleSeatGame.seatGameGeneration;
        check(!seatOneContext->validatesEpoch(staleSeatGame),
              "stale Seat-game generation is rejected");

        auto reusedPid = *seatOneActive.process;
        reusedPid.authoritativeProcess.creationTime100ns ^= 1u;
        check(!seatOneContext->validatesCurrentProcess(reusedPid),
              "same PID with different creation time cannot validate as current process");
    }

    check(host.assignSeatGame(2u, {"context-player-two", "context-game-two"},
                              4102u).succeeded() &&
              host.startSeatGame(2u, 4103u).succeeded(),
          "Seat 2 starts with an isolated production activation context");
    const auto seatTwoContext = bridgeState->contextFor(2u);
    const auto seatTwoPids = waitForPids(seatTwoPidFile, 1u, std::chrono::seconds(3));
    std::optional<ProcessProbe> seatTwoProbe;
    if (!seatTwoPids.empty()) seatTwoProbe = probeProcess(seatTwoPids.front());
    ProductionActivationContextSnapshot seatTwoActive;
    if (seatTwoContext) seatTwoActive = seatTwoContext->snapshot();
    check(seatTwoContext && seatTwoContext != seatOneContext &&
              seatTwoActive.stage == ProductionActivationContextStage::ProcessActive &&
              seatTwoActive.process && seatTwoActive.process->epoch.seatId == 2u &&
              seatTwoProbe &&
              seatTwoActive.process->authoritativeProcess.processId ==
                  seatTwoProbe->processId &&
              seatTwoActive.process->authoritativeProcess.creationTime100ns ==
                  seatTwoProbe->creationTime100ns,
          "second Seat receives a separate exact process authority");

    std::optional<ProductionProcessActivatedContext> oldAuthority = seatOneActive.process;
    check(seatOneRoot && terminateExactProcess(*seatOneRoot),
          "controlled test terminates only the exact Seat 1 root instance");

    ProductionActivationContextSnapshot handedOff;
    bool observedHandoff = false;
    if (seatOneContext && oldAuthority) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto current = seatOneContext->snapshot();
            if (current.stage == ProductionActivationContextStage::ProcessActive &&
                current.process &&
                current.process->handoffGeneration > oldAuthority->handoffGeneration &&
                !current.process->authoritativeProcess.sameInstance(
                    oldAuthority->authoritativeProcess)) {
                handedOff = current;
                observedHandoff = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    check(observedHandoff && handedOff.process && seatOneChild &&
              handedOff.process->authoritativeProcess.processId ==
                  seatOneChild->processId &&
              handedOff.process->authoritativeProcess.creationTime100ns ==
                  seatOneChild->creationTime100ns &&
              handedOff.process->handoffState ==
                  process::ProcessHandoffState::DescendantActive,
          "handoff explicitly replaces authority with exact trusted descendant identity");
    if (seatOneContext && oldAuthority && handedOff.process) {
        check(!seatOneContext->validatesCurrentProcess(*oldAuthority),
              "old exact process authority is invalid immediately after handoff");
        check(seatOneContext->validatesCurrentProcess(*handedOff.process),
              "new handoff process authority validates explicitly");
    }
    if (seatTwoContext && seatTwoActive.process) {
        check(seatTwoContext->validatesCurrentProcess(*seatTwoActive.process) &&
                  seatTwoProbe && sameProcessStillRunning(*seatTwoProbe),
              "Seat 1 handoff does not mutate Seat 2 process authority");
    }

    check(host.stopSeatGame(1u, 4104u).succeeded(),
          "Seat 1 rollback/stop succeeds after process handoff");
    if (seatOneContext) {
        const auto stoppedContext = seatOneContext->snapshot();
        check(stoppedContext.stage ==
                  ProductionActivationContextStage::ProcessInvalidated &&
                  !stoppedContext.process && !stoppedContext.handoffState,
              "rollback clears consumer-visible process context before/through teardown");
        if (handedOff.process) {
            check(!seatOneContext->validatesCurrentProcess(*handedOff.process),
                  "stopped Seat cannot validate its former process authority");
        }
    }
    if (seatTwoContext && seatTwoActive.process) {
        check(seatTwoContext->validatesCurrentProcess(*seatTwoActive.process) &&
                  seatTwoProbe && sameProcessStillRunning(*seatTwoProbe),
              "Seat 2 remains active and isolated after Seat 1 rollback");
    }
    check(host.stopSeatGame(2u, 4105u).succeeded(),
          "Seat 2 stops independently after Seat 1 context invalidation");

    std::filesystem::remove(seatOnePidFile, ignored);
    std::filesystem::remove(seatTwoPidFile, ignored);

    // A bridge resource that insists on process authority during prepare must
    // fail before Process activation; absence is a typed state, not accepted
    // uncertainty.
    auto earlyState = std::make_shared<ContextBridgeState>();
    earlyState->requireProcessDuringPrepare =
        std::pair{SeatId{1}, ResourceKind::Input};
    auto earlyBridge = std::make_shared<ContextRecordingBridge>(earlyState);
    ProductionLaunchServices earlyServices;
    earlyServices.inputBridge = earlyBridge;
    auto earlyTrusted = std::make_shared<TestTrustedRequirementSource>();
    auto earlyRegistry = std::make_shared<HostProviderPlanRegistry>(
        earlyServices, earlyTrusted);
    RuntimeHost earlyHost({}, earlyRegistry);
    activateHost(earlyHost, profile, 4200u);
    const auto earlyPidFile = temporaryPidFile("context-too-early");
    std::filesystem::remove(earlyPidFile, ignored);
    auto earlyPlan = makePlan({
        makeProviderSeat(profile[0], "early-player", "early-game",
                         processAndInputRequirements(), fixture.wstring(),
                         processFixtureArguments(earlyPidFile)),
        makeProviderSeat(profile[1], "unused-player", "unused-game",
                         processOnlyRequirements(), fixture.wstring(),
                         {L"--depth", L"0", L"--sleep-ms", L"30000"}),
    });
    earlyTrusted->setPlan(earlyPlan);
    const auto earlyInstall = makeInstallRequest(earlyHost, earlyPlan, 1u);
    check(earlyHost.installProviderPlan(earlyInstall).code == ProviderPlanInstallCode::Ok,
          "early-consumer test installs exact Seat epoch");
    check(earlyHost.assignSeatGame(1u, {"early-player", "early-game"},
                                   4204u).succeeded(),
          "early-consumer test assigns Seat binding");
    const auto earlyStart = earlyHost.startSeatGame(1u, 4205u);
    check(earlyStart.code == SeatGameResultCode::BackendFailure,
          "process-bound bridge consumption during prepare fails closed");
    check(readPids(earlyPidFile).empty(),
          "too-early process consumer fails before ProcessLauncher mutation");
    const auto earlyContext = earlyState->contextFor(1u);
    if (earlyContext) {
        const auto snapshot = earlyContext->snapshot();
        check(snapshot.stage == ProductionActivationContextStage::ProcessInvalidated &&
                  !snapshot.process,
              "failed pre-activation transaction invalidates its retained context handle");
    }
    std::filesystem::remove(earlyPidFile, ignored);
}

#endif

} // namespace

int main(int argc, char** argv) {
    testProductionMaterializationAuthorityWiring();
    testRegistryAndFailureIndexIsolation();

#if defined(_WIN32)
    if (argc < 2) {
        std::cerr << "FAIL: production process fixture path argument is required\n";
        ++failures;
    } else {
        const std::filesystem::path fixture(argv[1]);
        check(std::filesystem::exists(fixture),
              "production process fixture exists");
        if (std::filesystem::exists(fixture)) {
            testLiveProductionIpcInstallerAndOrphanZero(fixture);
            testLiveProductionTwoSeatHandoff(fixture);
            testLiveProductionActivationContextAuthority(fixture);
        }
    }
#else
    (void)argc;
    (void)argv;
#endif

    if (failures != 0) {
        std::cerr << failures << " production launch runtime test(s) failed.\n";
        return 1;
    }
    std::cout << "Production launch runtime tests passed.\n";
    return 0;
}
