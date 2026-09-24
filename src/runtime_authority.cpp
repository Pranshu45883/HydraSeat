#include "hydra/runtime_authority.hpp"

#include <limits>

namespace hydra::runtime {

SeatRuntime::SeatRuntime(std::uint32_t seatId) noexcept : seatId_(seatId) {}

ActivationToken SeatRuntime::beginActivation() noexcept {
    std::lock_guard lock(mutex_);
    if ((seatId_ != 1 && seatId_ != 2) ||
        active_ ||
        generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return {};
    }

    ++generation_;
    active_ = true;
    process_.reset();
    targetHwnd_ = 0;
    controllerBinding_.reset();
    audioEndpoint_.reset();
    return {seatId_, generation_};
}

bool SeatRuntime::publishProcess(const ActivationToken& token,
                                 const ProcessIdentity& process) noexcept {
    if (!process.valid()) return false;

    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token)) return false;
    if (process_ && *process_ != process) return false;

    process_ = process;
    return true;
}

bool SeatRuntime::bindTargetWindow(const ActivationToken& token,
                                   const ProcessIdentity& owner,
                                   std::uintptr_t hwnd) noexcept {
    if (!owner.valid() || hwnd == 0) return false;

    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token) || !process_ || *process_ != owner) return false;

    targetHwnd_ = hwnd;
    return true;
}

bool SeatRuntime::bindController(const ActivationToken& token,
                                 const controller::SeatBinding& binding) noexcept {
    if (binding.seatId != seatId_ || binding.runtimeKey.empty()) return false;

    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token)) return false;
    if (controllerBinding_ && *controllerBinding_ != binding) return false;

    controllerBinding_ = binding;
    return true;
}

bool SeatRuntime::bindAudioEndpoint(const ActivationToken& token,
                                    const AudioEndpointIdentity& endpoint) noexcept {
    if (!endpoint.valid()) return false;

    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token)) return false;
    
    // Audio endpoints are allowed to be reassigned during the same activation
    audioEndpoint_ = endpoint;
    return true;
}

bool SeatRuntime::clearAudioEndpoint(const ActivationToken& token) noexcept {
    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token)) return false;
    audioEndpoint_.reset();
    return true;
}

bool SeatRuntime::endActivation(const ActivationToken& token) noexcept {
    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token)) return false;

    active_ = false;
    process_.reset();
    targetHwnd_ = 0;
    controllerBinding_.reset();
    audioEndpoint_.reset();
    return true;
}

SeatRuntimeSnapshot SeatRuntime::snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    return {seatId_, generation_, active_, process_, targetHwnd_, controllerBinding_, audioEndpoint_};
}

bool SeatRuntime::ownsTokenLocked(const ActivationToken& token) const noexcept {
    return active_ && token.valid() && token.seatId == seatId_ &&
           token.generation == generation_;
}

ActivationToken SessionController::beginSeatActivation(std::uint32_t seatId) noexcept {
    std::lock_guard lock(mutex_);
    const auto runtime = seat(seatId);
    return runtime ? runtime->beginActivation() : ActivationToken{};
}

bool SessionController::publishProcess(const ActivationToken& token,
                                       const ProcessIdentity& process) noexcept {
    if (!process.valid()) return false;

    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    const auto other = otherSeat(token.seatId);
    if (!runtime || !other) return false;

    const auto otherSnapshot = other->snapshot();
    if (otherSnapshot.active && otherSnapshot.process == process) return false;

    return runtime->publishProcess(token, process);
}

bool SessionController::bindTargetWindow(const ActivationToken& token,
                                         const ProcessIdentity& owner,
                                         std::uintptr_t hwnd) noexcept {
    if (!owner.valid() || hwnd == 0) return false;

    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    const auto other = otherSeat(token.seatId);
    if (!runtime || !other) return false;

    const auto otherSnapshot = other->snapshot();
    if (otherSnapshot.active && otherSnapshot.targetHwnd == hwnd) return false;

    return runtime->bindTargetWindow(token, owner, hwnd);
}

bool SessionController::bindController(
    const ActivationToken& token,
    const controller::SeatBinding& binding,
    const controller::InventorySnapshot& inventory) noexcept {
    if (binding.seatId != token.seatId || binding.runtimeKey.empty() ||
        !controller::bindingMatchesInventory(binding, inventory)) {
        return false;
    }

    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    const auto other = otherSeat(token.seatId);
    if (!runtime || !other) return false;

    const auto otherSnapshot = other->snapshot();
    if (otherSnapshot.active && otherSnapshot.controllerBinding &&
        controller::sameControllerSource(*otherSnapshot.controllerBinding, binding)) {
        return false;
    }

    return runtime->bindController(token, binding);
}

bool SessionController::bindAudioEndpoint(
    const ActivationToken& token,
    const AudioEndpointIdentity& endpoint) noexcept {
    if (!endpoint.valid()) return false;

    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    const auto other = otherSeat(token.seatId);
    if (!runtime || !other) return false;

    const auto otherSnapshot = other->snapshot();
    if (otherSnapshot.active && otherSnapshot.audioEndpoint &&
        *otherSnapshot.audioEndpoint == endpoint) {
        // Technically an endpoint could be routed twice to different processes in Windows,
        // but for HydraSeat we might want to restrict to one process per endpoint.
        // Wait, the instructions say:
        // "Two simultaneous routes. Process A -> Endpoint A, Process B -> Endpoint B. Both assignments can coexist."
        // We will just allow it, but we prevent identical assignment logic here just in case? No, we don't need to prevent it.
        // Actually, just pass it through to the seat runtime.
    }

    return runtime->bindAudioEndpoint(token, endpoint);
}

AudioRouteStatus SessionController::applyAudioRoute(const ActivationToken& token, AudioRouter& router) noexcept {
    if (!token.valid()) return AudioRouteStatus::InvalidProcess;

    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    if (!runtime) return AudioRouteStatus::InvalidProcess;

    const auto current = runtime->snapshot();
    if (!current.active || current.generation != token.generation ||
        !current.process || !current.audioEndpoint) {
        return AudioRouteStatus::RoutingFailed;
    }

    return router.assignEndpoint(*current.process, *current.audioEndpoint);
}

AudioRouteStatus SessionController::clearAudioRoute(const ActivationToken& token, AudioRouter& router) noexcept {
    if (!token.valid()) return AudioRouteStatus::InvalidProcess;

    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    if (!runtime) return AudioRouteStatus::InvalidProcess;

    const auto current = runtime->snapshot();
    if (!current.active || current.generation != token.generation ||
        !current.process) {
        return AudioRouteStatus::RoutingFailed;
    }

    auto status = router.clearAssignment(*current.process);
    if (status == AudioRouteStatus::Success) {
        runtime->clearAudioEndpoint(token);
    }
    return status;
}

controller::PollResult SessionController::pollController(
    const ActivationToken& token,
    const controller::InventorySnapshot& inventory) noexcept {
    if (!token.valid()) return {controller::IoStatus::InvalidBinding, std::nullopt};

    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    if (!runtime) return {controller::IoStatus::InvalidBinding, std::nullopt};

    const auto current = runtime->snapshot();
    if (!current.active || current.generation != token.generation ||
        !current.controllerBinding) {
        return {controller::IoStatus::InvalidBinding, std::nullopt};
    }
    return controller::pollBoundController(*current.controllerBinding, inventory);
}

controller::IoStatus SessionController::setControllerVibration(
    const ActivationToken& token,
    const controller::InventorySnapshot& inventory,
    std::uint16_t lowFrequencyMotor,
    std::uint16_t highFrequencyMotor) noexcept {
    if (!token.valid()) return controller::IoStatus::InvalidBinding;

    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    if (!runtime) return controller::IoStatus::InvalidBinding;

    const auto current = runtime->snapshot();
    if (!current.active || current.generation != token.generation ||
        !current.controllerBinding) {
        return controller::IoStatus::InvalidBinding;
    }
    return controller::setBoundControllerVibration(
        *current.controllerBinding, inventory,
        lowFrequencyMotor, highFrequencyMotor);
}

bool SessionController::endSeatActivation(const ActivationToken& token) noexcept {
    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    return runtime && runtime->endActivation(token);
}

std::optional<SeatRuntimeSnapshot> SessionController::snapshot(
    std::uint32_t seatId) const noexcept {
    const auto runtime = seat(seatId);
    if (!runtime) return std::nullopt;
    return runtime->snapshot();
}

SeatRuntime* SessionController::seat(std::uint32_t seatId) noexcept {
    if (seatId == 1) return &seat1_;
    if (seatId == 2) return &seat2_;
    return nullptr;
}

const SeatRuntime* SessionController::seat(std::uint32_t seatId) const noexcept {
    if (seatId == 1) return &seat1_;
    if (seatId == 2) return &seat2_;
    return nullptr;
}

SeatRuntime* SessionController::otherSeat(std::uint32_t seatId) noexcept {
    if (seatId == 1) return &seat2_;
    if (seatId == 2) return &seat1_;
    return nullptr;
}

const SeatRuntime* SessionController::otherSeat(std::uint32_t seatId) const noexcept {
    if (seatId == 1) return &seat2_;
    if (seatId == 2) return &seat1_;
    return nullptr;
}

} // namespace hydra::runtime
