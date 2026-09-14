#pragma once

#include "hydra/controller_io.hpp"
#include "hydra/controller_inventory.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace hydra::controller {

struct XInputPairingSlot {
    bool connected{false};
    GamepadState state{};

    bool operator==(const XInputPairingSlot&) const = default;
};

struct XInputPairingSnapshot {
    bool authoritative{false};
    std::array<XInputPairingSlot, kXInputSlotCount> slots{};

    bool operator==(const XInputPairingSnapshot&) const = default;
};

enum class PairingProbeStatus : std::uint8_t {
    UniqueButtonPress = 0,
    NoButtonPress = 1,
    AmbiguousButtonPress = 2,
    PlatformUnavailable = 3,
    NativeFailure = 4,
};

struct PairingProbeResult {
    PairingProbeStatus status{PairingProbeStatus::NoButtonPress};
    std::optional<std::uint8_t> runtimeSlot;
};

XInputPairingSnapshot captureXInputPairingSnapshot() noexcept;

// Pairing accepts only a newly pressed digital button on exactly one already
// connected XInput slot. Analog drift and packet-only changes do not count.
PairingProbeResult detectUniqueXInputButtonPress(
    const XInputPairingSnapshot& before,
    const XInputPairingSnapshot& after) noexcept;

PairingResult pairPhysicalControllerFromButtonPress(
    std::uint32_t seatId,
    const std::wstring& persistentControllerId,
    const XInputPairingSnapshot& before,
    const XInputPairingSnapshot& after,
    const InventorySnapshot& inventory) noexcept;

} // namespace hydra::controller
