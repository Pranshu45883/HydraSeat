#include "hydra/runtime_host.hpp"

namespace hydra::runtime {

namespace {

hostipc::SeatSnapshot toHostSnapshot(const SeatRuntimeSnapshot& snapshot) noexcept {
    return {
        snapshot.seatId,
        snapshot.generation,
        snapshot.active,
        snapshot.uiLeaseActive,
        snapshot.gameLeaseActive,
        snapshot.process.has_value(),
        snapshot.targetHwnd != 0,
        snapshot.controllerBinding.has_value(),
    };
}

} // namespace

hostipc::HostSnapshot RuntimeHost::snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    hostipc::HostSnapshot result;
    result.authorityRevision = authorityRevision_;
    const auto seat1 = controller_.snapshot(1);
    const auto seat2 = controller_.snapshot(2);
    if (seat1) result.seats[0] = toHostSnapshot(*seat1);
    if (seat2) result.seats[1] = toHostSnapshot(*seat2);
    return result;
}

std::optional<SeatRuntimeSnapshot> RuntimeHost::seatSnapshot(
    std::uint32_t seatId) const noexcept {
    std::lock_guard lock(mutex_);
    return controller_.snapshot(seatId);
}

ActivationToken RuntimeHost::acquireUiLease(std::uint32_t seatId) noexcept {
    std::lock_guard lock(mutex_);
    const auto lease =
        controller_.acquireSeatLease(seatId, LeaseClass::UiConfiguration);
    noteMutationLocked(lease.valid());
    return lease;
}

bool RuntimeHost::releaseUiLease(const ActivationToken& token) noexcept {
    if (token.leaseClass != LeaseClass::UiConfiguration) return false;
    std::lock_guard lock(mutex_);
    const bool changed = controller_.releaseSeatLease(token);
    noteMutationLocked(changed);
    return changed;
}

bool RuntimeHost::pairController(
    const ActivationToken& uiLease,
    const std::string& persistentControllerId,
    std::uint8_t runtimeXInputSlot) noexcept {
    if (uiLease.leaseClass != LeaseClass::UiConfiguration ||
        persistentControllerId.empty() ||
        runtimeXInputSlot >= controller::kXInputSlotCount) {
        return false;
    }

    std::lock_guard lock(mutex_);
    controller::ControllerInventory inventory;
    const auto snapshot = inventory.scan();
    if (!snapshot.authoritative) return false;

    std::wstring persistentId(
        persistentControllerId.begin(), persistentControllerId.end());
    const auto paired = controller::pairPhysicalControllerToXInput(
        uiLease.seatId, persistentId, runtimeXInputSlot, snapshot);
    if (paired.status != controller::PairingStatus::Ok || !paired.binding) {
        return false;
    }

    const bool changed =
        controller_.bindController(uiLease, *paired.binding, snapshot);
    noteMutationLocked(changed);
    return changed;
}

ActivationToken RuntimeHost::beginSeatActivation(std::uint32_t seatId) noexcept {
    std::lock_guard lock(mutex_);
    const auto activation = controller_.beginSeatActivation(seatId);
    noteMutationLocked(activation.valid());
    return activation;
}

bool RuntimeHost::publishProcess(const ActivationToken& activation,
                                 const ProcessIdentity& process) noexcept {
    std::lock_guard lock(mutex_);
    const bool changed = controller_.publishProcess(activation, process);
    noteMutationLocked(changed);
    return changed;
}

bool RuntimeHost::bindTargetWindow(const ActivationToken& activation,
                                   const ProcessIdentity& owner,
                                   std::uintptr_t hwnd) noexcept {
    std::lock_guard lock(mutex_);
    const bool changed = controller_.bindTargetWindow(activation, owner, hwnd);
    noteMutationLocked(changed);
    return changed;
}

bool RuntimeHost::bindController(
    const ActivationToken& activation,
    const controller::SeatBinding& binding,
    const controller::InventorySnapshot& inventory) noexcept {
    std::lock_guard lock(mutex_);
    const bool changed = controller_.bindController(activation, binding, inventory);
    noteMutationLocked(changed);
    return changed;
}

controller::PollResult RuntimeHost::pollController(
    const ActivationToken& activation,
    const controller::InventorySnapshot& inventory) noexcept {
    std::lock_guard lock(mutex_);
    return controller_.pollController(activation, inventory);
}

controller::IoStatus RuntimeHost::setControllerVibration(
    const ActivationToken& activation,
    const controller::InventorySnapshot& inventory,
    std::uint16_t lowFrequencyMotor,
    std::uint16_t highFrequencyMotor) noexcept {
    std::lock_guard lock(mutex_);
    return controller_.setControllerVibration(
        activation, inventory, lowFrequencyMotor, highFrequencyMotor);
}

std::optional<controller::VirtualXInputMapping>
RuntimeHost::virtualXInputMapping(const ActivationToken& activation) const noexcept {
    std::lock_guard lock(mutex_);
    return controller_.virtualXInputMapping(activation);
}

bool RuntimeHost::endSeatActivation(const ActivationToken& activation) noexcept {
    std::lock_guard lock(mutex_);
    const bool changed = controller_.endSeatActivation(activation);
    noteMutationLocked(changed);
    return changed;
}

void RuntimeHost::noteMutationLocked(bool changed) noexcept {
    if (changed && authorityRevision_ != UINT64_MAX) {
        ++authorityRevision_;
    }
}

} // namespace hydra::runtime
