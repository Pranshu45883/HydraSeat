#include "hydra/controller_io.hpp"

#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <xinput.h>
#endif

namespace hydra::controller {
namespace {

bool validXInputBinding(const SeatBinding& binding, std::uint8_t& slot) noexcept {
    if (binding.api != ApiSurface::XInput || !binding.runtimeXInputSlot) return false;
    slot = *binding.runtimeXInputSlot;
    if (slot >= kXInputSlotCount) return false;
    return binding.runtimeKey == "xinput-slot:" + std::to_string(slot);
}

} // namespace

PollResult pollBoundController(const SeatBinding& binding) noexcept {
    if (binding.api != ApiSurface::XInput) {
        return {IoStatus::UnsupportedApi, std::nullopt};
    }

    std::uint8_t slot = 0;
    if (!validXInputBinding(binding, slot)) {
        return {IoStatus::InvalidBinding, std::nullopt};
    }

#if defined(_WIN32)
    XINPUT_STATE native{};
    const DWORD result = XInputGetState(static_cast<DWORD>(slot), &native);
    if (result == ERROR_DEVICE_NOT_CONNECTED) {
        return {IoStatus::Disconnected, std::nullopt};
    }
    if (result != ERROR_SUCCESS) {
        return {IoStatus::NativeFailure, std::nullopt};
    }

    GamepadState state;
    state.packetNumber = native.dwPacketNumber;
    state.buttons = native.Gamepad.wButtons;
    state.leftTrigger = native.Gamepad.bLeftTrigger;
    state.rightTrigger = native.Gamepad.bRightTrigger;
    state.thumbLX = native.Gamepad.sThumbLX;
    state.thumbLY = native.Gamepad.sThumbLY;
    state.thumbRX = native.Gamepad.sThumbRX;
    state.thumbRY = native.Gamepad.sThumbRY;
    return {IoStatus::Ok, state};
#else
    (void)slot;
    return {IoStatus::PlatformUnavailable, std::nullopt};
#endif
}

IoStatus setBoundControllerVibration(const SeatBinding& binding,
                                     std::uint16_t leftMotor,
                                     std::uint16_t rightMotor) noexcept {
    if (binding.api != ApiSurface::XInput) return IoStatus::UnsupportedApi;

    std::uint8_t slot = 0;
    if (!validXInputBinding(binding, slot)) return IoStatus::InvalidBinding;

#if defined(_WIN32)
    XINPUT_STATE state{};
    const DWORD connected = XInputGetState(static_cast<DWORD>(slot), &state);
    if (connected == ERROR_DEVICE_NOT_CONNECTED) return IoStatus::Disconnected;
    if (connected != ERROR_SUCCESS) return IoStatus::NativeFailure;

    XINPUT_VIBRATION vibration{};
    vibration.wLeftMotorSpeed = leftMotor;
    vibration.wRightMotorSpeed = rightMotor;
    const DWORD result = XInputSetState(static_cast<DWORD>(slot), &vibration);
    if (result == ERROR_DEVICE_NOT_CONNECTED) return IoStatus::Disconnected;
    if (result != ERROR_SUCCESS) return IoStatus::NativeFailure;
    return IoStatus::Ok;
#else
    (void)slot;
    (void)leftMotor;
    (void)rightMotor;
    return IoStatus::PlatformUnavailable;
#endif
}

} // namespace hydra::controller
