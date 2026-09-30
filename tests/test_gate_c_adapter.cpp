#include "hydra/gate_c_adapter.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

static_assert(sizeof(HydraGateCAdapterClipRectV2) ==
              HYDRA_GATE_C_ADAPTER_CLIP_RECT_V2_BYTES);
static_assert(sizeof(HydraGateCAdapterWindowStateV2) ==
              HYDRA_GATE_C_ADAPTER_WINDOW_STATE_V2_BYTES);
static_assert(sizeof(HydraGateCAdapterRawRegistrationV3) ==
              HYDRA_GATE_C_ADAPTER_RAW_REGISTRATION_V3_BYTES);
static_assert(sizeof(HydraGateCAdapterRawRegistrationEntryV3) ==
              HYDRA_GATE_C_ADAPTER_RAW_REGISTRATION_ENTRY_V3_BYTES);
static_assert(sizeof(HydraGateCAdapterRawDeliveryV3) ==
              HYDRA_GATE_C_ADAPTER_RAW_DELIVERY_V3_BYTES);

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

HydraGateCAdapterInputEventV1 keyboardEvent(std::uint32_t vkey) {
    HydraGateCAdapterInputEventV1 event{};
    event.struct_size = sizeof(event);
    event.kind = HYDRA_GATE_C_ADAPTER_INPUT_KEYBOARD;
    event.key_transition = HYDRA_GATE_C_ADAPTER_KEY_DOWN;
    event.vkey = vkey;
    return event;
}

HydraGateCAdapterControlStateV1 controlState(
    std::int32_t cursorX, std::int32_t cursorY) {
    HydraGateCAdapterControlStateV1 state{};
    state.struct_size = sizeof(state);
    state.cursor_x = cursorX;
    state.cursor_y = cursorY;
    state.clip_enabled = 1;
    state.virtual_foreground = 1;
    state.virtual_capture = 1;
    state.clip_left = 0;
    state.clip_top = 0;
    state.clip_right = 100;
    state.clip_bottom = 100;
    return state;
}

void testApiAndIndependentContexts() {
    check(hydra_gate_c_adapter_api_version() ==
              HYDRA_GATE_C_ADAPTER_API_VERSION,
          "adapter exports the expected C API version");

    HydraGateCAdapterHandle first = hydra_gate_c_adapter_create();
    HydraGateCAdapterHandle second = hydra_gate_c_adapter_create();
    check(first != nullptr && second != nullptr,
          "two independent adapter contexts are created");

    HydraGateCAdapterWindowStateV2 firstWindows{};
    firstWindows.struct_size = sizeof(firstWindows);
    firstWindows.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    firstWindows.process_id = 101;
    firstWindows.target_window = 0x1001u;
    auto legacyWindows = firstWindows;
    legacyWindows.api_version = 1;
    check(hydra_gate_c_adapter_configure_window_state(
              first, &legacyWindows) ==
              HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH,
          "adapter v1/v2 window-state mismatch fails closed");
    HydraGateCAdapterWindowStateV2 secondWindows = firstWindows;
    secondWindows.process_id = 202;
    secondWindows.target_window = 0x2002u;
    check(hydra_gate_c_adapter_configure_window_state(
              first, &firstWindows) == HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_configure_window_state(
                  second, &secondWindows) == HYDRA_GATE_C_ADAPTER_OK,
          "independent fixed-width logical window state is configured");

    auto firstControl = controlState(10, 20);
    auto secondControl = controlState(70, 80);
    check(hydra_gate_c_adapter_apply_control(first, 1, &firstControl) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "first context accepts control state");
    check(hydra_gate_c_adapter_apply_control(second, 1, &secondControl) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "second context accepts different control state");

    auto keyA = keyboardEvent(0x41);
    auto keyB = keyboardEvent(0x42);
    check(hydra_gate_c_adapter_apply_input(first, 2, &keyA) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "first context accepts A key");
    check(hydra_gate_c_adapter_apply_input(second, 2, &keyB) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "second context accepts B key");

    HydraGateCAdapterInputEventV1 firstMouse{};
    firstMouse.struct_size = sizeof(firstMouse);
    firstMouse.kind = HYDRA_GATE_C_ADAPTER_INPUT_MOUSE;
    firstMouse.delta_x = 5;
    firstMouse.delta_y = 7;
    firstMouse.mouse_button_flags = 0x0001;
    firstMouse.wheel_delta = 120;
    HydraGateCAdapterInputEventV1 secondMouse = firstMouse;
    secondMouse.delta_x = -8;
    secondMouse.delta_y = -9;
    secondMouse.mouse_button_flags = 0x0004;
    secondMouse.wheel_delta = -120;
    check(hydra_gate_c_adapter_apply_input(first, 3, &firstMouse) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "first context accepts mouse state");
    check(hydra_gate_c_adapter_apply_input(second, 3, &secondMouse) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "second context accepts different mouse state");

    HydraGateCAdapterSnapshotV1 firstSnapshot{};
    firstSnapshot.struct_size = sizeof(firstSnapshot);
    HydraGateCAdapterSnapshotV1 secondSnapshot{};
    secondSnapshot.struct_size = sizeof(secondSnapshot);
    check(hydra_gate_c_adapter_get_snapshot(first, 0x41, &firstSnapshot) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "first context snapshot succeeds");
    check(hydra_gate_c_adapter_get_snapshot(second, 0x41, &secondSnapshot) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "second context snapshot succeeds");

    check(firstSnapshot.keyboard_state_byte == 0x80u &&
              firstSnapshot.async_key_state_value == 0x8001u &&
              firstSnapshot.cursor_x == 15 && firstSnapshot.cursor_y == 27 &&
              (firstSnapshot.mouse_buttons_down & 1u) != 0 &&
              firstSnapshot.wheel_accumulator == 120 &&
              firstSnapshot.virtual_foreground == 1,
          "first adapter context exposes only Seat 1 state");
    check(secondSnapshot.keyboard_state_byte == 0 &&
              secondSnapshot.async_key_state_value == 0 &&
              secondSnapshot.cursor_x == 62 && secondSnapshot.cursor_y == 71 &&
              (secondSnapshot.mouse_buttons_down & (1u << 1)) != 0 &&
              secondSnapshot.wheel_accumulator == -120 &&
              secondSnapshot.virtual_foreground == 1,
          "second adapter context does not inherit Seat 1 state");

    HydraGateCAdapterSnapshotV1 secondB{};
    secondB.struct_size = sizeof(secondB);
    check(hydra_gate_c_adapter_get_snapshot(second, 0x42, &secondB) ==
              HYDRA_GATE_C_ADAPTER_OK &&
              secondB.keyboard_state_byte == 0x80u &&
              secondB.async_key_state_value == 0x8001u,
          "second adapter context retains its own B key and edge");

    std::uint16_t asyncA = 0xffffu;
    check(hydra_gate_c_adapter_get_async_key_state(first, 0x41, &asyncA) ==
              HYDRA_GATE_C_ADAPTER_OK && asyncA == 0x8000u,
          "C API consumes each press edge only once");

    std::uint16_t keyStateA = 0;
    check(hydra_gate_c_adapter_get_key_state(first, 0x41, &keyStateA) ==
              HYDRA_GATE_C_ADAPTER_OK && keyStateA == 0x8000u,
          "C API provides GetKeyState-style key-down state without consuming an edge");

    HydraGateCAdapterControlStateV1 controlSnapshot{};
    controlSnapshot.struct_size = sizeof(controlSnapshot);
    check(hydra_gate_c_adapter_get_control_state(first, &controlSnapshot) ==
              HYDRA_GATE_C_ADAPTER_OK &&
              controlSnapshot.cursor_x == 15 &&
              controlSnapshot.cursor_y == 27 &&
              controlSnapshot.virtual_foreground == 1 &&
              controlSnapshot.virtual_capture == 1,
          "C API exposes process-local cursor/focus/capture state directly");

    std::uint32_t mouseButtons = 0;
    std::int64_t wheelAccumulator = 0;
    check(hydra_gate_c_adapter_get_mouse_state(
              first, &mouseButtons, &wheelAccumulator) ==
              HYDRA_GATE_C_ADAPTER_OK &&
              (mouseButtons & 1u) != 0 && wheelAccumulator == 120,
          "C API exposes process-local mouse button and wheel state directly");

    firstWindows = {};
    firstWindows.struct_size = sizeof(firstWindows);
    secondWindows = {};
    secondWindows.struct_size = sizeof(secondWindows);
    check(hydra_gate_c_adapter_get_window_state(first, &firstWindows) ==
              HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_get_window_state(second, &secondWindows) ==
              HYDRA_GATE_C_ADAPTER_OK &&
              firstWindows.logical_foreground_window == 0x1001u &&
              firstWindows.logical_active_window == 0x1001u &&
              firstWindows.logical_focus_window == 0x1001u &&
              firstWindows.virtual_capture_window == 0x1001u &&
              secondWindows.logical_foreground_window == 0x2002u &&
              secondWindows.virtual_capture_window == 0x2002u,
          "control flags resolve to each adapter's own logical target");

    HydraGateCAdapterClipRectV2 negativeClip{};
    negativeClip.struct_size = sizeof(negativeClip);
    negativeClip.enabled = 1;
    negativeClip.left = -100;
    negativeClip.top = -50;
    negativeClip.right = 0;
    negativeClip.bottom = 25;
    check(hydra_gate_c_adapter_set_virtual_clip(first, &negativeClip) ==
              HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_set_virtual_cursor(first, 500, -500) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "negative logical clip coordinates and cursor clamping are accepted");
    controlSnapshot = {};
    controlSnapshot.struct_size = sizeof(controlSnapshot);
    check(hydra_gate_c_adapter_get_control_state(
              first, &controlSnapshot) == HYDRA_GATE_C_ADAPTER_OK &&
              controlSnapshot.cursor_x == -1 &&
              controlSnapshot.cursor_y == -50,
          "cursor clamps to right/bottom-exclusive negative clip boundaries");
    auto invalidClip = negativeClip;
    invalidClip.right = invalidClip.left;
    check(hydra_gate_c_adapter_set_virtual_clip(first, &invalidClip) ==
              HYDRA_GATE_C_ADAPTER_INVALID_STATE,
          "invalid clip rectangle fails without changing adapter state");

    std::uint64_t previousCapture = 0;
    check(hydra_gate_c_adapter_set_virtual_capture(
              first, 0x3003u, &previousCapture) ==
              HYDRA_GATE_C_ADAPTER_OK && previousCapture == 0x1001u &&
              hydra_gate_c_adapter_release_virtual_capture(first) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "virtual capture returns the previous logical HWND and releases locally");
    check(hydra_gate_c_adapter_invalidate_window(first, 0x1001u) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "destroyed logical target can be invalidated without touching another context");

    std::array<std::uint8_t, 256> keyboard{};
    check(hydra_gate_c_adapter_get_keyboard_state(
              first, keyboard.data(), keyboard.size()) ==
              HYDRA_GATE_C_ADAPTER_OK && keyboard[0x41] == 0x80u &&
              keyboard[0x42] == 0,
          "C API provides GetKeyboardState-compatible high bits");

    check(hydra_gate_c_adapter_raw_configure(
              first, 1u, 101u,
              static_cast<std::uint16_t>(sizeof(void*) * 8u)) ==
              HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_raw_configure(
                  second, 2u, 202u,
                  static_cast<std::uint16_t>(sizeof(void*) * 8u)) ==
                  HYDRA_GATE_C_ADAPTER_OK,
          "raw input is configured independently per adapter context");
    HydraGateCAdapterRawRegistrationV3 rawRegistration{};
    rawRegistration.struct_size = sizeof(rawRegistration);
    rawRegistration.usage_page = 0x01u;
    rawRegistration.usage = 0x06u;
    rawRegistration.flags = 0x00002100u;
    rawRegistration.target_window = 0x4444u;
    rawRegistration.target_window_current_process = 1u;
    auto malformedRawRegistration = rawRegistration;
    malformedRawRegistration.struct_size = 0;
    check(hydra_gate_c_adapter_raw_register(
              first, &malformedRawRegistration, 1u) ==
              HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH,
          "raw registration ABI rejects an invalid struct size without mutation");
    check(hydra_gate_c_adapter_raw_register(
              first, &rawRegistration, 1u) == HYDRA_GATE_C_ADAPTER_OK &&
              hydra_gate_c_adapter_raw_register(
                  second, &rawRegistration, 1u) == HYDRA_GATE_C_ADAPTER_OK,
          "raw keyboard registration accepts INPUTSINK plus DEVNOTIFY");
    std::uint32_t rawRegistrationCount = 0;
    check(hydra_gate_c_adapter_raw_get_registered(
              first, nullptr, &rawRegistrationCount) ==
              HYDRA_GATE_C_ADAPTER_OK && rawRegistrationCount == 1u,
          "raw registration query reports the required capacity");
    HydraGateCAdapterRawRegistrationEntryV3 rawEntry{};
    rawEntry.struct_size = sizeof(rawEntry);
    check(hydra_gate_c_adapter_raw_get_registered(
              first, &rawEntry, &rawRegistrationCount) ==
              HYDRA_GATE_C_ADAPTER_OK &&
              rawEntry.requested_flags == 0x00002100u &&
              rawEntry.observable_flags == 0x00000100u &&
              rawEntry.device_notification_requested == 1u,
          "raw registration preserves requested DEVNOTIFY without echoing it as observable");

    auto keyC = keyboardEvent(0x43u);
    keyC.scan_code = 0x2eu;
    check(hydra_gate_c_adapter_apply_input(first, 4u, &keyC) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "registered raw input is serialized during adapter input application");
    HydraGateCAdapterRawDeliveryV3 rawDelivery{};
    rawDelivery.struct_size = sizeof(rawDelivery);
    check(hydra_gate_c_adapter_raw_take_delivery(first, &rawDelivery) ==
              HYDRA_GATE_C_ADAPTER_OK && rawDelivery.token != 0u &&
              rawDelivery.target_window == 0x4444u &&
              hydra_gate_c_adapter_raw_complete_delivery(
                  first, rawDelivery.token, 1u, 1u) ==
                  HYDRA_GATE_C_ADAPTER_OK,
          "raw delivery carries an opaque token and completes after a valid post");
    std::uint32_t rawBytes = 0;
    check(hydra_gate_c_adapter_raw_get_data(
              first, rawDelivery.token, 0x10000003u, nullptr, &rawBytes,
              static_cast<std::uint32_t>(sizeof(void*) == 8u ? 24u : 16u)) ==
              HYDRA_GATE_C_ADAPTER_OK && rawBytes != 0u,
          "RID_INPUT size query retains the raw token");
    std::array<std::uint8_t, 64> rawStorage{};
    std::uint32_t crossBytes = static_cast<std::uint32_t>(rawStorage.size());
    check(hydra_gate_c_adapter_raw_get_data(
              second, rawDelivery.token, 0x10000003u, rawStorage.data(),
              &crossBytes,
              static_cast<std::uint32_t>(sizeof(void*) == 8u ? 24u : 16u)) ==
              HYDRA_GATE_C_ADAPTER_RAW_INPUT_FAILURE,
          "an opaque raw token cannot cross adapter contexts");
    std::uint32_t rawCapacity = static_cast<std::uint32_t>(rawStorage.size());
    check(hydra_gate_c_adapter_raw_get_data(
              first, rawDelivery.token, 0x10000003u, rawStorage.data(),
              &rawCapacity,
              static_cast<std::uint32_t>(sizeof(void*) == 8u ? 24u : 16u)) ==
              HYDRA_GATE_C_ADAPTER_OK && rawCapacity == rawBytes,
          "full RID_INPUT read consumes the exact serialized packet");
    rawCapacity = static_cast<std::uint32_t>(rawStorage.size());
    check(hydra_gate_c_adapter_raw_get_data(
              first, rawDelivery.token, 0x10000003u, rawStorage.data(),
              &rawCapacity,
              static_cast<std::uint32_t>(sizeof(void*) == 8u ? 24u : 16u)) ==
              HYDRA_GATE_C_ADAPTER_RAW_INPUT_FAILURE,
          "a consumed raw token fails deterministically on reuse");

    check(hydra_gate_c_adapter_apply_input(first, 2, &keyA) ==
              HYDRA_GATE_C_ADAPTER_STALE_SEQUENCE,
          "adapter rejects stale sequences");

    auto invalidInput = keyA;
    invalidInput.struct_size = 0;
    check(hydra_gate_c_adapter_apply_input(first, 4, &invalidInput) ==
              HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH,
          "adapter rejects incompatible input struct versions");

    check(hydra_gate_c_adapter_get_keyboard_state(
              first, keyboard.data(), 32) ==
              HYDRA_GATE_C_ADAPTER_BUFFER_TOO_SMALL,
          "adapter rejects undersized keyboard-state buffers");

    check(hydra_gate_c_adapter_reset(first) == HYDRA_GATE_C_ADAPTER_OK,
          "adapter context resets safely");
    HydraGateCAdapterSnapshotV1 resetSnapshot{};
    resetSnapshot.struct_size = sizeof(resetSnapshot);
    check(hydra_gate_c_adapter_get_snapshot(
              first, HYDRA_GATE_C_ADAPTER_NO_PROBE_KEY,
              &resetSnapshot) == HYDRA_GATE_C_ADAPTER_OK &&
              resetSnapshot.last_applied_sequence == 0 &&
              resetSnapshot.mouse_buttons_down == 0 &&
              resetSnapshot.virtual_foreground == 0,
          "reset clears process-local state");

    hydra_gate_c_adapter_destroy(first);
    hydra_gate_c_adapter_destroy(second);
}

} // namespace

int main() {
    testApiAndIndependentContexts();
    std::cout << "Gate C adapter C API tests passed.\n";
    return EXIT_SUCCESS;
}
