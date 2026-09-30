#include "hydra/controller_virtual_xinput.hpp"

namespace hydra::controller {

PollResult pollVirtualXInput(const VirtualXInputMapping& mapping,
                             std::uint8_t logicalSlot,
                             const InventorySnapshot& inventory) noexcept {
    if (!mapping.valid()) {
        return {IoStatus::InvalidBinding, std::nullopt};
    }
    if (logicalSlot != kSeatLogicalXInputSlot) {
        return {IoStatus::Disconnected, std::nullopt};
    }
    return pollBoundController(mapping.source, inventory);
}

IoStatus setVirtualXInputVibration(const VirtualXInputMapping& mapping,
                                   std::uint8_t logicalSlot,
                                   const InventorySnapshot& inventory,
                                   std::uint16_t lowFrequencyMotor,
                                   std::uint16_t highFrequencyMotor) noexcept {
    if (!mapping.valid()) return IoStatus::InvalidBinding;
    if (logicalSlot != kSeatLogicalXInputSlot) return IoStatus::Disconnected;
    return setBoundControllerVibration(
        mapping.source, inventory, lowFrequencyMotor, highFrequencyMotor);
}

} // namespace hydra::controller
