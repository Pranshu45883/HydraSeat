#pragma once

#include "hydra/controller_io.hpp"

#include <cstdint>

namespace hydra::controller {

inline constexpr std::uint8_t kSeatLogicalXInputSlot = 0;

struct VirtualXInputMapping {
    std::uint32_t seatId{0};
    std::uint64_t activationGeneration{0};
    SeatBinding source;

    bool valid() const noexcept {
        return (seatId == 1 || seatId == 2) &&
               activationGeneration != 0 &&
               source.seatId == seatId &&
               source.api == ApiSurface::XInput &&
               source.runtimeXInputSlot.has_value() &&
               *source.runtimeXInputSlot < kXInputSlotCount &&
               source.sourceGeneration != 0 &&
               !source.runtimeKey.empty();
    }

    bool operator==(const VirtualXInputMapping&) const = default;
};

// HydraSeat v1 exposes one controller to one game instance as logical XInput
// slot 0. Other logical slots are intentionally disconnected.
PollResult pollVirtualXInput(const VirtualXInputMapping& mapping,
                             std::uint8_t logicalSlot,
                             const InventorySnapshot& inventory) noexcept;

IoStatus setVirtualXInputVibration(const VirtualXInputMapping& mapping,
                                   std::uint8_t logicalSlot,
                                   const InventorySnapshot& inventory,
                                   std::uint16_t lowFrequencyMotor,
                                   std::uint16_t highFrequencyMotor) noexcept;

} // namespace hydra::controller
