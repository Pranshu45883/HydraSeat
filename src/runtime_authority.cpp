#include "hydra/runtime_authority.hpp"

#include <limits>

namespace hydra::runtime {

SeatRuntime::SeatRuntime(std::uint32_t seatId) noexcept : seatId_(seatId) {}

ActivationToken SeatRuntime::acquireLease(LeaseClass leaseClass) noexcept {
    std::lock_guard lock(mutex_);
    if (seatId_ != 1 && seatId_ != 2) return {};

    bool* activeLease = nullptr;
    switch (leaseClass) {
    case LeaseClass::UiConfiguration:
        activeLease = &uiLeaseActive_;
        break;
    case LeaseClass::GameProcess:
        activeLease = &gameLeaseActive_;
        break;
    }
    if (activeLease == nullptr || *activeLease) return {};

    if (!uiLeaseActive_ && !gameLeaseActive_) {
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) return {};
        ++generation_;
        process_.reset();
        targetHwnd_ = 0;
        controllerBinding_.reset();
    }

    *activeLease = true;
    return {seatId_, generation_, leaseClass};
}

bool SeatRuntime::publishProcess(const ActivationToken& token,
                                 const ProcessIdentity& process) noexcept {
    if (!process.valid()) return false;

    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token) || token.leaseClass != LeaseClass::GameProcess) {
        return false;
    }
    if (process_ && *process_ != process) return false;

    process_ = process;
    return true;
}

bool SeatRuntime::bindTargetWindow(const ActivationToken& token,
                                   const ProcessIdentity& owner,
                                   std::uintptr_t hwnd) noexcept {
    if (!owner.valid() || hwnd == 0) return false;

    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token) || token.leaseClass != LeaseClass::GameProcess ||
        !process_ || *process_ != owner) {
        return false;
    }

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

bool SeatRuntime::releaseLease(const ActivationToken& token) noexcept {
    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token)) return false;

    switch (token.leaseClass) {
    case LeaseClass::UiConfiguration:
        uiLeaseActive_ = false;
        break;
    case LeaseClass::GameProcess:
        gameLeaseActive_ = false;
        // Process/window state belongs to the game lease and must never remain
        // authoritative merely because a UI configuration lease is still open.
        process_.reset();
        targetHwnd_ = 0;
        break;
    }

    if (!uiLeaseActive_ && !gameLeaseActive_) {
        process_.reset();
        targetHwnd_ = 0;
        controllerBinding_.reset();
    }
    return true;
}

SeatRuntimeSnapshot SeatRuntime::snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    const bool active = uiLeaseActive_ || gameLeaseActive_;
    return {seatId_, generation_, active, uiLeaseActive_, gameLeaseActive_,
            process_, targetHwnd_, controllerBinding_};
}

bool SeatRuntime::ownsTokenLocked(const ActivationToken& token) const noexcept {
    if (!token.valid() || token.seatId != seatId_ ||
        token.generation != generation_) {
        return false;
    }
    switch (token.leaseClass) {
    case LeaseClass::UiConfiguration:
        return uiLeaseActive_;
    case LeaseClass::GameProcess:
        return gameLeaseActive_;
    }
    return false;
}

ActivationToken SessionController::acquireSeatLease(
    std::uint32_t seatId,
    LeaseClass leaseClass) noexcept {
    std::lock_guard lock(mutex_);
    const auto runtime = seat(seatId);
    return runtime ? runtime->acquireLease(leaseClass) : ActivationToken{};
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

controller::PollResult SessionController::pollController(
    const ActivationToken& token,
    const controller::InventorySnapshot& inventory) noexcept {
    if (!token.valid() || token.leaseClass != LeaseClass::GameProcess) {
        return {controller::IoStatus::InvalidBinding, std::nullopt};
    }

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
    if (!token.valid() || token.leaseClass != LeaseClass::GameProcess) {
        return controller::IoStatus::InvalidBinding;
    }

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

std::optional<controller::VirtualXInputMapping>
SessionController::virtualXInputMapping(const ActivationToken& token) const noexcept {
    if (!token.valid() || token.leaseClass != LeaseClass::GameProcess) {
        return std::nullopt;
    }

    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    if (!runtime) return std::nullopt;

    const auto current = runtime->snapshot();
    if (!current.active || current.generation != token.generation ||
        !current.controllerBinding) {
        return std::nullopt;
    }
    controller::VirtualXInputMapping mapping{
        token.seatId, token.generation, *current.controllerBinding};
    if (!mapping.valid()) return std::nullopt;
    return mapping;
}

bool SessionController::releaseSeatLease(const ActivationToken& token) noexcept {
    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    return runtime && runtime->releaseLease(token);
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
