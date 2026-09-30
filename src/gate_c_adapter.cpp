#include "hydra/gate_c_adapter.h"

#include "hydra/gate_c_protocol.hpp"
#include "hydra/virtual_input_state.hpp"
#include "hydra/virtual_raw_input.hpp"
#include "hydra/virtual_xinput_state.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <span>
#include <vector>

namespace {

struct AdapterContext {
    explicit AdapterContext(std::uint16_t contextDiscriminator)
        : raw(architecture(), contextDiscriminator) {}

    static constexpr hydra::gatec::RawArchitecture architecture() noexcept {
        return sizeof(void*) == 8
            ? hydra::gatec::RawArchitecture::X64
            : hydra::gatec::RawArchitecture::X86;
    }

    std::mutex mutex;
    hydra::gatec::VirtualInputState state;
    hydra::gatec::VirtualRawInputContext raw;
    hydra::gatec::VirtualXInputContext xinput;
    std::deque<hydra::gatec::VirtualRawDelivery> pendingRawDeliveries;
    bool rawEnabled{false};
    std::uint32_t processId{0};
    std::uint64_t targetWindow{0};
    std::uint64_t logicalForegroundWindow{0};
    std::uint64_t logicalActiveWindow{0};
    std::uint64_t logicalFocusWindow{0};
    std::uint64_t virtualCaptureWindow{0};
};

std::atomic<std::uint32_t> gNextRawContextDiscriminator{1};

static_assert(sizeof(HydraGateCAdapterInputEventV1) ==
              HYDRA_GATE_C_ADAPTER_INPUT_EVENT_V1_BYTES);
static_assert(sizeof(HydraGateCAdapterControlStateV1) ==
              HYDRA_GATE_C_ADAPTER_CONTROL_STATE_V1_BYTES);
static_assert(sizeof(HydraGateCAdapterSnapshotV1) ==
              HYDRA_GATE_C_ADAPTER_SNAPSHOT_V1_BYTES);
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
static_assert(sizeof(HydraGateCAdapterXInputSourceV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputMappingV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_MAPPING_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputSourceStateV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_STATE_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputStateV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_STATE_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputSourceCapabilitiesV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_CAPABILITIES_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputCapabilitiesV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_CAPABILITIES_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputSourceBatteryV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_BATTERY_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputBatteryV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_BATTERY_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputVibrationV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_VIBRATION_V4_BYTES);

AdapterContext* contextOf(HydraGateCAdapterHandle handle) noexcept {
    return static_cast<AdapterContext*>(handle);
}

bool validBoolean(std::uint8_t value) noexcept {
    return value <= 1u;
}

bool allZero(const std::uint8_t (&bytes)[3]) noexcept {
    return bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0;
}

bool allZero(const std::uint8_t (&bytes)[2]) noexcept {
    return bytes[0] == 0 && bytes[1] == 0;
}

bool allZero(const std::uint8_t (&bytes)[7]) noexcept {
    return std::all_of(std::begin(bytes), std::end(bytes),
                       [](std::uint8_t value) { return value == 0; });
}

HydraGateCAdapterResult mapRawResult(
    hydra::gatec::VirtualRawResult result) noexcept {
    using hydra::gatec::VirtualRawResult;
    switch (result) {
    case VirtualRawResult::Success:
        return HYDRA_GATE_C_ADAPTER_OK;
    case VirtualRawResult::InvalidArgument:
    case VirtualRawResult::UnsupportedUsage:
    case VirtualRawResult::InvalidFlags:
    case VirtualRawResult::InvalidTarget:
    case VirtualRawResult::UnsupportedCommand:
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    case VirtualRawResult::BufferTooSmall:
        return HYDRA_GATE_C_ADAPTER_BUFFER_TOO_SMALL;
    case VirtualRawResult::RegistrationMissing:
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    default:
        return HYDRA_GATE_C_ADAPTER_RAW_INPUT_FAILURE;
    }
}

HydraGateCAdapterResult mapXInputResult(
    hydra::gatec::VirtualXInputResult result) noexcept {
    using hydra::gatec::VirtualXInputResult;
    switch (result) {
    case VirtualXInputResult::Success:
        return HYDRA_GATE_C_ADAPTER_OK;
    case VirtualXInputResult::InvalidArgument:
    case VirtualXInputResult::InvalidSource:
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    case VirtualXInputResult::InvalidLogicalSlot:
        return HYDRA_GATE_C_ADAPTER_XINPUT_INVALID_SLOT;
    case VirtualXInputResult::DuplicateSource:
        return HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_CONFLICT;
    case VirtualXInputResult::Disconnected:
    case VirtualXInputResult::NotMapped:
        return HYDRA_GATE_C_ADAPTER_XINPUT_DISCONNECTED;
    case VirtualXInputResult::StaleSequence:
        return HYDRA_GATE_C_ADAPTER_STALE_SEQUENCE;
    case VirtualXInputResult::StaleGeneration:
        return HYDRA_GATE_C_ADAPTER_XINPUT_STALE_GENERATION;
    case VirtualXInputResult::MappingGenerationMismatch:
        return HYDRA_GATE_C_ADAPTER_XINPUT_MAPPING_MISMATCH;
    case VirtualXInputResult::InvalidState:
    case VirtualXInputResult::GenerationOverflow:
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
}

hydra::gatec::ControllerSourceIdentity sourceOf(
    std::uint8_t kind, std::uint8_t runtimeSlot,
    std::uint64_t sourceKey) noexcept {
    return {
        static_cast<hydra::gatec::ControllerSourceKind>(kind),
        runtimeSlot,
        sourceKey,
    };
}

hydra::gatec::NormalizedXInputGamepad gamepadOf(
    std::uint16_t buttons, std::uint8_t leftTrigger,
    std::uint8_t rightTrigger, std::int16_t thumbLX,
    std::int16_t thumbLY, std::int16_t thumbRX,
    std::int16_t thumbRY) noexcept {
    return {buttons, leftTrigger, rightTrigger, thumbLX, thumbLY,
            thumbRX, thumbRY};
}

void copySource(const hydra::gatec::ControllerSourceIdentity& source,
                std::uint8_t& kind, std::uint8_t& runtimeSlot,
                std::uint64_t& sourceKey) noexcept {
    kind = static_cast<std::uint8_t>(source.kind);
    runtimeSlot = source.runtimeXInputSlotHint;
    sourceKey = source.sourceKey;
}

void copyGamepad(const hydra::gatec::NormalizedXInputGamepad& source,
                 std::uint16_t& buttons, std::uint8_t& leftTrigger,
                 std::uint8_t& rightTrigger, std::int16_t& thumbLX,
                 std::int16_t& thumbLY, std::int16_t& thumbRX,
                 std::int16_t& thumbRY) noexcept {
    buttons = source.buttons;
    leftTrigger = source.leftTrigger;
    rightTrigger = source.rightTrigger;
    thumbLX = source.thumbLX;
    thumbLY = source.thumbLY;
    thumbRX = source.thumbRX;
    thumbRY = source.thumbRY;
}

bool validXInputHeader(std::uint32_t structSize,
                       std::uint32_t expectedSize,
                       std::uint32_t apiVersion) noexcept {
    return structSize == expectedSize &&
           apiVersion == HYDRA_GATE_C_ADAPTER_API_VERSION;
}

void clearWindowState(AdapterContext& context) noexcept {
    context.processId = 0;
    context.targetWindow = 0;
    context.logicalForegroundWindow = 0;
    context.logicalActiveWindow = 0;
    context.logicalFocusWindow = 0;
    context.virtualCaptureWindow = 0;
}

void copyWindowState(const AdapterContext& source,
                     HydraGateCAdapterWindowStateV2& destination) noexcept {
    destination.process_id = source.processId;
    destination.target_window = source.targetWindow;
    destination.logical_foreground_window =
        source.logicalForegroundWindow;
    destination.logical_active_window = source.logicalActiveWindow;
    destination.logical_focus_window = source.logicalFocusWindow;
    destination.virtual_capture_window = source.virtualCaptureWindow;
}

HydraGateCAdapterResult validateInput(
    const HydraGateCAdapterInputEventV1* eventData) noexcept {
    if (eventData == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (eventData->struct_size != sizeof(HydraGateCAdapterInputEventV1)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    if (eventData->reserved0 != 0 || !validBoolean(eventData->is_touchpad)) {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    if (eventData->kind == HYDRA_GATE_C_ADAPTER_INPUT_KEYBOARD) {
        if (eventData->vkey >= 256u ||
            (eventData->key_transition != HYDRA_GATE_C_ADAPTER_KEY_DOWN &&
             eventData->key_transition != HYDRA_GATE_C_ADAPTER_KEY_UP)) {
            return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
        }
    } else if (eventData->kind == HYDRA_GATE_C_ADAPTER_INPUT_MOUSE) {
        if (eventData->key_transition != HYDRA_GATE_C_ADAPTER_KEY_NONE) {
            return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
        }
    } else {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    return HYDRA_GATE_C_ADAPTER_OK;
}

HydraGateCAdapterResult validateControl(
    const HydraGateCAdapterControlStateV1* controlData) noexcept {
    if (controlData == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (controlData->struct_size != sizeof(HydraGateCAdapterControlStateV1)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    if (controlData->reserved0 != 0 ||
        !validBoolean(controlData->clip_enabled) ||
        !validBoolean(controlData->virtual_foreground) ||
        !validBoolean(controlData->virtual_capture)) {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    if (controlData->clip_enabled != 0 &&
        (controlData->clip_right <= controlData->clip_left ||
         controlData->clip_bottom <= controlData->clip_top)) {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    return HYDRA_GATE_C_ADAPTER_OK;
}

hydra::gatec::InputEventMessage toMessage(
    const HydraGateCAdapterInputEventV1& eventData) noexcept {
    hydra::gatec::InputEventMessage message;
    message.kind = eventData.kind == HYDRA_GATE_C_ADAPTER_INPUT_KEYBOARD
                       ? hydra::gatec::InputKind::Keyboard
                       : hydra::gatec::InputKind::Mouse;
    message.keyTransition = static_cast<hydra::gatec::KeyTransition>(
        eventData.key_transition);
    message.isTouchpad = eventData.is_touchpad != 0;
    message.timestampMicros = eventData.timestamp_micros;
    message.vkey = eventData.vkey;
    message.scanCode = eventData.scan_code;
    message.keyboardFlags = eventData.keyboard_flags;
    message.deltaX = eventData.delta_x;
    message.deltaY = eventData.delta_y;
    message.mouseButtonFlags = eventData.mouse_button_flags;
    message.wheelDelta = eventData.wheel_delta;
    return message;
}

hydra::gatec::ControlStateMessage toMessage(
    const HydraGateCAdapterControlStateV1& controlData) noexcept {
    hydra::gatec::ControlStateMessage message;
    message.cursorX = controlData.cursor_x;
    message.cursorY = controlData.cursor_y;
    message.clipEnabled = controlData.clip_enabled != 0;
    message.virtualForeground = controlData.virtual_foreground != 0;
    message.virtualCapture = controlData.virtual_capture != 0;
    message.clipLeft = controlData.clip_left;
    message.clipTop = controlData.clip_top;
    message.clipRight = controlData.clip_right;
    message.clipBottom = controlData.clip_bottom;
    return message;
}

void copySnapshot(const hydra::gatec::StateSnapshotMessage& source,
                  HydraGateCAdapterSnapshotV1& destination) noexcept {
    destination.last_applied_sequence = source.lastAppliedSequence;
    std::copy(source.keyDownBits.begin(), source.keyDownBits.end(),
              destination.key_down_bits);
    std::copy(source.keyPressedEdgeBits.begin(),
              source.keyPressedEdgeBits.end(),
              destination.key_pressed_edge_bits);
    destination.mouse_buttons_down = source.mouseButtonsDown;
    destination.wheel_accumulator = source.wheelAccumulator;
    destination.probe_vkey = source.probeVkey;
    destination.async_key_state_value = source.asyncKeyStateValue;
    destination.keyboard_state_byte = source.keyboardStateByte;
    destination.clip_enabled = source.clipEnabled ? 1u : 0u;
    destination.virtual_foreground = source.virtualForeground ? 1u : 0u;
    destination.virtual_capture = source.virtualCapture ? 1u : 0u;
    destination.cursor_x = source.cursorX;
    destination.cursor_y = source.cursorY;
    destination.clip_left = source.clipLeft;
    destination.clip_top = source.clipTop;
    destination.clip_right = source.clipRight;
    destination.clip_bottom = source.clipBottom;
}

} // namespace

extern "C" {

std::uint32_t HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_api_version(void) {
    return HYDRA_GATE_C_ADAPTER_API_VERSION;
}

HydraGateCAdapterHandle HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_create(void) {
    try {
        const auto discriminator =
            gNextRawContextDiscriminator.fetch_add(1);
        constexpr std::uint32_t maximumDiscriminator =
            sizeof(void*) == 8 ? 0xffu : 0x3fu;
        if (discriminator == 0 || discriminator > maximumDiscriminator) {
            return nullptr;
        }
        return new (std::nothrow) AdapterContext(
            static_cast<std::uint16_t>(discriminator));
    } catch (...) {
        return nullptr;
    }
}

void HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_destroy(HydraGateCAdapterHandle handle) {
    delete contextOf(handle);
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL hydra_gate_c_adapter_reset(
    HydraGateCAdapterHandle handle) {
    auto* context = contextOf(handle);
    if (context == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        context->state.reset();
        context->raw.reset();
        context->xinput.reset();
        context->pendingRawDeliveries.clear();
        context->rawEnabled = false;
        clearWindowState(*context);
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_apply_input(
    HydraGateCAdapterHandle handle, std::uint64_t sequence,
    const HydraGateCAdapterInputEventV1* eventData) {
    auto* context = contextOf(handle);
    if (context == nullptr || sequence == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    const auto validation = validateInput(eventData);
    if (validation != HYDRA_GATE_C_ADAPTER_OK) {
        return validation;
    }
    try {
        std::scoped_lock lock(context->mutex);
        if (sequence <= context->state.lastAppliedSequence()) {
            return HYDRA_GATE_C_ADAPTER_STALE_SEQUENCE;
        }
        const auto message = toMessage(*eventData);
        if (!context->state.applyInput(sequence, message)) {
            return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
        }
        if (context->rawEnabled) {
            const auto delivery = context->raw.enqueueInput(sequence, message);
            if (delivery.result ==
                hydra::gatec::VirtualRawResult::Success) {
                if (context->pendingRawDeliveries.size() >=
                    hydra::gatec::kVirtualRawMaximumPackets) {
                    (void)context->raw.completeDelivery(
                        delivery.token, false, false);
                    return HYDRA_GATE_C_ADAPTER_RAW_INPUT_FAILURE;
                }
                context->pendingRawDeliveries.push_back(delivery);
            } else if (delivery.result !=
                       hydra::gatec::VirtualRawResult::RegistrationMissing) {
                return mapRawResult(delivery.result);
            }
        }
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_apply_control(
    HydraGateCAdapterHandle handle, std::uint64_t sequence,
    const HydraGateCAdapterControlStateV1* controlData) {
    auto* context = contextOf(handle);
    if (context == nullptr || sequence == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    const auto validation = validateControl(controlData);
    if (validation != HYDRA_GATE_C_ADAPTER_OK) {
        return validation;
    }
    try {
        std::scoped_lock lock(context->mutex);
        if (sequence <= context->state.lastAppliedSequence()) {
            return HYDRA_GATE_C_ADAPTER_STALE_SEQUENCE;
        }
        if (!context->state.applyControl(sequence, toMessage(*controlData))) {
            return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
        }
        const auto logicalTarget = context->targetWindow;
        context->logicalForegroundWindow =
            controlData->virtual_foreground != 0 ? logicalTarget : 0;
        context->logicalActiveWindow =
            controlData->virtual_foreground != 0 ? logicalTarget : 0;
        context->logicalFocusWindow =
            controlData->virtual_foreground != 0 ? logicalTarget : 0;
        context->virtualCaptureWindow =
            controlData->virtual_capture != 0 ? logicalTarget : 0;
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_get_async_key_state(
    HydraGateCAdapterHandle handle, std::uint32_t vkey,
    std::uint16_t* value) {
    auto* context = contextOf(handle);
    if (context == nullptr || value == nullptr || vkey >= 256u) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        *value = context->state.consumeAsyncKeyState(vkey);
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_get_key_state(
    HydraGateCAdapterHandle handle, std::uint32_t vkey,
    std::uint16_t* value) {
    auto* context = contextOf(handle);
    if (context == nullptr || value == nullptr || vkey >= 256u) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        *value = context->state.keyDown(vkey) ? 0x8000u : 0u;
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_get_keyboard_state(
    HydraGateCAdapterHandle handle, std::uint8_t* state,
    std::size_t stateBytes) {
    auto* context = contextOf(handle);
    if (context == nullptr || state == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (stateBytes < HYDRA_GATE_C_ADAPTER_KEYBOARD_STATE_BYTES) {
        return HYDRA_GATE_C_ADAPTER_BUFFER_TOO_SMALL;
    }
    try {
        std::scoped_lock lock(context->mutex);
        const auto keyboard = context->state.keyboardState();
        std::copy(keyboard.begin(), keyboard.end(), state);
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_get_control_state(
    HydraGateCAdapterHandle handle,
    HydraGateCAdapterControlStateV1* controlState) {
    auto* context = contextOf(handle);
    if (context == nullptr || controlState == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (controlState->struct_size != sizeof(HydraGateCAdapterControlStateV1)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    try {
        std::scoped_lock lock(context->mutex);
        const auto requestedSize = controlState->struct_size;
        std::memset(controlState, 0, sizeof(*controlState));
        controlState->struct_size = requestedSize;
        controlState->cursor_x = context->state.cursorX();
        controlState->cursor_y = context->state.cursorY();
        controlState->clip_enabled = context->state.clipEnabled() ? 1u : 0u;
        controlState->virtual_foreground =
            context->state.virtualForeground() ? 1u : 0u;
        controlState->virtual_capture =
            context->state.virtualCapture() ? 1u : 0u;
        controlState->clip_left = context->state.clipLeft();
        controlState->clip_top = context->state.clipTop();
        controlState->clip_right = context->state.clipRight();
        controlState->clip_bottom = context->state.clipBottom();
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_get_mouse_state(
    HydraGateCAdapterHandle handle, std::uint32_t* buttonsDown,
    std::int64_t* wheelAccumulator) {
    auto* context = contextOf(handle);
    if (context == nullptr || buttonsDown == nullptr ||
        wheelAccumulator == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        *buttonsDown = context->state.mouseButtonsDown();
        *wheelAccumulator = context->state.wheelAccumulator();
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_get_snapshot(
    HydraGateCAdapterHandle handle, std::uint16_t probeVkey,
    HydraGateCAdapterSnapshotV1* snapshot) {
    auto* context = contextOf(handle);
    if (context == nullptr || snapshot == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (snapshot->struct_size != sizeof(HydraGateCAdapterSnapshotV1)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    if (probeVkey != HYDRA_GATE_C_ADAPTER_NO_PROBE_KEY && probeVkey >= 256u) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        auto state = context->state.snapshot();
        state.probeVkey = probeVkey;
        if (probeVkey < 256u) {
            const auto keyboard = context->state.keyboardState();
            state.keyboardStateByte =
                keyboard[static_cast<std::size_t>(probeVkey)];
            state.asyncKeyStateValue =
                context->state.consumeAsyncKeyState(probeVkey);
        }
        const auto requestedSize = snapshot->struct_size;
        std::memset(snapshot, 0, sizeof(*snapshot));
        snapshot->struct_size = requestedSize;
        copySnapshot(state, *snapshot);
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_set_virtual_cursor(
    HydraGateCAdapterHandle handle, std::int32_t x, std::int32_t y) {
    auto* context = contextOf(handle);
    if (context == nullptr) return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    try {
        std::scoped_lock lock(context->mutex);
        context->state.setVirtualCursor(x, y);
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_set_virtual_clip(
    HydraGateCAdapterHandle handle,
    const HydraGateCAdapterClipRectV2* clip) {
    auto* context = contextOf(handle);
    if (context == nullptr || clip == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (clip->struct_size != sizeof(HydraGateCAdapterClipRectV2)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    if (!validBoolean(clip->enabled) || !allZero(clip->reserved0)) {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    try {
        std::scoped_lock lock(context->mutex);
        return context->state.setVirtualClip(
                   clip->enabled != 0, clip->left, clip->top,
                   clip->right, clip->bottom)
                   ? HYDRA_GATE_C_ADAPTER_OK
                   : HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_configure_window_state(
    HydraGateCAdapterHandle handle,
    const HydraGateCAdapterWindowStateV2* windowState) {
    auto* context = contextOf(handle);
    if (context == nullptr || windowState == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (windowState->struct_size != sizeof(HydraGateCAdapterWindowStateV2) ||
        windowState->api_version != HYDRA_GATE_C_ADAPTER_API_VERSION) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    if (windowState->process_id == 0 || windowState->reserved0 != 0 ||
        windowState->target_window == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    try {
        std::scoped_lock lock(context->mutex);
        context->processId = windowState->process_id;
        context->targetWindow = windowState->target_window;
        context->logicalForegroundWindow =
            windowState->logical_foreground_window;
        context->logicalActiveWindow = windowState->logical_active_window;
        context->logicalFocusWindow = windowState->logical_focus_window;
        context->virtualCaptureWindow =
            windowState->virtual_capture_window;
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_get_window_state(
    HydraGateCAdapterHandle handle,
    HydraGateCAdapterWindowStateV2* windowState) {
    auto* context = contextOf(handle);
    if (context == nullptr || windowState == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (windowState->struct_size != sizeof(HydraGateCAdapterWindowStateV2)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    try {
        std::scoped_lock lock(context->mutex);
        const auto requestedSize = windowState->struct_size;
        std::memset(windowState, 0, sizeof(*windowState));
        windowState->struct_size = requestedSize;
        windowState->api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
        copyWindowState(*context, *windowState);
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_set_virtual_capture(
    HydraGateCAdapterHandle handle, std::uint64_t window,
    std::uint64_t* previousWindow) {
    auto* context = contextOf(handle);
    if (context == nullptr || window == 0 || previousWindow == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        *previousWindow = context->virtualCaptureWindow;
        context->virtualCaptureWindow = window;
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_release_virtual_capture(
    HydraGateCAdapterHandle handle) {
    auto* context = contextOf(handle);
    if (context == nullptr) return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    try {
        std::scoped_lock lock(context->mutex);
        context->virtualCaptureWindow = 0;
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_invalidate_window(
    HydraGateCAdapterHandle handle, std::uint64_t window) {
    auto* context = contextOf(handle);
    if (context == nullptr || window == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        if (context->targetWindow == window) context->targetWindow = 0;
        if (context->logicalForegroundWindow == window) {
            context->logicalForegroundWindow = 0;
        }
        if (context->logicalActiveWindow == window) {
            context->logicalActiveWindow = 0;
        }
        if (context->logicalFocusWindow == window) {
            context->logicalFocusWindow = 0;
        }
        if (context->virtualCaptureWindow == window) {
            context->virtualCaptureWindow = 0;
        }
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_raw_configure(
    HydraGateCAdapterHandle handle, std::uint32_t seatId,
    std::uint32_t processId, std::uint16_t architectureBits) {
    auto* context = contextOf(handle);
    if (context == nullptr || seatId == 0 || processId == 0 ||
        (architectureBits != 32 && architectureBits != 64) ||
        architectureBits != sizeof(void*) * 8u) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        const auto result = context->raw.configure(seatId, processId);
        if (result != hydra::gatec::VirtualRawResult::Success) {
            return mapRawResult(result);
        }
        context->rawEnabled = true;
        context->pendingRawDeliveries.clear();
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_raw_register(
    HydraGateCAdapterHandle handle,
    const HydraGateCAdapterRawRegistrationV3* registrations,
    std::uint32_t registrationCount) {
    auto* context = contextOf(handle);
    if (context == nullptr || registrations == nullptr ||
        registrationCount == 0 ||
        registrationCount >
            hydra::gatec::kVirtualRawMaximumRegistrationOperations) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::vector<hydra::gatec::VirtualRawRegistrationRequest> requests;
        requests.reserve(registrationCount);
        for (std::uint32_t index = 0; index < registrationCount; ++index) {
            const auto& value = registrations[index];
            if (value.struct_size != sizeof(value)) {
                return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
            }
            if (!allZero(value.reserved0) ||
                !validBoolean(value.target_window_current_process)) {
                return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
            }
            requests.push_back({
                {value.usage_page, value.usage}, value.flags,
                value.target_window,
                value.target_window_current_process != 0});
        }
        std::scoped_lock lock(context->mutex);
        if (!context->rawEnabled) {
            return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
        }
        return mapRawResult(context->raw.registerDevices(requests));
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_raw_get_registered(
    HydraGateCAdapterHandle handle,
    HydraGateCAdapterRawRegistrationEntryV3* registrations,
    std::uint32_t* registrationCount) {
    auto* context = contextOf(handle);
    if (context == nullptr || registrationCount == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        if (!context->rawEnabled) {
            return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
        }
        const auto values = context->raw.registrations();
        if (registrations == nullptr) {
            *registrationCount = static_cast<std::uint32_t>(values.size());
            return HYDRA_GATE_C_ADAPTER_OK;
        }
        const auto capacity = *registrationCount;
        *registrationCount = static_cast<std::uint32_t>(values.size());
        if (capacity < values.size()) {
            return HYDRA_GATE_C_ADAPTER_BUFFER_TOO_SMALL;
        }
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (registrations[index].struct_size !=
                sizeof(HydraGateCAdapterRawRegistrationEntryV3)) {
                return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
            }
        }
        for (std::size_t index = 0; index < values.size(); ++index) {
            const auto requestedSize = registrations[index].struct_size;
            std::memset(&registrations[index], 0,
                        sizeof(registrations[index]));
            registrations[index].struct_size = requestedSize;
            registrations[index].usage_page = values[index].key.usagePage;
            registrations[index].usage = values[index].key.usage;
            registrations[index].requested_flags =
                values[index].requestedFlags;
            registrations[index].observable_flags =
                values[index].observableFlags;
            registrations[index].target_window =
                values[index].targetWindowRuntimeValue;
            registrations[index].generation = values[index].generation;
            registrations[index].target_validated_at_registration =
                values[index].targetWindowValidatedAtRegistration ? 1u : 0u;
            registrations[index].device_notification_requested =
                values[index].deviceNotificationRequested ? 1u : 0u;
        }
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_raw_take_delivery(
    HydraGateCAdapterHandle handle,
    HydraGateCAdapterRawDeliveryV3* delivery) {
    auto* context = contextOf(handle);
    if (context == nullptr || delivery == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (delivery->struct_size != sizeof(*delivery)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    try {
        std::scoped_lock lock(context->mutex);
        if (!context->rawEnabled || context->pendingRawDeliveries.empty()) {
            return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
        }
        const auto value = context->pendingRawDeliveries.front();
        context->pendingRawDeliveries.pop_front();
        const auto requestedSize = delivery->struct_size;
        std::memset(delivery, 0, sizeof(*delivery));
        delivery->struct_size = requestedSize;
        delivery->result = static_cast<std::uint32_t>(value.result);
        delivery->token = value.token;
        delivery->target_window = value.targetWindowRuntimeValue;
        delivery->registration_generation =
            value.registrationGeneration;
        delivery->message_wparam = value.messageWParam;
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_raw_complete_delivery(
    HydraGateCAdapterHandle handle, std::uint64_t token,
    std::uint8_t targetCurrentlyValid, std::uint8_t postSucceeded) {
    auto* context = contextOf(handle);
    if (context == nullptr || token == 0 ||
        !validBoolean(targetCurrentlyValid) ||
        !validBoolean(postSucceeded)) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        if (!context->rawEnabled) {
            return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
        }
        return mapRawResult(context->raw.completeDelivery(
            token, targetCurrentlyValid != 0, postSucceeded != 0));
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_raw_get_data(
    HydraGateCAdapterHandle handle, std::uint64_t token,
    std::uint32_t command, void* data, std::uint32_t* size,
    std::uint32_t headerSize) {
    auto* context = contextOf(handle);
    if (context == nullptr || size == nullptr || token == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        if (!context->rawEnabled) {
            return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
        }
        const auto capacity = *size;
        auto output = data == nullptr
            ? std::span<std::byte>{}
            : std::span<std::byte>(static_cast<std::byte*>(data), capacity);
        const auto result = context->raw.readData(
            token, command, headerSize, output, data == nullptr);
        *size = result.sizeAfter;
        return mapRawResult(result.result);
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_raw_get_buffer(
    HydraGateCAdapterHandle handle, void* data, std::uint32_t* size,
    std::uint32_t headerSize, std::uint32_t* packetCount) {
    auto* context = contextOf(handle);
    if (context == nullptr || size == nullptr || packetCount == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        std::scoped_lock lock(context->mutex);
        if (!context->rawEnabled) {
            return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
        }
        const auto capacity = *size;
        auto output = data == nullptr
            ? std::span<std::byte>{}
            : std::span<std::byte>(static_cast<std::byte*>(data), capacity);
        const auto result = context->raw.readBuffer(
            headerSize, output, data == nullptr);
        *size = result.sizeAfter;
        *packetCount = result.packetCount;
        return mapRawResult(result.result);
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_raw_begin_stopping(HydraGateCAdapterHandle handle) {
    auto* context = contextOf(handle);
    if (context == nullptr) return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    try {
        std::scoped_lock lock(context->mutex);
        if (context->rawEnabled) context->raw.beginStopping();
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_raw_reset(HydraGateCAdapterHandle handle) {
    auto* context = contextOf(handle);
    if (context == nullptr) return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    try {
        std::scoped_lock lock(context->mutex);
        context->raw.reset();
        context->pendingRawDeliveries.clear();
        context->rawEnabled = false;
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_xinput_map_slot(
    HydraGateCAdapterHandle handle, std::uint64_t sequence,
    HydraGateCAdapterXInputMappingV4* mapping) {
    auto* context = contextOf(handle);
    if (context == nullptr || mapping == nullptr || sequence == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (!validXInputHeader(mapping->struct_size, sizeof(*mapping),
                           mapping->api_version)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    if (mapping->reserved0 != 0 || mapping->mapping_generation != 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    try {
        const auto source = sourceOf(
            mapping->source_kind, mapping->runtime_xinput_slot_hint,
            mapping->source_key);
        auto result = context->xinput.mapLogicalSlot(
            sequence, mapping->logical_slot, source,
            mapping->source_generation);
        if (result != hydra::gatec::VirtualXInputResult::Success) {
            return mapXInputResult(result);
        }
        hydra::gatec::VirtualXInputMapping queried;
        result = context->xinput.getMapping(mapping->logical_slot, queried);
        if (result != hydra::gatec::VirtualXInputResult::Success) {
            return mapXInputResult(result);
        }
        const auto structSize = mapping->struct_size;
        const auto apiVersion = mapping->api_version;
        std::memset(mapping, 0, sizeof(*mapping));
        mapping->struct_size = structSize;
        mapping->api_version = apiVersion;
        mapping->logical_slot = queried.logicalSlot;
        copySource(queried.source, mapping->source_kind,
                   mapping->runtime_xinput_slot_hint,
                   mapping->source_key);
        mapping->source_generation = queried.sourceGeneration;
        mapping->mapping_generation = queried.mappingGeneration;
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_xinput_unmap_slot(
    HydraGateCAdapterHandle handle, std::uint64_t sequence,
    std::uint8_t logicalSlot) {
    auto* context = contextOf(handle);
    if (context == nullptr || sequence == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    try {
        return mapXInputResult(context->xinput.unmapLogicalSlot(
            sequence, logicalSlot));
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_xinput_apply_state(
    HydraGateCAdapterHandle handle, std::uint64_t sequence,
    const HydraGateCAdapterXInputSourceStateV4* state) {
    auto* context = contextOf(handle);
    if (context == nullptr || state == nullptr || sequence == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (!validXInputHeader(state->struct_size, sizeof(*state),
                           state->api_version)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    if (!allZero(state->reserved0)) {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    try {
        const auto source = sourceOf(
            state->source_kind, state->runtime_xinput_slot_hint,
            state->source_key);
        const auto gamepad = gamepadOf(
            state->buttons, state->left_trigger, state->right_trigger,
            state->thumb_lx, state->thumb_ly, state->thumb_rx,
            state->thumb_ry);
        return mapXInputResult(context->xinput.applySourceState(
            sequence, source, state->source_generation, gamepad));
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_xinput_apply_capabilities(
    HydraGateCAdapterHandle handle, std::uint64_t sequence,
    const HydraGateCAdapterXInputSourceCapabilitiesV4* capabilities) {
    auto* context = contextOf(handle);
    if (context == nullptr || capabilities == nullptr || sequence == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (!validXInputHeader(capabilities->struct_size,
                           sizeof(*capabilities),
                           capabilities->api_version)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    if (!validBoolean(capabilities->vibration_supported) ||
        !allZero(capabilities->reserved0)) {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    try {
        const auto source = sourceOf(
            capabilities->source_kind,
            capabilities->runtime_xinput_slot_hint,
            capabilities->source_key);
        hydra::gatec::NormalizedXInputCapabilities value;
        value.type = static_cast<hydra::gatec::XInputCapabilityType>(
            capabilities->type);
        value.subtype = capabilities->subtype;
        value.flags = capabilities->flags;
        value.gamepad = gamepadOf(
            capabilities->buttons, capabilities->left_trigger,
            capabilities->right_trigger, capabilities->thumb_lx,
            capabilities->thumb_ly, capabilities->thumb_rx,
            capabilities->thumb_ry);
        value.vibrationSupported =
            capabilities->vibration_supported != 0;
        value.leftMotorMaximum = capabilities->left_motor_maximum;
        value.rightMotorMaximum = capabilities->right_motor_maximum;
        return mapXInputResult(context->xinput.applySourceCapabilities(
            sequence, source, capabilities->source_generation, value));
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_xinput_apply_battery(
    HydraGateCAdapterHandle handle, std::uint64_t sequence,
    const HydraGateCAdapterXInputSourceBatteryV4* battery) {
    auto* context = contextOf(handle);
    if (context == nullptr || battery == nullptr || sequence == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (!validXInputHeader(battery->struct_size, sizeof(*battery),
                           battery->api_version)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    if (!validBoolean(battery->available) ||
        !allZero(battery->reserved0)) {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    try {
        const auto source = sourceOf(
            battery->source_kind, battery->runtime_xinput_slot_hint,
            battery->source_key);
        const hydra::gatec::NormalizedXInputBattery value{
            battery->available != 0,
            static_cast<hydra::gatec::XInputBatteryDeviceType>(
                battery->device_type),
            static_cast<hydra::gatec::XInputBatteryType>(
                battery->battery_type),
            static_cast<hydra::gatec::XInputBatteryLevel>(
                battery->battery_level),
        };
        return mapXInputResult(context->xinput.applySourceBattery(
            sequence, source, battery->source_generation, value));
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_xinput_disconnect(
    HydraGateCAdapterHandle handle, std::uint64_t sequence,
    const HydraGateCAdapterXInputSourceV4* sourceData) {
    auto* context = contextOf(handle);
    if (context == nullptr || sourceData == nullptr || sequence == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (!validXInputHeader(sourceData->struct_size, sizeof(*sourceData),
                           sourceData->api_version)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    if (!allZero(sourceData->reserved0)) {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    try {
        const auto source = sourceOf(
            sourceData->source_kind,
            sourceData->runtime_xinput_slot_hint,
            sourceData->source_key);
        return mapXInputResult(context->xinput.disconnectSource(
            sequence, source, sourceData->source_generation));
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_xinput_get_state(
    HydraGateCAdapterHandle handle, std::uint8_t logicalSlot,
    HydraGateCAdapterXInputStateV4* state) {
    auto* context = contextOf(handle);
    if (context == nullptr || state == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (!validXInputHeader(state->struct_size, sizeof(*state),
                           state->api_version)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    try {
        hydra::gatec::VirtualXInputState value;
        const auto result = context->xinput.getState(logicalSlot, value);
        const auto structSize = state->struct_size;
        const auto apiVersion = state->api_version;
        std::memset(state, 0, sizeof(*state));
        state->struct_size = structSize;
        state->api_version = apiVersion;
        state->logical_slot = logicalSlot;
        if (result == hydra::gatec::VirtualXInputResult::Success ||
            result == hydra::gatec::VirtualXInputResult::Disconnected) {
            state->connected = value.connected ? 1u : 0u;
            copySource(value.mapping.source, state->source_kind,
                       state->runtime_xinput_slot_hint,
                       state->source_key);
            state->source_generation = value.mapping.sourceGeneration;
            state->mapping_generation = value.mapping.mappingGeneration;
            state->packet_number = value.packetNumber;
            copyGamepad(value.gamepad, state->buttons,
                        state->left_trigger, state->right_trigger,
                        state->thumb_lx, state->thumb_ly,
                        state->thumb_rx, state->thumb_ry);
        }
        return mapXInputResult(result);
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_xinput_get_capabilities(
    HydraGateCAdapterHandle handle, std::uint8_t logicalSlot,
    HydraGateCAdapterXInputCapabilitiesV4* capabilities) {
    auto* context = contextOf(handle);
    if (context == nullptr || capabilities == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (!validXInputHeader(capabilities->struct_size,
                           sizeof(*capabilities),
                           capabilities->api_version)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    try {
        hydra::gatec::VirtualXInputCapabilities value;
        const auto result = context->xinput.getCapabilities(
            logicalSlot, value);
        const auto structSize = capabilities->struct_size;
        const auto apiVersion = capabilities->api_version;
        std::memset(capabilities, 0, sizeof(*capabilities));
        capabilities->struct_size = structSize;
        capabilities->api_version = apiVersion;
        capabilities->logical_slot = logicalSlot;
        if (result == hydra::gatec::VirtualXInputResult::Success) {
            copySource(value.mapping.source, capabilities->source_kind,
                       capabilities->runtime_xinput_slot_hint,
                       capabilities->source_key);
            capabilities->source_generation =
                value.mapping.sourceGeneration;
            capabilities->mapping_generation =
                value.mapping.mappingGeneration;
            capabilities->type = static_cast<std::uint8_t>(
                value.capabilities.type);
            capabilities->subtype = value.capabilities.subtype;
            capabilities->vibration_supported =
                value.capabilities.vibrationSupported ? 1u : 0u;
            capabilities->flags = value.capabilities.flags;
            copyGamepad(value.capabilities.gamepad,
                        capabilities->buttons,
                        capabilities->left_trigger,
                        capabilities->right_trigger,
                        capabilities->thumb_lx,
                        capabilities->thumb_ly,
                        capabilities->thumb_rx,
                        capabilities->thumb_ry);
            capabilities->left_motor_maximum =
                value.capabilities.leftMotorMaximum;
            capabilities->right_motor_maximum =
                value.capabilities.rightMotorMaximum;
        }
        return mapXInputResult(result);
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_xinput_get_battery(
    HydraGateCAdapterHandle handle, std::uint8_t logicalSlot,
    HydraGateCAdapterXInputBatteryV4* battery) {
    auto* context = contextOf(handle);
    if (context == nullptr || battery == nullptr) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (!validXInputHeader(battery->struct_size, sizeof(*battery),
                           battery->api_version)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    try {
        hydra::gatec::VirtualXInputBattery value;
        const auto result = context->xinput.getBattery(logicalSlot, value);
        const auto structSize = battery->struct_size;
        const auto apiVersion = battery->api_version;
        std::memset(battery, 0, sizeof(*battery));
        battery->struct_size = structSize;
        battery->api_version = apiVersion;
        battery->logical_slot = logicalSlot;
        if (result == hydra::gatec::VirtualXInputResult::Success) {
            battery->available = value.battery.available ? 1u : 0u;
            battery->device_type = static_cast<std::uint8_t>(
                value.battery.deviceType);
            battery->battery_type = static_cast<std::uint8_t>(
                value.battery.batteryType);
            battery->battery_level = static_cast<std::uint8_t>(
                value.battery.batteryLevel);
            copySource(value.mapping.source, battery->source_kind,
                       battery->runtime_xinput_slot_hint,
                       battery->source_key);
            battery->source_generation =
                value.mapping.sourceGeneration;
            battery->mapping_generation =
                value.mapping.mappingGeneration;
        }
        return mapXInputResult(result);
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

HydraGateCAdapterResult HYDRA_GATE_C_ADAPTER_CALL
hydra_gate_c_adapter_xinput_route_vibration(
    HydraGateCAdapterHandle handle, std::uint64_t sequence,
    HydraGateCAdapterXInputVibrationV4* vibration) {
    auto* context = contextOf(handle);
    if (context == nullptr || vibration == nullptr || sequence == 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT;
    }
    if (!validXInputHeader(vibration->struct_size, sizeof(*vibration),
                           vibration->api_version)) {
        return HYDRA_GATE_C_ADAPTER_STRUCT_VERSION_MISMATCH;
    }
    if (vibration->reserved0 != 0 || vibration->source_kind != 0 ||
        vibration->runtime_xinput_slot_hint != 0 ||
        vibration->source_key != 0 || vibration->command_sequence != 0 ||
        vibration->route_count != 0) {
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }
    try {
        hydra::gatec::VirtualXInputVibrationRequest request;
        request.logicalSlot = vibration->logical_slot;
        request.leftMotor = vibration->left_motor;
        request.rightMotor = vibration->right_motor;
        request.expectedMappingGeneration =
            vibration->mapping_generation;
        request.expectedSourceGeneration =
            vibration->source_generation;
        hydra::gatec::VirtualXInputVibrationRoute route;
        const auto result = context->xinput.routeVibration(
            sequence, request, route);
        if (result != hydra::gatec::VirtualXInputResult::Success) {
            return mapXInputResult(result);
        }
        const auto structSize = vibration->struct_size;
        const auto apiVersion = vibration->api_version;
        std::memset(vibration, 0, sizeof(*vibration));
        vibration->struct_size = structSize;
        vibration->api_version = apiVersion;
        vibration->logical_slot = route.logicalSlot;
        copySource(route.source, vibration->source_kind,
                   vibration->runtime_xinput_slot_hint,
                   vibration->source_key);
        vibration->source_generation = route.sourceGeneration;
        vibration->mapping_generation = route.mappingGeneration;
        vibration->left_motor = route.leftMotor;
        vibration->right_motor = route.rightMotor;
        vibration->command_sequence = route.commandSequence;
        vibration->route_count = route.routeCount;
        return HYDRA_GATE_C_ADAPTER_OK;
    } catch (...) {
        return HYDRA_GATE_C_ADAPTER_INTERNAL_ERROR;
    }
}

} // extern "C"
