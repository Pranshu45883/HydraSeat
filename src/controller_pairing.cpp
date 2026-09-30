#include "hydra/controller_pairing.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <xinput.h>
#endif

namespace hydra::controller {
namespace {

#if defined(_WIN32)
GamepadState normalizeXInputState(const XINPUT_STATE& native) noexcept {
    GamepadState state;
    state.packetNumber = native.dwPacketNumber;
    state.buttons = native.Gamepad.wButtons;
    state.leftTrigger = native.Gamepad.bLeftTrigger;
    state.rightTrigger = native.Gamepad.bRightTrigger;
    state.thumbLX = native.Gamepad.sThumbLX;
    state.thumbLY = native.Gamepad.sThumbLY;
    state.thumbRX = native.Gamepad.sThumbRX;
    state.thumbRY = native.Gamepad.sThumbRY;
    return state;
}
#endif

} // namespace

XInputPairingSnapshot captureXInputPairingSnapshot() noexcept {
    XInputPairingSnapshot snapshot;

#if defined(_WIN32)
    for (std::uint8_t slot = 0; slot < kXInputSlotCount; ++slot) {
        XINPUT_STATE native{};
        const DWORD result = XInputGetState(static_cast<DWORD>(slot), &native);
        if (result == ERROR_SUCCESS) {
            snapshot.slots[slot].connected = true;
            snapshot.slots[slot].state = normalizeXInputState(native);
            continue;
        }
        if (result != ERROR_DEVICE_NOT_CONNECTED) {
            return snapshot;
        }
    }
    snapshot.authoritative = true;
#endif

    return snapshot;
}

PairingProbeResult detectUniqueXInputButtonPress(
    const XInputPairingSnapshot& before,
    const XInputPairingSnapshot& after) noexcept {
    if (!before.authoritative || !after.authoritative) {
#if defined(_WIN32)
        return {PairingProbeStatus::NativeFailure, std::nullopt};
#else
        return {PairingProbeStatus::PlatformUnavailable, std::nullopt};
#endif
    }

    std::optional<std::uint8_t> candidate;
    for (std::uint8_t slot = 0; slot < kXInputSlotCount; ++slot) {
        const auto& previous = before.slots[slot];
        const auto& current = after.slots[slot];
        if (!previous.connected || !current.connected) continue;

        const std::uint16_t newlyPressed = static_cast<std::uint16_t>(
            current.state.buttons & static_cast<std::uint16_t>(~previous.state.buttons));
        if (newlyPressed == 0) continue;

        if (candidate) {
            return {PairingProbeStatus::AmbiguousButtonPress, std::nullopt};
        }
        candidate = slot;
    }

    if (!candidate) {
        return {PairingProbeStatus::NoButtonPress, std::nullopt};
    }
    return {PairingProbeStatus::UniqueButtonPress, candidate};
}

PairingResult pairPhysicalControllerFromButtonPress(
    std::uint32_t seatId,
    const std::wstring& persistentControllerId,
    const XInputPairingSnapshot& before,
    const XInputPairingSnapshot& after,
    const InventorySnapshot& inventory) noexcept {
    const auto probe = detectUniqueXInputButtonPress(before, after);
    if (probe.status == PairingProbeStatus::AmbiguousButtonPress) {
        return {PairingStatus::PairingGestureAmbiguous, std::nullopt};
    }
    if (probe.status != PairingProbeStatus::UniqueButtonPress || !probe.runtimeSlot) {
        return {PairingStatus::PairingGestureNotDetected, std::nullopt};
    }
    return pairPhysicalControllerToXInput(
        seatId, persistentControllerId, *probe.runtimeSlot, inventory);
}

} // namespace hydra::controller
