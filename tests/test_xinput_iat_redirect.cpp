#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/xinput_iat_redirect.hpp"

#include <windows.h>
#include <xinput.h>

#include <cassert>
#include <cstdint>

namespace {

std::uint32_t g_stateCalls = 0;
std::uint32_t g_vibrationCalls = 0;
std::uint32_t g_capabilityCalls = 0;

DWORD WINAPI fakeGetState(
    DWORD userIndex,
    XINPUT_STATE* state) {
    ++g_stateCalls;
    if (userIndex != 0 || state == nullptr) {
        return ERROR_BAD_ARGUMENTS;
    }
    *state = {};
    state->dwPacketNumber = 0x13572468u;
    state->Gamepad.wButtons = XINPUT_GAMEPAD_A;
    return ERROR_SUCCESS;
}

DWORD WINAPI fakeSetState(
    DWORD userIndex,
    XINPUT_VIBRATION* vibration) {
    ++g_vibrationCalls;
    return userIndex == 0 && vibration != nullptr
               ? ERROR_SUCCESS
               : ERROR_BAD_ARGUMENTS;
}

DWORD WINAPI fakeGetCapabilities(
    DWORD userIndex,
    DWORD,
    XINPUT_CAPABILITIES* capabilities) {
    ++g_capabilityCalls;
    if (userIndex != 0 || capabilities == nullptr) {
        return ERROR_BAD_ARGUMENTS;
    }
    *capabilities = {};
    capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
    capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    return ERROR_SUCCESS;
}

} // namespace

int main() {
    // Force all three reviewed XInput imports into the executable IAT before
    // redirect discovery. Their native results are intentionally not asserted
    // because the test host may or may not have a controller connected.
    XINPUT_STATE nativeState{};
    XINPUT_VIBRATION nativeVibration{};
    XINPUT_CAPABILITIES nativeCapabilities{};
    (void)XInputGetState(0, &nativeState);
    (void)XInputSetState(0, &nativeVibration);
    (void)XInputGetCapabilities(
        0, XINPUT_FLAG_GAMEPAD, &nativeCapabilities);

    hydra::controller::XInputIatRedirect redirect;
    hydra::controller::XInputReplacementSet replacements{
        reinterpret_cast<std::uintptr_t>(&fakeGetState),
        reinterpret_cast<std::uintptr_t>(&fakeSetState),
        reinterpret_cast<std::uintptr_t>(&fakeGetCapabilities),
    };

    const auto installed = redirect.install(replacements);
    assert(installed);
    assert(redirect.installed());
    assert(installed.patchedSlotCount >= 3u);

    XINPUT_STATE state{};
    XINPUT_VIBRATION vibration{};
    XINPUT_CAPABILITIES capabilities{};
    assert(XInputGetState(0, &state) == ERROR_SUCCESS);
    assert(state.dwPacketNumber == 0x13572468u);
    assert(state.Gamepad.wButtons == XINPUT_GAMEPAD_A);
    assert(XInputSetState(0, &vibration) == ERROR_SUCCESS);
    assert(
        XInputGetCapabilities(
            0, XINPUT_FLAG_GAMEPAD, &capabilities) ==
        ERROR_SUCCESS);
    assert(g_stateCalls == 1u);
    assert(g_vibrationCalls == 1u);
    assert(g_capabilityCalls == 1u);

    const auto restored = redirect.uninstall();
    assert(restored);
    assert(restored.rollbackComplete);
    assert(!redirect.installed());

    // A post-uninstall native call must no longer hit the fake replacement.
    (void)XInputGetState(0, &state);
    assert(g_stateCalls == 1u);

    return 0;
}
