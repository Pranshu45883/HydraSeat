#include "hydra/two_seat_launch.hpp"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace hydra::launch {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::size_t kMaximumWindowsPathChars = 32767u;

struct FingerprintBuilder {
    std::uint64_t value{kFnvOffset};

    void byte(std::uint8_t input) noexcept {
        value ^= input;
        value *= kFnvPrime;
    }

    void boolean(bool input) noexcept { byte(input ? 1u : 0u); }

    void u32(std::uint32_t input) noexcept {
        for (unsigned shift = 0; shift < 32u; shift += 8u) {
            byte(static_cast<std::uint8_t>((input >> shift) & 0xffu));
        }
    }

    void u64(std::uint64_t input) noexcept {
        for (unsigned shift = 0; shift < 64u; shift += 8u) {
            byte(static_cast<std::uint8_t>((input >> shift) & 0xffu));
        }
    }

    void text(std::string_view input) noexcept {
        u64(static_cast<std::uint64_t>(input.size()));
        for (const char raw : input) {
            const auto octet = static_cast<unsigned char>(raw);
            byte(static_cast<std::uint8_t>(octet));
        }
    }

    void wide(std::wstring_view input) noexcept {
        u64(static_cast<std::uint64_t>(input.size()));
        for (const wchar_t character : input) {
            const auto unit = static_cast<std::uint32_t>(character);
            u32(unit);
        }
    }
};

bool boundedWide(std::wstring_view value, std::size_t maximum,
                 bool allowEmpty = false) noexcept {
    return (allowEmpty || !value.empty()) && value.size() <= maximum &&
           value.find(L'\0') == std::wstring_view::npos;
}

bool boundedText(std::string_view value, std::size_t maximum,
                 bool allowEmpty = false) noexcept {
    return (allowEmpty || !value.empty()) && value.size() <= maximum &&
           value.find('\0') == std::string_view::npos;
}

std::wstring canonical(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towupper(character));
                   });
    return result;
}

void issue(CompileResult& result, CompileIssueCode code, SeatId seatId,
           std::string detail) {
    result.issues.push_back({code, seatId, std::move(detail)});
}

bool validProcessSpec(const SeatLaunchInput& input, std::string& detail) {
    const auto& spec = input.target.process;
    if (spec.seatId != input.seat.seatId ||
        !boundedWide(spec.executablePath, kMaximumWindowsPathChars) ||
        !boundedWide(spec.workingDirectory, kMaximumWindowsPathChars, true) ||
        spec.arguments.size() > kMaximumLaunchArguments ||
        spec.environmentOverrides.size() > kMaximumLaunchEnvironmentOverrides) {
        detail = "process launch spec is malformed, over-bound, or belongs to another Seat";
        return false;
    }
    for (const auto& argument : spec.arguments) {
        if (!boundedWide(argument, kMaximumWindowsPathChars, true)) {
            detail = "process argument is malformed or exceeds the Windows path/argument bound";
            return false;
        }
    }
    std::set<std::wstring> environmentNames;
    for (const auto& [name, value] : spec.environmentOverrides) {
        if (!boundedWide(name, 32767u) || name.find(L'=') != std::wstring::npos ||
            !boundedWide(value, 32767u, true)) {
            detail = "process environment override is malformed or over-bound";
            return false;
        }
        if (!environmentNames.insert(canonical(name)).second) {
            detail = "process environment override contains a duplicate variable";
            return false;
        }
    }
    return true;
}

bool capabilityAvailable(const TargetSpec& target, ResourceKind kind) noexcept {
    switch (kind) {
        case ResourceKind::Recovery: return target.capabilities.recovery;
        case ResourceKind::Process: return target.capabilities.process;
        case ResourceKind::Window: return target.capabilities.window;
        case ResourceKind::Display: return target.capabilities.display;
        case ResourceKind::Input: return target.capabilities.input;
        case ResourceKind::Controller: return target.capabilities.controller;
        case ResourceKind::Audio: return target.capabilities.audio;
    }
    return false;
}

std::vector<ResourceKind> requiredResources(const TargetSpec& target) {
    std::vector<ResourceKind> result;
    if (target.requirements.recovery) result.push_back(ResourceKind::Recovery);
    result.push_back(ResourceKind::Process);
    if (target.requirements.windowOwnership) result.push_back(ResourceKind::Window);
    if (target.requirements.display) result.push_back(ResourceKind::Display);
    if (target.requirements.keyboard || target.requirements.mouse) {
        result.push_back(ResourceKind::Input);
    }
    if (target.requirements.controller) result.push_back(ResourceKind::Controller);
    if (target.requirements.audioOutput) result.push_back(ResourceKind::Audio);
    return result;
}

void hashSeat(FingerprintBuilder& hash, const SeatActivationPlan& plan) {
    hash.u32(plan.seatId);
    hash.boolean(plan.seat.active);
    hash.wide(plan.seat.name);
    hash.u64(static_cast<std::uint64_t>(plan.seat.displayIds.size()));
    for (const auto& value : plan.seat.displayIds) hash.wide(value);
    hash.boolean(plan.seat.primaryDisplayId.has_value());
    if (plan.seat.primaryDisplayId) hash.wide(*plan.seat.primaryDisplayId);
    hash.u64(static_cast<std::uint64_t>(plan.seat.keyboardIds.size()));
    for (const auto& value : plan.seat.keyboardIds) hash.wide(value);
    hash.u64(static_cast<std::uint64_t>(plan.seat.mouseIds.size()));
    for (const auto& value : plan.seat.mouseIds) hash.wide(value);
    hash.u64(static_cast<std::uint64_t>(plan.seat.controllerIds.size()));
    for (const auto& value : plan.seat.controllerIds) hash.wide(value);
    hash.boolean(plan.seat.audioOutputEndpointId.has_value());
    if (plan.seat.audioOutputEndpointId) hash.wide(*plan.seat.audioOutputEndpointId);
    hash.boolean(plan.seat.audioInputEndpointId.has_value());
    if (plan.seat.audioInputEndpointId) hash.wide(*plan.seat.audioInputEndpointId);

    hash.text(plan.target.gameId);
    hash.wide(plan.target.process.executablePath);
    hash.wide(plan.target.process.workingDirectory);
    hash.u32(static_cast<std::uint32_t>(plan.target.process.architecture));
    hash.u32(static_cast<std::uint32_t>(plan.target.process.containment));
    hash.boolean(plan.target.process.createNewConsole);
    hash.u64(static_cast<std::uint64_t>(plan.target.process.arguments.size()));
    for (const auto& argument : plan.target.process.arguments) hash.wide(argument);
    hash.u64(static_cast<std::uint64_t>(plan.target.process.environmentOverrides.size()));
    for (const auto& [name, value] : plan.target.process.environmentOverrides) {
        hash.wide(name);
        hash.wide(value);
    }

    const auto& requirements = plan.target.requirements;
    hash.boolean(requirements.display);
    hash.boolean(requirements.keyboard);
    hash.boolean(requirements.mouse);
    hash.boolean(requirements.controller);
    hash.boolean(requirements.audioOutput);
    hash.boolean(requirements.windowOwnership);
    hash.boolean(requirements.recovery);
    hash.boolean(requirements.highRisk);
    hash.boolean(plan.target.highRiskApproved);
    hash.boolean(plan.target.capabilities.process);
    hash.boolean(plan.target.capabilities.window);
    hash.boolean(plan.target.capabilities.display);
    hash.boolean(plan.target.capabilities.input);
    hash.boolean(plan.target.capabilities.controller);
    hash.boolean(plan.target.capabilities.audio);
    hash.boolean(plan.target.capabilities.recovery);
    for (const auto resource : plan.resources) {
        hash.byte(static_cast<std::uint8_t>(resource));
    }
}

void checkExclusiveOverlap(CompileResult& result,
                           SeatId seatId,
                           std::span<const std::wstring> values,
                           std::set<std::wstring>& seen,
                           CompileIssueCode code,
                           const char* label) {
    std::set<std::wstring> local;
    for (const auto& value : values) {
        const auto key = canonical(value);
        if (!local.insert(key).second) {
            issue(result, code, seatId,
                  std::string("exclusive ") + label +
                  " appears more than once inside one Seat launch input");
            continue;
        }
        if (!seen.insert(key).second) {
            issue(result, code, seatId,
                  std::string("exclusive ") + label +
                  " is assigned to both launch Seats");
        }
    }
}

} // namespace

CompileResult compileTwoSeatLaunchPlan(std::span<const SeatLaunchInput> inputs) {
    CompileResult result;
    if (inputs.size() != runtime::kV1MaximumActiveSeats) {
        issue(result, CompileIssueCode::ActiveSeatCount, 0,
              "P5-LAUNCH-01 requires exactly two active Seat launch inputs");
        return result;
    }

    std::vector<SeatActivationPlan> seats;
    seats.reserve(inputs.size());
    std::set<SeatId> seatIds;
    std::set<std::wstring> displays;
    std::set<std::wstring> keyboards;
    std::set<std::wstring> mice;
    std::set<std::wstring> controllers;
    std::set<std::wstring> audioOutputs;

    for (const auto& input : inputs) {
        const SeatId seatId = input.seat.seatId;
        if (seatId == 0) {
            issue(result, CompileIssueCode::InvalidSeatId, seatId,
                  "Seat ID zero is invalid");
        } else if (!seatIds.insert(seatId).second) {
            issue(result, CompileIssueCode::DuplicateSeatId, seatId,
                  "launch inputs contain a duplicate Seat ID");
        }
        if (!input.seat.active) {
            issue(result, CompileIssueCode::InactiveSeat, seatId,
                  "P5 launch input must describe an active Seat");
        }
        if (!boundedText(input.target.gameId, kMaximumGameIdBytes)) {
            issue(result, CompileIssueCode::InvalidGameId, seatId,
                  "game ID is empty, malformed, or over-bound");
        }
        std::string processError;
        if (!validProcessSpec(input, processError)) {
            issue(result, CompileIssueCode::InvalidProcessSpec, seatId,
                  std::move(processError));
        }

        const auto& requirements = input.target.requirements;
        if (requirements.display && (input.seat.displayIds.empty() ||
                                     !input.seat.primaryDisplayId)) {
            issue(result, CompileIssueCode::MissingDisplay, seatId,
                  "selected game requires a configured display group and primary display");
        }
        if (requirements.display && input.seat.primaryDisplayId) {
            const auto primary = canonical(*input.seat.primaryDisplayId);
            const bool present = std::any_of(
                input.seat.displayIds.begin(), input.seat.displayIds.end(),
                [&](const std::wstring& value) {
                    return canonical(value) == primary;
                });
            if (!present) {
                issue(result, CompileIssueCode::MissingDisplay, seatId,
                      "primary display is not a member of the Seat display group");
            }
        }
        if (requirements.keyboard && input.seat.keyboardIds.empty()) {
            issue(result, CompileIssueCode::MissingKeyboard, seatId,
                  "selected game requires a keyboard for this Seat");
        }
        if (requirements.mouse && input.seat.mouseIds.empty()) {
            issue(result, CompileIssueCode::MissingMouse, seatId,
                  "selected game requires a mouse for this Seat");
        }
        if (requirements.controller && input.seat.controllerIds.empty()) {
            issue(result, CompileIssueCode::MissingController, seatId,
                  "selected game requires a controller for this Seat");
        }
        if (requirements.audioOutput && !input.seat.audioOutputEndpointId) {
            issue(result, CompileIssueCode::MissingAudioOutput, seatId,
                  "selected game requires a configured audio output endpoint");
        }
        if (requirements.highRisk && !input.target.highRiskApproved) {
            issue(result, CompileIssueCode::HighRiskApprovalRequired, seatId,
                  "high-risk launch option requires explicit approval before plan compilation");
        }

        const auto resources = requiredResources(input.target);
        for (const auto kind : resources) {
            if (!capabilityAvailable(input.target, kind)) {
                issue(result, CompileIssueCode::MissingCapability, seatId,
                      std::string("required capability is unavailable: ") +
                          std::string(resourceKindName(kind)));
            }
        }

        if (requirements.display) {
            checkExclusiveOverlap(result, seatId, input.seat.displayIds, displays,
                                  CompileIssueCode::DuplicateExclusiveDisplay,
                                  "display");
        }
        if (requirements.keyboard) {
            checkExclusiveOverlap(result, seatId, input.seat.keyboardIds, keyboards,
                                  CompileIssueCode::DuplicateExclusiveKeyboard,
                                  "keyboard");
        }
        if (requirements.mouse) {
            checkExclusiveOverlap(result, seatId, input.seat.mouseIds, mice,
                                  CompileIssueCode::DuplicateExclusiveMouse,
                                  "mouse");
        }
        if (requirements.controller) {
            checkExclusiveOverlap(result, seatId, input.seat.controllerIds, controllers,
                                  CompileIssueCode::DuplicateExclusiveController,
                                  "controller");
        }
        if (requirements.audioOutput && input.seat.audioOutputEndpointId) {
            const auto key = canonical(*input.seat.audioOutputEndpointId);
            if (!audioOutputs.insert(key).second) {
                issue(result, CompileIssueCode::DuplicateExclusiveAudioOutput,
                      seatId,
                      "required audio output endpoint is assigned to both launch Seats");
            }
        }

        SeatActivationPlan plan;
        plan.seatId = seatId;
        plan.seat = input.seat;
        // targetHwnd is legacy/transient runtime state. Window ownership is
        // derived from the launched process tree, never from a persisted HWND.
        plan.seat.targetHwnd = 0;
        plan.target = input.target;
        plan.resources = resources;
        seats.push_back(std::move(plan));
    }

    if (!result.issues.empty()) return result;

    std::sort(seats.begin(), seats.end(),
              [](const SeatActivationPlan& left,
                 const SeatActivationPlan& right) {
                  return left.seatId < right.seatId;
              });

    FingerprintBuilder total;
    total.u32(1u);
    total.u64(static_cast<std::uint64_t>(seats.size()));
    for (auto& seat : seats) {
        FingerprintBuilder local;
        hashSeat(local, seat);
        seat.fingerprint = local.value == 0 ? 1u : local.value;
        total.u64(seat.fingerprint);
    }

    TwoSeatLaunchPlan plan;
    plan.fingerprint = total.value == 0 ? 1u : total.value;
    plan.seats = std::move(seats);
    result.plan = std::move(plan);
    return result;
}

PlannedSeatGameInstance::PlannedSeatGameInstance(
    SeatActivationPlan plan,
    std::shared_ptr<ISeatActivationResourceFactory> factory,
    std::unique_ptr<ISeatActivationLifecycleHook> lifecycleHook)
    : plan_(std::move(plan)),
      factory_(std::move(factory)),
      lifecycleHook_(std::move(lifecycleHook)) {}

PlannedSeatGameInstance::~PlannedSeatGameInstance() {
    std::string ignored;
    (void)stop(ignored);
}

bool PlannedSeatGameInstance::start(const runtime::SeatGameBinding& binding,
                                    std::string& error) {
    error.clear();
    if (binding.gameId != plan_.target.gameId) {
        error = "Seat binding game ID does not match the immutable launch plan";
        return false;
    }
    if (started_) return true;
    if (recoveryRequired_) {
        error = "Seat activation is blocked until retained recovery state is cleaned";
        return false;
    }
    if (!factory_ || plan_.seatId == 0 || plan_.fingerprint == 0 ||
        binding.gameId != plan_.target.gameId) {
        error = "Seat binding does not match the immutable launch plan";
        return false;
    }

    if (lifecycleHook_) {
        std::string prepareError;
        bool prepared = false;
        try {
            prepared = lifecycleHook_->prepare(plan_, binding, prepareError);
        } catch (const std::exception& exception) {
            prepareError = std::string("exception: ") + exception.what();
        } catch (...) {
            prepareError = "unknown exception";
        }
        if (!prepared) {
            error = "Seat activation lifecycle prepare failed" +
                    (prepareError.empty() ? "" : ": " + prepareError);
            std::string verifyError;
            const bool safe = lifecycleHook_->verifySafe(verifyError) &&
                              !lifecycleHook_->recoveryRequired();
            recoveryRequired_ = !safe;
            if (!safe && !verifyError.empty()) error += "; verify-safe: " + verifyError;
            return false;
        }
    }

    resources_.clear();
    resources_.reserve(plan_.resources.size());

    const auto failAndRollback = [&](std::string primaryError) {
        error = std::move(primaryError);
        std::string rollbackError;
        const bool rolledBack = rollbackActivation(rollbackError);
        std::string verifySafeError;
        const bool verifiedSafe = verifySafeActivation(verifySafeError);
        const bool safe = rolledBack && verifiedSafe;
        recoveryRequired_ = !safe;
        if (!safe && !rollbackError.empty()) error += "; rollback: " + rollbackError;
        if (!safe && !verifySafeError.empty()) error += "; verify-safe: " + verifySafeError;
        if (safe) resources_.clear();
        return false;
    };

    for (const auto kind : plan_.resources) {
        std::string createError;
        auto resource = factory_->create(kind, plan_, createError);
        if (!resource || resource->kind() != kind) {
            return failAndRollback(
                "Seat activation resource creation failed for " +
                std::string(resourceKindName(kind)) +
                (createError.empty() ? "" : ": " + createError));
        }
        resources_.push_back(std::move(resource));
        std::string prepareError;
        if (!resources_.back()->prepare(plan_, binding, prepareError)) {
            return failAndRollback(
                "Seat activation prepare failed for " +
                std::string(resourceKindName(kind)) +
                (prepareError.empty() ? "" : ": " + prepareError));
        }
    }

    for (auto& resource : resources_) {
        if (resource->kind() == ResourceKind::Process &&
            !invokeLifecycleBoundary("PreSpawn",
                                     &ISeatActivationLifecycleHook::preSpawn,
                                     error)) {
            return failAndRollback(error);
        }

        std::string activationError;
        const bool activated = resource->activate(activationError);
        std::string verificationError;
        const bool verified = activated && resource->verifyActive(verificationError);
        if (!verified) {
            std::string failure = "Seat activation failed for " +
                                  std::string(resourceKindName(resource->kind()));
            if (!activationError.empty()) failure += ": " + activationError;
            if (!verificationError.empty()) failure += "; verify: " + verificationError;
            return failAndRollback(std::move(failure));
        }

        if (resource->kind() == ResourceKind::Process &&
            !invokeLifecycleBoundary("Startup",
                                     &ISeatActivationLifecycleHook::startup,
                                     error)) {
            return failAndRollback(error);
        }
        if (resource->kind() == ResourceKind::Window &&
            !invokeLifecycleBoundary("PostWindow",
                                     &ISeatActivationLifecycleHook::postWindow,
                                     error)) {
            return failAndRollback(error);
        }
    }

    if (!invokeLifecycleBoundary("Runtime",
                                 &ISeatActivationLifecycleHook::runtime,
                                 error)) {
        return failAndRollback(error);
    }

    started_ = true;
    recoveryRequired_ = false;
    return true;
}

bool PlannedSeatGameInstance::invokeLifecycleBoundary(
    const char* boundary,
    bool (ISeatActivationLifecycleHook::*callback)(std::string&),
    std::string& error) {
    if (!lifecycleHook_) return true;
    std::string localError;
    bool succeeded = false;
    try {
        succeeded = (lifecycleHook_.get()->*callback)(localError);
    } catch (const std::exception& exception) {
        localError = std::string("exception: ") + exception.what();
    } catch (...) {
        localError = "unknown exception";
    }
    if (succeeded) return true;
    error = std::string("Seat compatibility lifecycle ") + boundary + " failed" +
            (localError.empty() ? "" : ": " + localError);
    return false;
}

bool PlannedSeatGameInstance::rollbackActivation(std::string& error) noexcept {
    bool success = true;
    std::string firstError;
    bool lifecycleRolledBack = lifecycleHook_ == nullptr;

    const auto rollbackLifecycle = [&]() noexcept {
        if (lifecycleRolledBack || !lifecycleHook_) return;
        std::string localError;
        if (!lifecycleHook_->rollback(localError) || lifecycleHook_->recoveryRequired()) {
            success = false;
            if (firstError.empty()) {
                firstError = "compatibility lifecycle rollback failed" +
                             (localError.empty() ? "" : ": " + localError);
            }
        }
        lifecycleRolledBack = true;
    };

    for (auto iterator = resources_.rbegin(); iterator != resources_.rend(); ++iterator) {
        if (!*iterator) continue;
        // Recovery is the outermost ownership resource. The compatibility
        // transaction outlives Process/Window/later resources so no running
        // process can observe files while they are restored or removed.
        if ((*iterator)->kind() == ResourceKind::Recovery) rollbackLifecycle();
        std::string localError;
        if (!(*iterator)->rollback(localError)) {
            success = false;
            if (firstError.empty()) {
                firstError = std::string(resourceKindName((*iterator)->kind())) +
                             " rollback failed" +
                             (localError.empty() ? "" : ": " + localError);
            }
        }
    }
    rollbackLifecycle();
    if (!success) error = std::move(firstError);
    return success;
}

bool PlannedSeatGameInstance::verifySafeActivation(std::string& error) noexcept {
    bool success = true;
    std::string firstError;
    for (auto& resource : resources_) {
        if (!resource) continue;
        std::string localError;
        if (!resource->verifySafe(localError) || resource->active()) {
            success = false;
            if (firstError.empty()) {
                firstError = std::string(resourceKindName(resource->kind())) +
                             " safe-state verification failed" +
                             (localError.empty() ? "" : ": " + localError);
            }
        }
    }
    if (lifecycleHook_) {
        std::string localError;
        if (!lifecycleHook_->verifySafe(localError) || lifecycleHook_->recoveryRequired()) {
            success = false;
            if (firstError.empty()) {
                firstError = "compatibility lifecycle safe-state verification failed" +
                             (localError.empty() ? "" : ": " + localError);
            }
        }
    }
    if (!success) error = std::move(firstError);
    return success;
}

bool PlannedSeatGameInstance::stop(std::string& error) noexcept {
    error.clear();
    if (resources_.empty() && !lifecycleHook_) {
        started_ = false;
        recoveryRequired_ = false;
        return true;
    }
    const bool rolledBack = rollbackActivation(error);
    std::string verifyError;
    const bool safe = verifySafeActivation(verifyError);
    if (!safe && error.empty()) error = std::move(verifyError);
    started_ = false;
    recoveryRequired_ = !(rolledBack && safe);
    if (!recoveryRequired_) {
        resources_.clear();
        lifecycleHook_.reset();
    }
    return !recoveryRequired_;
}

bool PlannedSeatGameInstance::verifyStopped(std::string& error) noexcept {
    error.clear();
    if (resources_.empty() && !lifecycleHook_) return !started_ && !recoveryRequired_;
    const bool safe = verifySafeActivation(error);
    if (safe) {
        started_ = false;
        recoveryRequired_ = false;
        resources_.clear();
        lifecycleHook_.reset();
    } else {
        recoveryRequired_ = true;
    }
    return safe;
}

bool PlannedSeatGameInstance::running() const noexcept {
    if (!started_) return false;
    const auto processResource = std::find_if(
        resources_.begin(), resources_.end(),
        [](const std::unique_ptr<ISeatActivationResource>& resource) {
            return resource && resource->kind() == ResourceKind::Process;
        });
    return processResource != resources_.end() && (*processResource)->active();
}

PlannedSeatGameInstanceFactory::PlannedSeatGameInstanceFactory(
    TwoSeatLaunchPlan plan,
    std::shared_ptr<ISeatActivationResourceFactory> resources,
    std::shared_ptr<ISeatActivationLifecycleHookFactory> lifecycleHooks)
    : plan_(std::move(plan)),
      resources_(std::move(resources)),
      lifecycleHooks_(std::move(lifecycleHooks)) {}

std::unique_ptr<runtime::ISeatGameInstance>
PlannedSeatGameInstanceFactory::create(SeatId seatId, std::string& error) {
    error.clear();
    if (!resources_ || plan_.schemaVersion != 1u || plan_.fingerprint == 0 ||
        plan_.seats.size() != runtime::kV1MaximumActiveSeats) {
        error = "two-Seat launch factory is not initialized with a valid immutable plan";
        return nullptr;
    }
    const auto found = std::find_if(
        plan_.seats.begin(), plan_.seats.end(),
        [&](const SeatActivationPlan& plan) { return plan.seatId == seatId; });
    if (found == plan_.seats.end()) {
        error = "requested Seat is absent from the immutable two-Seat launch plan";
        return nullptr;
    }
    std::unique_ptr<ISeatActivationLifecycleHook> lifecycleHook;
    if (lifecycleHooks_) {
        lifecycleHook = lifecycleHooks_->create(*found, error);
        if (!lifecycleHook) {
            if (error.empty()) error = "Seat activation lifecycle hook creation failed";
            return nullptr;
        }
    }
    return std::make_unique<PlannedSeatGameInstance>(
        *found, resources_, std::move(lifecycleHook));
}

std::string_view resourceKindName(ResourceKind kind) noexcept {
    switch (kind) {
        case ResourceKind::Recovery: return "recovery";
        case ResourceKind::Process: return "process";
        case ResourceKind::Window: return "window";
        case ResourceKind::Display: return "display";
        case ResourceKind::Input: return "input";
        case ResourceKind::Controller: return "controller";
        case ResourceKind::Audio: return "audio";
    }
    return "unknown";
}

std::string_view compileIssueCodeName(CompileIssueCode code) noexcept {
    switch (code) {
        case CompileIssueCode::ActiveSeatCount: return "active-seat-count";
        case CompileIssueCode::InvalidSeatId: return "invalid-seat-id";
        case CompileIssueCode::DuplicateSeatId: return "duplicate-seat-id";
        case CompileIssueCode::InactiveSeat: return "inactive-seat";
        case CompileIssueCode::InvalidGameId: return "invalid-game-id";
        case CompileIssueCode::InvalidProcessSpec: return "invalid-process-spec";
        case CompileIssueCode::MissingDisplay: return "missing-display";
        case CompileIssueCode::MissingKeyboard: return "missing-keyboard";
        case CompileIssueCode::MissingMouse: return "missing-mouse";
        case CompileIssueCode::MissingController: return "missing-controller";
        case CompileIssueCode::MissingAudioOutput: return "missing-audio-output";
        case CompileIssueCode::DuplicateExclusiveDisplay: return "duplicate-exclusive-display";
        case CompileIssueCode::DuplicateExclusiveKeyboard: return "duplicate-exclusive-keyboard";
        case CompileIssueCode::DuplicateExclusiveMouse: return "duplicate-exclusive-mouse";
        case CompileIssueCode::DuplicateExclusiveController: return "duplicate-exclusive-controller";
        case CompileIssueCode::DuplicateExclusiveAudioOutput: return "duplicate-exclusive-audio-output";
        case CompileIssueCode::MissingCapability: return "missing-capability";
        case CompileIssueCode::HighRiskApprovalRequired: return "high-risk-approval-required";
    }
    return "unknown";
}

} // namespace hydra::launch
