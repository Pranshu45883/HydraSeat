#include "hydra/gate_c_adapter.h"
#include "hydra/gate_c_architecture.hpp"
#include "hydra/gate_c_shim_api.h"
#include "hydra/gate_c_probe_snapshot.hpp"
#include "hydra/gate_c_protocol.hpp"
#include "hydra/gate_c_transport.hpp"
#include "hydra/virtual_raw_input.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <hidusage.h>
#include <windows.h>
#endif

namespace {

using hydra::gatec::ControlStateMessage;
using hydra::gatec::DecodedFrame;
using hydra::gatec::ErrorMessage;
using hydra::gatec::HelloAckMessage;
using hydra::gatec::HelloMessage;
using hydra::gatec::InputEventMessage;
using hydra::gatec::MessageType;
using hydra::gatec::PipeChannel;
using hydra::gatec::ProbeComparison;
using hydra::gatec::QuerySnapshotMessage;
using hydra::gatec::SessionToken;
using hydra::gatec::StateSnapshotMessage;
using hydra::gatec::TransportStatus;

class AdapterOwner {
public:
    AdapterOwner() : m_handle(hydra_gate_c_adapter_create()) {}
    ~AdapterOwner() { hydra_gate_c_adapter_destroy(m_handle); }

    AdapterOwner(const AdapterOwner&) = delete;
    AdapterOwner& operator=(const AdapterOwner&) = delete;

    HydraGateCAdapterHandle get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }

private:
    HydraGateCAdapterHandle m_handle{nullptr};
};

[[maybe_unused]] HydraGateCAdapterInputEventV1 toAdapterEvent(
    const InputEventMessage& message) noexcept {
    HydraGateCAdapterInputEventV1 result{};
    result.struct_size = sizeof(result);
    result.kind = static_cast<std::uint8_t>(
        message.kind == hydra::gatec::InputKind::Keyboard
                      ? HYDRA_GATE_C_ADAPTER_INPUT_KEYBOARD
                      : HYDRA_GATE_C_ADAPTER_INPUT_MOUSE);
    result.key_transition = static_cast<std::uint8_t>(message.keyTransition);
    result.is_touchpad = message.isTouchpad ? 1u : 0u;
    result.timestamp_micros = message.timestampMicros;
    result.vkey = message.vkey;
    result.scan_code = message.scanCode;
    result.keyboard_flags = message.keyboardFlags;
    result.delta_x = message.deltaX;
    result.delta_y = message.deltaY;
    result.mouse_button_flags = message.mouseButtonFlags;
    result.wheel_delta = message.wheelDelta;
    return result;
}

[[maybe_unused]] HydraGateCAdapterControlStateV1 toAdapterControl(
    const ControlStateMessage& message) noexcept {
    HydraGateCAdapterControlStateV1 result{};
    result.struct_size = sizeof(result);
    result.cursor_x = message.cursorX;
    result.cursor_y = message.cursorY;
    result.clip_enabled = message.clipEnabled ? 1u : 0u;
    result.virtual_foreground = message.virtualForeground ? 1u : 0u;
    result.virtual_capture = message.virtualCapture ? 1u : 0u;
    result.clip_left = message.clipLeft;
    result.clip_top = message.clipTop;
    result.clip_right = message.clipRight;
    result.clip_bottom = message.clipBottom;
    return result;
}

[[maybe_unused]] StateSnapshotMessage fromAdapterSnapshot(
    const HydraGateCAdapterSnapshotV1& source) noexcept {
    StateSnapshotMessage result;
    result.lastAppliedSequence = source.last_applied_sequence;
    std::copy_n(source.key_down_bits, result.keyDownBits.size(),
                result.keyDownBits.begin());
    std::copy_n(source.key_pressed_edge_bits,
                result.keyPressedEdgeBits.size(),
                result.keyPressedEdgeBits.begin());
    result.mouseButtonsDown = source.mouse_buttons_down;
    result.wheelAccumulator = source.wheel_accumulator;
    result.probeVkey = source.probe_vkey;
    result.asyncKeyStateValue = source.async_key_state_value;
    result.keyboardStateByte = source.keyboard_state_byte;
    result.clipEnabled = source.clip_enabled != 0;
    result.virtualForeground = source.virtual_foreground != 0;
    result.virtualCapture = source.virtual_capture != 0;
    result.cursorX = source.cursor_x;
    result.cursorY = source.cursor_y;
    result.clipLeft = source.clip_left;
    result.clipTop = source.clip_top;
    result.clipRight = source.clip_right;
    result.clipBottom = source.clip_bottom;
    return result;
}

HydraGateCAdapterXInputMappingV4 toAdapterMapping(
    const hydra::gatec::ControllerUpdateMessage& message) noexcept {
    HydraGateCAdapterXInputMappingV4 result{};
    result.struct_size = sizeof(result);
    result.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    result.logical_slot = message.logicalSlot;
    result.source_kind = static_cast<std::uint8_t>(message.source.kind);
    result.runtime_xinput_slot_hint =
        message.source.runtimeXInputSlotHint;
    result.source_key = message.source.sourceKey;
    result.source_generation = message.sourceGeneration;
    return result;
}

HydraGateCAdapterXInputSourceStateV4 toAdapterXInputState(
    const hydra::gatec::ControllerUpdateMessage& message) noexcept {
    HydraGateCAdapterXInputSourceStateV4 result{};
    result.struct_size = sizeof(result);
    result.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    result.source_kind = static_cast<std::uint8_t>(message.source.kind);
    result.runtime_xinput_slot_hint =
        message.source.runtimeXInputSlotHint;
    result.source_key = message.source.sourceKey;
    result.source_generation = message.sourceGeneration;
    result.buttons = message.gamepad.buttons;
    result.left_trigger = message.gamepad.leftTrigger;
    result.right_trigger = message.gamepad.rightTrigger;
    result.thumb_lx = message.gamepad.thumbLX;
    result.thumb_ly = message.gamepad.thumbLY;
    result.thumb_rx = message.gamepad.thumbRX;
    result.thumb_ry = message.gamepad.thumbRY;
    return result;
}

HydraGateCAdapterXInputSourceCapabilitiesV4 toAdapterXInputCapabilities(
    const hydra::gatec::ControllerUpdateMessage& message) noexcept {
    HydraGateCAdapterXInputSourceCapabilitiesV4 result{};
    result.struct_size = sizeof(result);
    result.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    result.source_kind = static_cast<std::uint8_t>(message.source.kind);
    result.runtime_xinput_slot_hint =
        message.source.runtimeXInputSlotHint;
    result.type = static_cast<std::uint8_t>(message.capabilities.type);
    result.subtype = message.capabilities.subtype;
    result.vibration_supported =
        message.capabilities.vibrationSupported ? 1u : 0u;
    result.source_key = message.source.sourceKey;
    result.source_generation = message.sourceGeneration;
    result.flags = message.capabilities.flags;
    result.buttons = message.capabilities.gamepad.buttons;
    result.left_trigger = message.capabilities.gamepad.leftTrigger;
    result.right_trigger = message.capabilities.gamepad.rightTrigger;
    result.thumb_lx = message.capabilities.gamepad.thumbLX;
    result.thumb_ly = message.capabilities.gamepad.thumbLY;
    result.thumb_rx = message.capabilities.gamepad.thumbRX;
    result.thumb_ry = message.capabilities.gamepad.thumbRY;
    result.left_motor_maximum = message.capabilities.leftMotorMaximum;
    result.right_motor_maximum = message.capabilities.rightMotorMaximum;
    return result;
}

HydraGateCAdapterXInputSourceBatteryV4 toAdapterXInputBattery(
    const hydra::gatec::ControllerUpdateMessage& message) noexcept {
    HydraGateCAdapterXInputSourceBatteryV4 result{};
    result.struct_size = sizeof(result);
    result.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    result.source_kind = static_cast<std::uint8_t>(message.source.kind);
    result.runtime_xinput_slot_hint =
        message.source.runtimeXInputSlotHint;
    result.available = message.battery.available ? 1u : 0u;
    result.device_type = static_cast<std::uint8_t>(
        message.battery.deviceType);
    result.battery_type = static_cast<std::uint8_t>(
        message.battery.batteryType);
    result.battery_level = static_cast<std::uint8_t>(
        message.battery.batteryLevel);
    result.source_key = message.source.sourceKey;
    result.source_generation = message.sourceGeneration;
    return result;
}

HydraGateCAdapterXInputSourceV4 toAdapterXInputSource(
    const hydra::gatec::ControllerUpdateMessage& message) noexcept {
    HydraGateCAdapterXInputSourceV4 result{};
    result.struct_size = sizeof(result);
    result.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    result.source_kind = static_cast<std::uint8_t>(message.source.kind);
    result.runtime_xinput_slot_hint =
        message.source.runtimeXInputSlotHint;
    result.source_key = message.source.sourceKey;
    result.source_generation = message.sourceGeneration;
    return result;
}

hydra::gatec::VirtualXInputResult toVirtualXInputResult(
    HydraGateCAdapterResult result) noexcept {
    using hydra::gatec::VirtualXInputResult;
    switch (result) {
    case HYDRA_GATE_C_ADAPTER_OK:
        return VirtualXInputResult::Success;
    case HYDRA_GATE_C_ADAPTER_STALE_SEQUENCE:
        return VirtualXInputResult::StaleSequence;
    case HYDRA_GATE_C_ADAPTER_XINPUT_DISCONNECTED:
        return VirtualXInputResult::Disconnected;
    case HYDRA_GATE_C_ADAPTER_XINPUT_STALE_GENERATION:
        return VirtualXInputResult::StaleGeneration;
    case HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_CONFLICT:
        return VirtualXInputResult::DuplicateSource;
    case HYDRA_GATE_C_ADAPTER_XINPUT_INVALID_SLOT:
        return VirtualXInputResult::InvalidLogicalSlot;
    case HYDRA_GATE_C_ADAPTER_XINPUT_MAPPING_MISMATCH:
        return VirtualXInputResult::MappingGenerationMismatch;
    case HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT:
        return VirtualXInputResult::InvalidArgument;
    default:
        return VirtualXInputResult::InvalidState;
    }
}

[[maybe_unused]] std::uint64_t monotonicMicros() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct ProbeOptions {
    std::wstring pipeName;
    std::uint32_t seatId{0};
    SessionToken token{};
    bool tokenSet{false};
    bool headless{false};
    bool baselineSelfTest{false};
    bool pollingShim{false};
    bool pollingShimSelfTest{false};
    bool cursorFocusShim{false};
    bool cursorFocusShimSelfTest{false};
    bool rawInputShim{false};
    bool rawInputShimSelfTest{false};
    bool xinputControlled{false};
    bool xinputSelfTest{false};
    std::wstring shimPath;
    bool testMissingWindow{false};
    bool testNoHandshake{false};
    bool testAbnormalExit{false};
    bool showHelp{false};
};

void printUsage(std::ostream& output) {
    output
        << "HydraSeat Gate C Win32 API baseline probe\n\n"
        << "Usage:\n"
        << "  hydra_gate_c_api_probe --pipe <name> --seat <id> --token <32-hex> [--headless]\n"
        << "  hydra_gate_c_api_probe --baseline-self-test\n\n"
        << "  hydra_gate_c_api_probe --polling-shim-self-test --shim <hydra_gate_c_shim.dll>\n\n"
        << "  hydra_gate_c_api_probe --cursor-focus-shim-self-test --shim <hydra_gate_c_shim.dll>\n\n"
        << "  hydra_gate_c_api_probe --raw-input-shim-self-test --shim <hydra_gate_c_shim.dll>\n\n"
        << "  hydra_gate_c_api_probe --xinput-self-test\n\n"
        << "The probe reads ordinary Win32 APIs and the direct Gate C adapter side\n"
        << "by side. The polling mode loads only the explicitly supplied HydraSeat\n"
        << "shim at startup and restores its process-local IAT before unload.\n";
}

#ifdef _WIN32

constexpr wchar_t kWindowClass[] = L"HydraSeatGateCApiProbe";
constexpr UINT kCaptureRequestMessage = WM_APP + 0x51;
constexpr UINT kStateChangedMessage = WM_APP + 0x52;
constexpr std::uint32_t kIoTimeoutMs = 5000;
constexpr std::uint32_t kReadPollMs = 250;
constexpr std::uint32_t kUiCaptureTimeoutMs = 3000;

std::uint64_t runtimeWindowValue(HWND window) noexcept {
    return static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(window));
}

bool parseUnsigned(std::wstring_view text, std::uint32_t& value) {
    if (text.empty()) return false;
    std::uint64_t parsed = 0;
    for (const auto character : text) {
        if (character < L'0' || character > L'9') return false;
        parsed = parsed * 10 + static_cast<std::uint64_t>(character - L'0');
        if (parsed > (std::numeric_limits<std::uint32_t>::max)()) return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

std::string wideAsciiToString(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const auto character : text) {
        const auto code = static_cast<std::uint32_t>(character);
        if (code > 0x7fu) return {};
        result.push_back(static_cast<char>(code));
    }
    return result;
}

ProbeOptions parseOptions(int argc, wchar_t** argv, bool& valid) {
    ProbeOptions options;
    valid = true;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--pipe" && index + 1 < argc) {
            options.pipeName = argv[++index];
        } else if (argument == L"--seat" && index + 1 < argc) {
            valid = parseUnsigned(argv[++index], options.seatId) && valid;
        } else if (argument == L"--token" && index + 1 < argc) {
            const auto token = hydra::gatec::tokenFromHex(
                wideAsciiToString(argv[++index]));
            if (!token) {
                valid = false;
            } else {
                options.token = *token;
                options.tokenSet = true;
            }
        } else if (argument == L"--headless") {
            options.headless = true;
        } else if (argument == L"--baseline-self-test") {
            options.baselineSelfTest = true;
        } else if (argument == L"--polling-shim") {
            options.pollingShim = true;
        } else if (argument == L"--polling-shim-self-test") {
            options.pollingShim = true;
            options.pollingShimSelfTest = true;
        } else if (argument == L"--cursor-focus-shim") {
            options.pollingShim = true;
            options.cursorFocusShim = true;
        } else if (argument == L"--cursor-focus-shim-self-test") {
            options.pollingShim = true;
            options.cursorFocusShim = true;
            options.cursorFocusShimSelfTest = true;
        } else if (argument == L"--raw-input-shim") {
            options.pollingShim = true;
            options.rawInputShim = true;
        } else if (argument == L"--raw-input-shim-self-test") {
            options.pollingShim = true;
            options.rawInputShim = true;
            options.rawInputShimSelfTest = true;
        } else if (argument == L"--xinput-controlled") {
            options.xinputControlled = true;
        } else if (argument == L"--xinput-self-test") {
            options.xinputSelfTest = true;
        } else if (argument == L"--shim" && index + 1 < argc) {
            options.shimPath = argv[++index];
        } else if (argument == L"--test-missing-window") {
            options.testMissingWindow = true;
        } else if (argument == L"--test-no-handshake") {
            options.testNoHandshake = true;
        } else if (argument == L"--test-abnormal-exit") {
            options.testAbnormalExit = true;
        } else if (argument == L"--help" || argument == L"-h") {
            options.showHelp = true;
        } else {
            valid = false;
        }
    }

    const bool connectedMode = !options.pipeName.empty() ||
        options.seatId != 0 || options.tokenSet;
    if (!options.showHelp && !options.baselineSelfTest &&
        !options.pollingShimSelfTest &&
        !options.cursorFocusShimSelfTest &&
        !options.rawInputShimSelfTest && !options.xinputSelfTest &&
        !connectedMode) {
        valid = false;
    }
    if (connectedMode &&
        (options.pipeName.empty() || options.seatId == 0 ||
         !options.tokenSet)) {
        valid = false;
    }
    if (options.testMissingWindow && options.testNoHandshake) valid = false;
    if (options.pollingShim && options.shimPath.empty()) valid = false;
    if (options.xinputControlled && options.pollingShim) valid = false;
    if ((options.pollingShimSelfTest ||
         options.cursorFocusShimSelfTest ||
         options.rawInputShimSelfTest || options.xinputSelfTest) &&
        connectedMode) valid = false;
    if (options.xinputControlled && !connectedMode) valid = false;
    return options;
}

class ShimOwner {
public:
    using ApiVersionFunction = std::uint32_t(HYDRA_GATE_C_SHIM_CALL*)(void);
    using InstallFunction = HydraGateCShimResult(HYDRA_GATE_C_SHIM_CALL*)(
        HydraGateCAdapterHandle, const HydraGateCShimConfigV2*);
    using MarkUnavailableFunction =
        HydraGateCShimResult(HYDRA_GATE_C_SHIM_CALL*)(void);
    using UninstallFunction =
        HydraGateCShimResult(HYDRA_GATE_C_SHIM_CALL*)(void);
    using StatusFunction = HydraGateCShimResult(HYDRA_GATE_C_SHIM_CALL*)(
        HydraGateCShimStatusV1*);
    using DispatchRawInputFunction =
        HydraGateCShimResult(HYDRA_GATE_C_SHIM_CALL*)(void);

    ~ShimOwner() {
        if (m_module != nullptr) {
            if (m_installed && !uninstall()) return;
            if (!m_unloadSafe) return;
            FreeLibrary(m_module);
        }
    }

    ShimOwner(const ShimOwner&) = delete;
    ShimOwner& operator=(const ShimOwner&) = delete;
    ShimOwner() = default;

    bool loadAndInstall(const std::wstring& path,
                        HydraGateCAdapterHandle adapter,
                        std::uint32_t seatId, HWND targetWindow,
                        bool enableCursorFocus = false,
                        bool enableRawInput = false) {
        if (m_module != nullptr || path.empty() || adapter == nullptr ||
            targetWindow == nullptr) {
            return false;
        }
        m_module = LoadLibraryW(path.c_str());
        if (m_module == nullptr) return false;
        m_apiVersion = resolve<ApiVersionFunction>(
            "hydra_gate_c_shim_api_version");
        m_install = resolve<InstallFunction>("hydra_gate_c_shim_install");
        m_markUnavailable = resolve<MarkUnavailableFunction>(
            "hydra_gate_c_shim_mark_adapter_unavailable");
        m_uninstall = resolve<UninstallFunction>(
            "hydra_gate_c_shim_uninstall");
        m_status = resolve<StatusFunction>("hydra_gate_c_shim_get_status");
        m_dispatchRawInput = resolve<DispatchRawInputFunction>(
            "hydra_gate_c_shim_dispatch_raw_input");
        if (m_apiVersion == nullptr || m_install == nullptr ||
            m_markUnavailable == nullptr || m_uninstall == nullptr ||
            m_status == nullptr || m_dispatchRawInput == nullptr ||
            m_apiVersion() != HYDRA_GATE_C_SHIM_API_VERSION) {
            FreeLibrary(m_module);
            m_module = nullptr;
            return false;
        }
        HydraGateCShimConfigV2 config{};
        config.struct_size = sizeof(config);
        config.api_version = HYDRA_GATE_C_SHIM_API_VERSION;
        config.seat_id = seatId;
        config.process_id = GetCurrentProcessId();
        config.flags =
            (enableCursorFocus ? HYDRA_GATE_C_SHIM_ENABLE_CURSOR_FOCUS : 0u) |
            (enableRawInput ? HYDRA_GATE_C_SHIM_ENABLE_RAW_INPUT : 0u);
        config.target_window = runtimeWindowValue(targetWindow);
        m_expectedApiMask = HYDRA_GATE_C_SHIM_POLLING_API_MASK |
            (enableCursorFocus ? HYDRA_GATE_C_SHIM_CURSOR_FOCUS_API_MASK : 0u) |
            (enableRawInput ? HYDRA_GATE_C_SHIM_RAW_INPUT_API_MASK : 0u);
        const auto result = m_install(adapter, &config);
        m_installed = result == HYDRA_GATE_C_SHIM_OK ||
                      result == HYDRA_GATE_C_SHIM_ALREADY_INSTALLED;
        if (!m_installed) {
            const auto value = status();
            m_unloadSafe = value && value->patched_api_mask == 0u &&
                           value->rollback_complete == 1u;
        }
        return m_installed;
    }

    bool active() const {
        const auto value = status();
        return value && value->lifecycle == HYDRA_GATE_C_SHIM_ACTIVE &&
               value->adapter_available == 1u &&
               value->expected_api_mask == m_expectedApiMask &&
               value->patched_api_mask == m_expectedApiMask;
    }

    std::optional<HydraGateCShimStatusV1> status() const {
        if (m_status == nullptr) return std::nullopt;
        HydraGateCShimStatusV1 value{};
        value.struct_size = sizeof(value);
        if (m_status(&value) != HYDRA_GATE_C_SHIM_OK) return std::nullopt;
        return value;
    }

    void markUnavailable() noexcept {
        if (m_installed && m_markUnavailable != nullptr) {
            (void)m_markUnavailable();
        }
    }

    bool dispatchRawInput() noexcept {
        return m_installed && m_dispatchRawInput != nullptr &&
               m_dispatchRawInput() == HYDRA_GATE_C_SHIM_OK;
    }

    bool uninstall() noexcept {
        if (!m_installed) return true;
        if (m_uninstall == nullptr || m_uninstall() != HYDRA_GATE_C_SHIM_OK) {
            return false;
        }
        m_installed = false;
        const auto value = status();
        m_unloadSafe = value &&
            value->lifecycle == HYDRA_GATE_C_SHIM_INACTIVE &&
            value->patched_api_mask == 0u &&
            value->restored_api_mask == m_expectedApiMask &&
            value->rollback_complete == 1u;
        return m_unloadSafe;
    }

private:
    template <typename Function>
    Function resolve(const char* name) const noexcept {
        auto address = GetProcAddress(m_module, name);
#if defined(_M_IX86)
        if (address == nullptr) {
            std::string decorated = "_";
            decorated += name;
            address = GetProcAddress(m_module, decorated.c_str());
        }
#endif
        return reinterpret_cast<Function>(address);
    }

    HMODULE m_module{nullptr};
    ApiVersionFunction m_apiVersion{nullptr};
    InstallFunction m_install{nullptr};
    MarkUnavailableFunction m_markUnavailable{nullptr};
    UninstallFunction m_uninstall{nullptr};
    StatusFunction m_status{nullptr};
    DispatchRawInputFunction m_dispatchRawInput{nullptr};
    bool m_installed{false};
    bool m_unloadSafe{true};
    std::uint32_t m_expectedApiMask{HYDRA_GATE_C_SHIM_ALL_API_MASK};
};

ProbeComparison captureComparison(HydraGateCAdapterHandle adapter,
                                  std::uint64_t sequence,
                                  std::uint32_t seatId,
                                  std::uint16_t probeVkey,
                                  HWND targetWindow,
                                  const hydra::gatec::RawInputApiSnapshot*
                                      rawInput = nullptr) {
    ProbeComparison comparison;
    comparison.sequence = sequence;
    comparison.monotonicTimestampMicros = monotonicMicros();
    comparison.processId = GetCurrentProcessId();
    comparison.threadId = GetCurrentThreadId();
    comparison.seatId = seatId;
    comparison.probeVkey = probeVkey;
    comparison.targetWindowRuntimeValue = runtimeWindowValue(targetWindow);

    comparison.os.asyncKeyState = GetAsyncKeyState(probeVkey);
    comparison.os.keyState = GetKeyState(probeVkey);
    SetLastError(ERROR_SUCCESS);
    comparison.os.keyboardStateSucceeded =
        GetKeyboardState(comparison.os.keyboardState.data()) != FALSE;
    if (!comparison.os.keyboardStateSucceeded) {
        comparison.os.keyboardStateError = GetLastError();
    }

    POINT cursor{};
    SetLastError(ERROR_SUCCESS);
    comparison.os.cursorPositionSucceeded = GetCursorPos(&cursor) != FALSE;
    if (comparison.os.cursorPositionSucceeded) {
        comparison.os.cursorX = cursor.x;
        comparison.os.cursorY = cursor.y;
    } else {
        comparison.os.cursorPositionError = GetLastError();
    }

    RECT clip{};
    SetLastError(ERROR_SUCCESS);
    comparison.os.clipRectangleSucceeded = GetClipCursor(&clip) != FALSE;
    if (comparison.os.clipRectangleSucceeded) {
        comparison.os.clipRectangle = {
            clip.left, clip.top, clip.right, clip.bottom};
    } else {
        comparison.os.clipRectangleError = GetLastError();
    }
    comparison.os.foregroundWindowRuntimeValue =
        runtimeWindowValue(GetForegroundWindow());
    comparison.os.activeWindowRuntimeValue =
        runtimeWindowValue(GetActiveWindow());
    comparison.os.focusWindowRuntimeValue = runtimeWindowValue(GetFocus());
    comparison.os.captureWindowRuntimeValue = runtimeWindowValue(GetCapture());

    HydraGateCAdapterSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    comparison.adapter.snapshotResult = static_cast<std::uint32_t>(
        hydra_gate_c_adapter_get_snapshot(adapter, probeVkey, &snapshot));
    if (comparison.adapter.snapshotResult ==
        static_cast<std::uint32_t>(HYDRA_GATE_C_ADAPTER_OK)) {
        comparison.adapter.lastAppliedSequence =
            snapshot.last_applied_sequence;
        comparison.adapter.asyncKeyState = snapshot.async_key_state_value;
        std::copy_n(snapshot.key_down_bits,
                    comparison.adapter.keyDownBits.size(),
                    comparison.adapter.keyDownBits.begin());
        std::copy_n(snapshot.key_pressed_edge_bits,
                    comparison.adapter.keyPressedEdgeBits.size(),
                    comparison.adapter.keyPressedEdgeBits.begin());
        comparison.adapter.cursorX = snapshot.cursor_x;
        comparison.adapter.cursorY = snapshot.cursor_y;
        comparison.adapter.clipEnabled = snapshot.clip_enabled != 0;
        comparison.adapter.virtualForeground =
            snapshot.virtual_foreground != 0;
        comparison.adapter.virtualCapture = snapshot.virtual_capture != 0;
        comparison.adapter.clipRectangle = {
            snapshot.clip_left, snapshot.clip_top,
            snapshot.clip_right, snapshot.clip_bottom};
    }

    comparison.adapter.keyStateResult = static_cast<std::uint32_t>(
        hydra_gate_c_adapter_get_key_state(
            adapter, probeVkey, &comparison.adapter.keyState));
    comparison.adapter.keyboardStateResult = static_cast<std::uint32_t>(
        hydra_gate_c_adapter_get_keyboard_state(
            adapter, comparison.adapter.keyboardState.data(),
            comparison.adapter.keyboardState.size()));

    HydraGateCAdapterControlStateV1 control{};
    control.struct_size = sizeof(control);
    comparison.adapter.controlStateResult = static_cast<std::uint32_t>(
        hydra_gate_c_adapter_get_control_state(adapter, &control));
    if (comparison.adapter.controlStateResult ==
        static_cast<std::uint32_t>(HYDRA_GATE_C_ADAPTER_OK)) {
        comparison.adapter.cursorX = control.cursor_x;
        comparison.adapter.cursorY = control.cursor_y;
        comparison.adapter.clipEnabled = control.clip_enabled != 0;
        comparison.adapter.virtualForeground = control.virtual_foreground != 0;
        comparison.adapter.virtualCapture = control.virtual_capture != 0;
        comparison.adapter.clipRectangle = {
            control.clip_left, control.clip_top,
            control.clip_right, control.clip_bottom};
    }
    comparison.adapter.mouseStateResult = static_cast<std::uint32_t>(
        hydra_gate_c_adapter_get_mouse_state(
            adapter, &comparison.adapter.mouseButtonsDown,
            &comparison.adapter.wheelAccumulator));

    HydraGateCAdapterWindowStateV2 windows{};
    windows.struct_size = sizeof(windows);
    comparison.adapter.windowStateResult = static_cast<std::uint32_t>(
        hydra_gate_c_adapter_get_window_state(adapter, &windows));
    if (comparison.adapter.windowStateResult ==
        static_cast<std::uint32_t>(HYDRA_GATE_C_ADAPTER_OK)) {
        comparison.adapter.targetWindowRuntimeValue = windows.target_window;
        comparison.adapter.logicalForegroundWindowRuntimeValue =
            windows.logical_foreground_window;
        comparison.adapter.logicalActiveWindowRuntimeValue =
            windows.logical_active_window;
        comparison.adapter.logicalFocusWindowRuntimeValue =
            windows.logical_focus_window;
        comparison.adapter.virtualCaptureWindowRuntimeValue =
            windows.virtual_capture_window;
    }

    if (rawInput != nullptr) comparison.rawInput = *rawInput;
    hydra::gatec::updateProbeComparison(comparison);
    return comparison;
}

bool registerProbeWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassExW(&windowClass) != 0) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int runLocalBaselineSelfTest(HINSTANCE instance) {
    AdapterOwner adapter;
    if (!adapter || hydra_gate_c_adapter_api_version() !=
                        HYDRA_GATE_C_ADAPTER_API_VERSION ||
        !registerProbeWindowClass(instance)) {
        return 9;
    }
    const HWND window = CreateWindowExW(
        0, kWindowClass, L"HydraSeat API Probe Self-Test",
        WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 10;

    ControlStateMessage control;
    control.cursorX = 17;
    control.cursorY = 29;
    control.clipEnabled = true;
    control.virtualForeground = true;
    control.virtualCapture = true;
    control.clipLeft = 0;
    control.clipTop = 0;
    control.clipRight = 100;
    control.clipBottom = 100;
    auto adapterControl = toAdapterControl(control);
    InputEventMessage key;
    key.kind = hydra::gatec::InputKind::Keyboard;
    key.keyTransition = hydra::gatec::KeyTransition::Down;
    key.vkey = 0x41;
    auto adapterKey = toAdapterEvent(key);
    if (hydra_gate_c_adapter_apply_control(adapter.get(), 1,
                                           &adapterControl) !=
            HYDRA_GATE_C_ADAPTER_OK ||
        hydra_gate_c_adapter_apply_input(adapter.get(), 2, &adapterKey) !=
            HYDRA_GATE_C_ADAPTER_OK) {
        DestroyWindow(window);
        return 11;
    }

    const auto comparison = captureComparison(
        adapter.get(), 3, 1, 0x41, window);
    const auto encoded = hydra::gatec::encodeProbeComparison(comparison);
    const auto decoded = hydra::gatec::decodeProbeComparison(encoded);
    const bool passed = decoded && decoded.comparison == comparison &&
        comparison.threadId == GetCurrentThreadId() &&
        comparison.os.keyboardStateSucceeded &&
        comparison.os.cursorPositionSucceeded &&
        comparison.os.clipRectangleSucceeded &&
        comparison.adapter.virtualForeground &&
        comparison.adapter.virtualCapture &&
        comparison.adapter.keyboardState[0x41] == 0x80u &&
        !comparison.osForegroundIsTarget &&
        !comparison.foregroundMatches;
    DestroyWindow(window);
    if (!passed) return 12;
    std::cout
        << "HydraSeat Gate C API probe baseline self-test passed: ordinary "
           "Win32 and direct adapter observations were captured side by side.\n";
    return EXIT_SUCCESS;
}

int runLocalPollingShimSelfTest(HINSTANCE instance,
                                const std::wstring& shimPath) {
    AdapterOwner adapter;
    if (!adapter || hydra_gate_c_adapter_api_version() !=
                        HYDRA_GATE_C_ADAPTER_API_VERSION ||
        !registerProbeWindowClass(instance)) {
        return 13;
    }
    const auto processArchitecture = hydra::gatec::detectProcessArchitecture(
        GetCurrentProcess());
    const auto shimArchitecture =
        hydra::gatec::detectPortableExecutableArchitecture(
            std::filesystem::path(shimPath));
    if (!processArchitecture || !shimArchitecture ||
        processArchitecture.architecture != shimArchitecture.architecture) {
        return 14;
    }
    const HWND window = CreateWindowExW(
        0, kWindowClass, L"HydraSeat Polling Shim Self-Test",
        WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 15;

    const auto beforeInstall = captureComparison(
        adapter.get(), 1, 1, 0x41, window);

    InputEventMessage key;
    key.kind = hydra::gatec::InputKind::Keyboard;
    key.keyTransition = hydra::gatec::KeyTransition::Down;
    key.vkey = 0x41;
    auto adapterKey = toAdapterEvent(key);
    if (hydra_gate_c_adapter_apply_input(adapter.get(), 1, &adapterKey) !=
        HYDRA_GATE_C_ADAPTER_OK) {
        DestroyWindow(window);
        return 16;
    }

    ShimOwner shim;
    if (!shim.loadAndInstall(shimPath, adapter.get(), 1, window) ||
        !shim.active()) {
        DestroyWindow(window);
        return 17;
    }
    const auto comparison = captureComparison(
        adapter.get(), 2, 1, 0x41, window);
    const bool virtualized =
        (static_cast<std::uint16_t>(comparison.os.asyncKeyState) & 0xffffu) ==
            0x8001u &&
        (static_cast<std::uint16_t>(comparison.os.keyState) & 0xffffu) ==
            0x8000u &&
        comparison.os.keyboardStateSucceeded &&
        comparison.os.keyboardState[0x41] == 0x80u &&
        comparison.os.keyboardState[0x42] == 0u &&
        comparison.asyncDownMatches && comparison.keyStateDownMatches &&
        comparison.keyboardStateDownMatches;
    const bool restored = shim.uninstall();
    const auto afterUninstall = captureComparison(
        adapter.get(), 3, 1, 0x41, window);
    const bool lifecycleSnapshots =
        beforeInstall.monotonicTimestampMicros != 0 &&
        comparison.monotonicTimestampMicros >=
            beforeInstall.monotonicTimestampMicros &&
        afterUninstall.monotonicTimestampMicros >=
            comparison.monotonicTimestampMicros &&
        beforeInstall.os.keyboardStateSucceeded &&
        afterUninstall.os.keyboardStateSucceeded;
    DestroyWindow(window);
    if (!virtualized || !restored || !lifecycleSnapshots) return 18;
    std::cout
        << "HydraSeat Gate C polling shim self-test passed: startup-loaded "
           "ordinary polling calls used adapter state and uninstall restored "
           "the original IAT pointers.\n";
    return EXIT_SUCCESS;
}

int runLocalCursorFocusShimSelfTest(HINSTANCE instance,
                                    const std::wstring& shimPath) {
    AdapterOwner adapter;
    if (!adapter || !registerProbeWindowClass(instance)) return 28;
    const HWND target = CreateWindowExW(
        0, kWindowClass, L"HydraSeat Cursor Focus Shim Self-Test",
        WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
        nullptr, nullptr, instance, nullptr);
    const HWND alternate = CreateWindowExW(
        0, kWindowClass, L"HydraSeat Cursor Focus Alternate",
        WS_OVERLAPPEDWINDOW, 0, 0, 200, 120,
        nullptr, nullptr, instance, nullptr);
    if (target == nullptr || alternate == nullptr) {
        if (target != nullptr) DestroyWindow(target);
        if (alternate != nullptr) DestroyWindow(alternate);
        return 29;
    }

    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto nativeGetCursorPos = reinterpret_cast<BOOL(WINAPI*)(LPPOINT)>(
        GetProcAddress(user32, "GetCursorPos"));
    const auto nativeGetClipCursor = reinterpret_cast<BOOL(WINAPI*)(LPRECT)>(
        GetProcAddress(user32, "GetClipCursor"));
    const auto nativeGetForegroundWindow =
        reinterpret_cast<HWND(WINAPI*)(void)>(
            GetProcAddress(user32, "GetForegroundWindow"));
    const auto nativeGetCapture = reinterpret_cast<HWND(WINAPI*)(void)>(
        GetProcAddress(user32, "GetCapture"));
    POINT nativeCursorBefore{};
    RECT nativeClipBefore{};
    if (nativeGetCursorPos == nullptr || nativeGetClipCursor == nullptr ||
        nativeGetForegroundWindow == nullptr || nativeGetCapture == nullptr ||
        nativeGetCursorPos(&nativeCursorBefore) == FALSE ||
        nativeGetClipCursor(&nativeClipBefore) == FALSE) {
        DestroyWindow(alternate);
        DestroyWindow(target);
        return 30;
    }
    const HWND nativeForegroundBefore = nativeGetForegroundWindow();
    const HWND nativeCaptureBefore = nativeGetCapture();

    ShimOwner shim;
    if (!shim.loadAndInstall(shimPath, adapter.get(), 1, target, true) ||
        !shim.active()) {
        DestroyWindow(alternate);
        DestroyWindow(target);
        return 31;
    }
    ControlStateMessage control;
    control.cursorX = 10;
    control.cursorY = 20;
    control.clipEnabled = true;
    control.virtualForeground = true;
    control.virtualCapture = true;
    control.clipLeft = 0;
    control.clipTop = 0;
    control.clipRight = 100;
    control.clipBottom = 100;
    auto adapterControl = toAdapterControl(control);
    if (hydra_gate_c_adapter_apply_control(adapter.get(), 1,
                                           &adapterControl) !=
        HYDRA_GATE_C_ADAPTER_OK) {
        (void)shim.uninstall();
        DestroyWindow(alternate);
        DestroyWindow(target);
        return 32;
    }

    POINT cursor{};
    RECT clip{};
    const RECT initialClip{0, 0, 100, 100};
    const bool initialQueries =
        GetCursorPos(&cursor) != FALSE && cursor.x == 10 && cursor.y == 20 &&
        GetClipCursor(&clip) != FALSE &&
        EqualRect(&clip, &initialClip) != FALSE &&
        GetForegroundWindow() == target && GetActiveWindow() == target &&
        GetFocus() == target && GetCapture() == target;

    RECT replacementClip{-10, -20, 30, 40};
    const HWND previousCapture = SetCapture(alternate);
    const bool virtualMutations =
        ClipCursor(&replacementClip) != FALSE &&
        SetCursorPos(100, 100) != FALSE &&
        GetCursorPos(&cursor) != FALSE && cursor.x == 29 && cursor.y == 39 &&
        GetClipCursor(&clip) != FALSE &&
        EqualRect(&clip, &replacementClip) != FALSE &&
        previousCapture == target && GetCapture() == alternate;

    HydraGateCAdapterWindowStateV2 destroyedState{};
    destroyedState.struct_size = sizeof(destroyedState);
    destroyedState.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    destroyedState.process_id = GetCurrentProcessId();
    destroyedState.target_window = runtimeWindowValue(alternate);
    destroyedState.logical_foreground_window = destroyedState.target_window;
    destroyedState.logical_active_window = destroyedState.target_window;
    destroyedState.logical_focus_window = destroyedState.target_window;
    destroyedState.virtual_capture_window = destroyedState.target_window;
    const bool destructionConfigured =
        hydra_gate_c_adapter_configure_window_state(
            adapter.get(), &destroyedState) == HYDRA_GATE_C_ADAPTER_OK;
    DestroyWindow(alternate);
    SetLastError(ERROR_SUCCESS);
    const bool staleRejected = destructionConfigured &&
        GetForegroundWindow() == nullptr &&
        GetLastError() == ERROR_INVALID_WINDOW_HANDLE &&
        GetActiveWindow() == nullptr && GetFocus() == nullptr &&
        GetCapture() == nullptr;

    HydraGateCAdapterWindowStateV2 restoredState{};
    restoredState.struct_size = sizeof(restoredState);
    restoredState.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    restoredState.process_id = GetCurrentProcessId();
    restoredState.target_window = runtimeWindowValue(target);
    restoredState.logical_foreground_window = restoredState.target_window;
    restoredState.logical_active_window = restoredState.target_window;
    restoredState.logical_focus_window = restoredState.target_window;
    const bool targetRestored = hydra_gate_c_adapter_configure_window_state(
        adapter.get(), &restoredState) == HYDRA_GATE_C_ADAPTER_OK;

    RECT invalidClip{5, 5, 5, 10};
    SetLastError(ERROR_SUCCESS);
    const bool invalidRejected = ClipCursor(&invalidClip) == FALSE &&
        GetLastError() == ERROR_INVALID_PARAMETER &&
        GetClipCursor(&clip) != FALSE &&
        EqualRect(&clip, &replacementClip) != FALSE;
    SetLastError(ERROR_SUCCESS);
    const bool foreignCaptureRejected =
        SetCapture(GetDesktopWindow()) == nullptr &&
        GetLastError() == ERROR_INVALID_WINDOW_HANDLE;
    RECT unclipped{};
    const bool releaseAndUnclip = targetRestored &&
        SetCapture(target) == nullptr && ReleaseCapture() != FALSE &&
        GetCapture() == nullptr && ClipCursor(nullptr) != FALSE &&
        GetClipCursor(&unclipped) != FALSE &&
        unclipped.left == (std::numeric_limits<LONG>::min)() &&
        unclipped.top == (std::numeric_limits<LONG>::min)() &&
        unclipped.right == (std::numeric_limits<LONG>::max)() &&
        unclipped.bottom == (std::numeric_limits<LONG>::max)();

    POINT nativeCursorAfter{};
    RECT nativeClipAfter{};
    const bool nativePreserved =
        nativeGetCursorPos(&nativeCursorAfter) != FALSE &&
        nativeGetClipCursor(&nativeClipAfter) != FALSE &&
        nativeCursorAfter.x == nativeCursorBefore.x &&
        nativeCursorAfter.y == nativeCursorBefore.y &&
        EqualRect(&nativeClipAfter, &nativeClipBefore) != FALSE &&
        nativeGetForegroundWindow() == nativeForegroundBefore &&
        nativeGetCapture() == nativeCaptureBefore;
    const bool restored = shim.uninstall();
    DestroyWindow(target);
    if (!initialQueries || !virtualMutations || !staleRejected ||
        !invalidRejected || !foreignCaptureRejected || !releaseAndUnclip ||
        !nativePreserved ||
        !restored) {
        return 33;
    }
    std::cout
        << "HydraSeat Gate C cursor/focus shim self-test passed: ordinary "
           "cursor, clip, logical focus, and virtual capture APIs used "
           "adapter state without mutating native global state.\n";
    return EXIT_SUCCESS;
}

int runLocalRawInputShimSelfTest(HINSTANCE instance,
                                 const std::wstring& shimPath) {
    UINT nativeCountBefore = 0;
    if (GetRegisteredRawInputDevices(
            nullptr, &nativeCountBefore, sizeof(RAWINPUTDEVICE)) ==
        static_cast<UINT>(-1)) {
        return 80;
    }
    const HWND window = CreateWindowExW(
        0, L"STATIC", L"HydraSeat local Raw Input shim test",
        WS_OVERLAPPED, 0, 0, 64, 64,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 81;
    const HWND routeWindowB = CreateWindowExW(
        0, L"STATIC", L"HydraSeat Raw route B", WS_OVERLAPPED,
        0, 0, 32, 32, nullptr, nullptr, instance, nullptr);
    if (routeWindowB == nullptr) {
        DestroyWindow(window);
        return 81;
    }
    AdapterOwner adapter;
    ShimOwner shim;
    if (!adapter || !shim.loadAndInstall(
                        shimPath, adapter.get(), 1, window, false, true) ||
        !shim.active()) {
        DestroyWindow(routeWindowB);
        DestroyWindow(window);
        return 82;
    }

    UINT emptyRegistrationCount = 99;
    bool passed = GetRegisteredRawInputDevices(
        nullptr, &emptyRegistrationCount, sizeof(RAWINPUTDEVICE)) == 0 &&
        emptyRegistrationCount == 0;

    RAWINPUTDEVICE devices[2]{};
    devices[0] = {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD,
                  RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window};
    devices[1] = {HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE,
                  RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window};
    passed = passed && RegisterRawInputDevices(
        devices, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
    UINT registeredCount = 0;
    passed = passed && GetRegisteredRawInputDevices(
        nullptr, &registeredCount, sizeof(RAWINPUTDEVICE)) == 0 &&
        registeredCount == 2;
    std::array<RAWINPUTDEVICE, 2> registered{};
    UINT capacity = static_cast<UINT>(registered.size());
    passed = passed && GetRegisteredRawInputDevices(
        registered.data(), &capacity, sizeof(RAWINPUTDEVICE)) == 2 &&
        capacity == 2 &&
        std::all_of(registered.begin(), registered.end(), [](const auto& value) {
            return value.dwFlags == RIDEV_INPUTSINK;
        });
    RAWINPUTDEVICE keyToWindowB{
        HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD,
        RIDEV_INPUTSINK, routeWindowB};
    passed = passed && RegisterRawInputDevices(
        &keyToWindowB, 1, sizeof(keyToWindowB)) != FALSE;
    capacity = static_cast<UINT>(registered.size());
    passed = passed && GetRegisteredRawInputDevices(
        registered.data(), &capacity, sizeof(RAWINPUTDEVICE)) == 2 &&
        std::any_of(registered.begin(), registered.end(),
                    [routeWindowB](const auto& value) {
                        return value.usUsage == HID_USAGE_GENERIC_KEYBOARD &&
                               value.hwndTarget == routeWindowB;
                    }) &&
        std::any_of(registered.begin(), registered.end(),
                    [window](const auto& value) {
                        return value.usUsage == HID_USAGE_GENERIC_MOUSE &&
                               value.hwndTarget == window;
                    });

    InputEventMessage keyMessage;
    keyMessage.kind = hydra::gatec::InputKind::Keyboard;
    keyMessage.keyTransition = hydra::gatec::KeyTransition::Down;
    keyMessage.vkey = 0x41;
    keyMessage.scanCode = 0x1e;
    auto key = toAdapterEvent(keyMessage);
    passed = passed && hydra_gate_c_adapter_apply_input(
        adapter.get(), 1, &key) == HYDRA_GATE_C_ADAPTER_OK &&
        shim.dispatchRawInput();
    MSG message{};
    passed = passed && PeekMessageW(
        &message, routeWindowB, WM_INPUT, WM_INPUT, PM_REMOVE) != FALSE;
    MSG unexpected{};
    passed = passed && PeekMessageW(
        &unexpected, window, WM_INPUT, WM_INPUT, PM_NOREMOVE) == FALSE;
    if (passed) {
        UINT bytes = 0;
        passed = GetRawInputData(
            reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT,
            nullptr, &bytes, sizeof(RAWINPUTHEADER)) == 0 &&
            bytes == sizeof(RAWINPUT);
        SetLastError(ERROR_SUCCESS);
        passed = passed && GetRawInputData(
            reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT,
            nullptr, nullptr, sizeof(RAWINPUTHEADER)) ==
                static_cast<UINT>(-1) &&
            GetLastError() == ERROR_INVALID_PARAMETER;
        UINT invalidBytes = bytes;
        SetLastError(ERROR_SUCCESS);
        passed = passed && GetRawInputData(
            reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT,
            nullptr, &invalidBytes, sizeof(RAWINPUTHEADER) - 1u) ==
                static_cast<UINT>(-1) &&
            GetLastError() == ERROR_INVALID_PARAMETER;
        invalidBytes = bytes;
        SetLastError(ERROR_SUCCESS);
        passed = passed && GetRawInputData(
            reinterpret_cast<HRAWINPUT>(message.lParam), 0u, nullptr,
            &invalidBytes, sizeof(RAWINPUTHEADER)) ==
                static_cast<UINT>(-1) &&
            GetLastError() == ERROR_INVALID_PARAMETER;
        std::vector<std::byte> shortStorage(bytes - 1u, std::byte{0x5a});
        const auto shortBefore = shortStorage;
        UINT shortBytes = bytes - 1u;
        SetLastError(ERROR_SUCCESS);
        passed = passed && GetRawInputData(
            reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT,
            shortStorage.data(), &shortBytes, sizeof(RAWINPUTHEADER)) ==
                static_cast<UINT>(-1) &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER &&
            shortBytes == bytes && shortStorage == shortBefore;
        std::vector<std::byte> storage(bytes);
        UINT readBytes = bytes;
        passed = passed && GetRawInputData(
            reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT,
            storage.data(), &readBytes, sizeof(RAWINPUTHEADER)) == bytes;
        if (passed) {
            RAWINPUT raw{};
            std::memcpy(&raw, storage.data(), sizeof(raw));
            passed = raw.header.dwType == RIM_TYPEKEYBOARD &&
                     raw.data.keyboard.VKey == 0x41 &&
                     raw.data.keyboard.MakeCode == 0x1e;
        }
        UINT consumedBytes = bytes;
        passed = passed && GetRawInputData(
            reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT,
            storage.data(), &consumedBytes, sizeof(RAWINPUTHEADER)) ==
                static_cast<UINT>(-1);
    }

    InputEventMessage mouseMessage;
    mouseMessage.kind = hydra::gatec::InputKind::Mouse;
    mouseMessage.deltaX = 5;
    mouseMessage.deltaY = 7;
    mouseMessage.mouseButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN |
                                    RI_MOUSE_WHEEL;
    mouseMessage.wheelDelta = 120;
    auto mouseEvent = toAdapterEvent(mouseMessage);
    passed = passed && hydra_gate_c_adapter_apply_input(
        adapter.get(), 2, &mouseEvent) == HYDRA_GATE_C_ADAPTER_OK &&
        shim.dispatchRawInput();
    passed = passed && PeekMessageW(
        &message, window, WM_INPUT, WM_INPUT, PM_REMOVE) != FALSE;
    if (passed) {
        UINT bytes = 0;
        passed = GetRawInputBuffer(
            nullptr, &bytes, sizeof(RAWINPUTHEADER)) == 0 &&
            bytes == sizeof(RAWINPUT);
        std::vector<std::byte> shortStorage(bytes - 1u, std::byte{0x6b});
        const auto shortBefore = shortStorage;
        UINT shortBytes = bytes - 1u;
        passed = passed && GetRawInputBuffer(
            reinterpret_cast<PRAWINPUT>(shortStorage.data()), &shortBytes,
            sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) &&
            shortBytes == bytes && shortStorage == shortBefore;
        std::vector<std::byte> storage(bytes);
        UINT readBytes = bytes;
        passed = passed && GetRawInputBuffer(
            reinterpret_cast<PRAWINPUT>(storage.data()), &readBytes,
            sizeof(RAWINPUTHEADER)) == 1;
        if (passed) {
            RAWINPUT raw{};
            std::memcpy(&raw, storage.data(), sizeof(raw));
            passed = raw.header.dwType == RIM_TYPEMOUSE &&
                     raw.data.mouse.lLastX == 5 &&
                     raw.data.mouse.lLastY == 7 &&
                     (raw.data.mouse.usButtonFlags &
                      RI_MOUSE_LEFT_BUTTON_DOWN) != 0;
        }
        UINT emptyBytes = 99;
        passed = passed && GetRawInputBuffer(
            nullptr, &emptyBytes, sizeof(RAWINPUTHEADER)) == 0 &&
            emptyBytes == 0;
    }

    RAWINPUTDEVICE removeKeyboard{
        HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD,
        RIDEV_REMOVE, nullptr};
    passed = passed && RegisterRawInputDevices(
        &removeKeyboard, 1, sizeof(removeKeyboard)) != FALSE;
    auto removedKey = toAdapterEvent(keyMessage);
    passed = passed && hydra_gate_c_adapter_apply_input(
        adapter.get(), 3, &removedKey) == HYDRA_GATE_C_ADAPTER_OK &&
        shim.dispatchRawInput() &&
        PeekMessageW(&unexpected, routeWindowB, WM_INPUT, WM_INPUT,
                     PM_NOREMOVE) == FALSE &&
        PeekMessageW(&unexpected, window, WM_INPUT, WM_INPUT,
                     PM_NOREMOVE) == FALSE;
    auto retainedMouse = toAdapterEvent(mouseMessage);
    passed = passed && hydra_gate_c_adapter_apply_input(
        adapter.get(), 4, &retainedMouse) == HYDRA_GATE_C_ADAPTER_OK &&
        shim.dispatchRawInput() &&
        PeekMessageW(&message, window, WM_INPUT, WM_INPUT, PM_REMOVE) != FALSE;
    if (passed) {
        UINT retainedBytes = 0;
        passed = GetRawInputBuffer(
            nullptr, &retainedBytes, sizeof(RAWINPUTHEADER)) == 0 &&
            retainedBytes == sizeof(RAWINPUT);
        std::vector<std::byte> retainedStorage(retainedBytes);
        UINT retainedCapacity = retainedBytes;
        passed = passed && GetRawInputBuffer(
            reinterpret_cast<PRAWINPUT>(retainedStorage.data()),
            &retainedCapacity, sizeof(RAWINPUTHEADER)) == 1;
    }
    UINT remainingCount = 0;
    passed = passed && GetRegisteredRawInputDevices(
        nullptr, &remainingCount, sizeof(RAWINPUTDEVICE)) == 0 &&
        remainingCount == 1;

    passed = shim.uninstall() && passed;
    DestroyWindow(routeWindowB);

    bool destroyedTargetRejected = false;
    const HWND doomedWindow = CreateWindowExW(
        0, L"STATIC", L"HydraSeat destroyed Raw target", WS_OVERLAPPED,
        0, 0, 32, 32, nullptr, nullptr, instance, nullptr);
    AdapterOwner failureAdapter;
    ShimOwner failureShim;
    if (doomedWindow != nullptr && failureAdapter &&
        failureShim.loadAndInstall(
            shimPath, failureAdapter.get(), 2, window, false, true) &&
        failureShim.active()) {
        RAWINPUTDEVICE doomedRegistration{
            HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD,
            RIDEV_INPUTSINK, doomedWindow};
        const bool doomedRegistered = RegisterRawInputDevices(
            &doomedRegistration, 1, sizeof(doomedRegistration)) != FALSE;
        const bool destroyed = DestroyWindow(doomedWindow) != FALSE;
        auto doomedEvent = toAdapterEvent(keyMessage);
        const bool enqueued = hydra_gate_c_adapter_apply_input(
            failureAdapter.get(), 1, &doomedEvent) ==
            HYDRA_GATE_C_ADAPTER_OK;
        destroyedTargetRejected = doomedRegistered && destroyed && enqueued &&
            !failureShim.dispatchRawInput();
        destroyedTargetRejected = failureShim.uninstall() &&
            destroyedTargetRejected;
    } else if (doomedWindow != nullptr) {
        DestroyWindow(doomedWindow);
    }
    passed = passed && destroyedTargetRejected;
    UINT nativeCountAfter = 0;
    passed = passed && GetRegisteredRawInputDevices(
        nullptr, &nativeCountAfter, sizeof(RAWINPUTDEVICE)) == 0 &&
        nativeCountAfter == nativeCountBefore;
    DestroyWindow(window);
    if (!passed) return 83;
    std::cout
        << "HydraSeat Gate C Raw Input shim self-test passed: ordinary "
           "registration, data, and buffer APIs consumed only bounded "
           "synthetic packets, a destroyed HWND failed visibly, and "
           "uninstall restored native registration.\n";
    return EXIT_SUCCESS;
}

int runLocalXInputSelfTest() {
    AdapterOwner adapter;
    if (!adapter || hydra_gate_c_adapter_api_version() != 4u) return 90;
    hydra::gatec::ControllerUpdateMessage update;
    update.seatId = 1u;
    update.kind = hydra::gatec::ControllerUpdateKind::Map;
    update.logicalSlot = 0u;
    update.source = {hydra::gatec::ControllerSourceKind::Synthetic,
                     0u, 0x58494e5055544101ull};
    update.sourceGeneration = 1u;
    auto mapping = toAdapterMapping(update);
    if (hydra_gate_c_adapter_xinput_map_slot(
            adapter.get(), 1u, &mapping) != HYDRA_GATE_C_ADAPTER_OK) {
        return 91;
    }
    update.kind = hydra::gatec::ControllerUpdateKind::State;
    update.gamepad.buttons = 0x1100u;
    update.gamepad.leftTrigger = 20u;
    update.gamepad.rightTrigger = 200u;
    update.gamepad.thumbLX = -12000;
    update.gamepad.thumbLY = 9000;
    const auto state = toAdapterXInputState(update);
    if (hydra_gate_c_adapter_xinput_apply_state(
            adapter.get(), 2u, &state) != HYDRA_GATE_C_ADAPTER_OK) {
        return 92;
    }
    update.kind = hydra::gatec::ControllerUpdateKind::Capabilities;
    update.gamepad = {};
    update.capabilities.subtype = 1u;
    update.capabilities.vibrationSupported = true;
    update.capabilities.leftMotorMaximum = 65535u;
    update.capabilities.rightMotorMaximum = 65535u;
    const auto capabilities = toAdapterXInputCapabilities(update);
    if (hydra_gate_c_adapter_xinput_apply_capabilities(
            adapter.get(), 3u, &capabilities) !=
        HYDRA_GATE_C_ADAPTER_OK) {
        return 93;
    }
    update.kind = hydra::gatec::ControllerUpdateKind::Battery;
    update.capabilities = {};
    update.battery = {
        true, hydra::gatec::XInputBatteryDeviceType::Gamepad,
        hydra::gatec::XInputBatteryType::Alkaline,
        hydra::gatec::XInputBatteryLevel::Full};
    const auto battery = toAdapterXInputBattery(update);
    if (hydra_gate_c_adapter_xinput_apply_battery(
            adapter.get(), 4u, &battery) != HYDRA_GATE_C_ADAPTER_OK) {
        return 94;
    }
    HydraGateCAdapterXInputStateV4 queried{};
    queried.struct_size = sizeof(queried);
    queried.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    if (hydra_gate_c_adapter_xinput_get_state(
            adapter.get(), 0u, &queried) != HYDRA_GATE_C_ADAPTER_OK ||
        queried.source_key != update.source.sourceKey ||
        queried.buttons != 0x1100u || queried.packet_number == 0u) {
        return 95;
    }
    HydraGateCAdapterXInputVibrationV4 vibration{};
    vibration.struct_size = sizeof(vibration);
    vibration.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    vibration.logical_slot = 0u;
    vibration.source_generation = queried.source_generation;
    vibration.mapping_generation = queried.mapping_generation;
    vibration.left_motor = 100u;
    vibration.right_motor = 200u;
    if (hydra_gate_c_adapter_xinput_route_vibration(
            adapter.get(), 5u, &vibration) != HYDRA_GATE_C_ADAPTER_OK ||
        vibration.source_key != update.source.sourceKey ||
        vibration.route_count != 1u) {
        return 96;
    }
    const auto disconnect = toAdapterXInputSource(update);
    if (hydra_gate_c_adapter_xinput_disconnect(
            adapter.get(), 6u, &disconnect) != HYDRA_GATE_C_ADAPTER_OK) {
        return 97;
    }
    queried = {};
    queried.struct_size = sizeof(queried);
    queried.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    if (hydra_gate_c_adapter_xinput_get_state(
            adapter.get(), 0u, &queried) !=
            HYDRA_GATE_C_ADAPTER_XINPUT_DISCONNECTED ||
        queried.buttons != 0u || queried.left_trigger != 0u ||
        queried.thumb_lx != 0) {
        return 98;
    }
    std::cout
        << "HydraSeat Gate C controlled XInput facade self-test passed: "
           "logical slot mapping, normalized state, metadata, vibration route, "
           "and disconnect clearing used only adapter ABI v4.\n";
    return EXIT_SUCCESS;
}

class ControlledProbe {
public:
    explicit ControlledProbe(ProbeOptions options)
        : m_options(std::move(options)) {}

    int run(HINSTANCE instance, int showCommand) {
        m_instance = instance;
        if (!m_adapter || hydra_gate_c_adapter_api_version() !=
                              HYDRA_GATE_C_ADAPTER_API_VERSION) {
            return 19;
        }
        if (!m_options.testMissingWindow && !createWindow(showCommand)) {
            return finish(20);
        }
        if (m_options.pollingShim && !m_options.testMissingWindow &&
            !m_shim.loadAndInstall(m_options.shimPath, m_adapter.get(),
                                   m_options.seatId, m_hwnd.load(),
                                   m_options.cursorFocusShim,
                                   m_options.rawInputShim)) {
            destroyWindow();
            return 18;
        }
        if (m_options.rawInputShim && !m_options.testMissingWindow &&
            !initializeRawInput()) {
            destroyWindow();
            return finish(28);
        }
        if (m_options.testNoHandshake) {
            Sleep(2000);
            destroyWindow();
            return finish(78);
        }
        if (!connectAndHandshake()) {
            destroyWindow();
            return finish(21);
        }
        if (m_options.testAbnormalExit) {
            destroyWindow();
            return finish(77);
        }

        m_reader = std::jthread(
            [this](std::stop_token token) { readerLoop(token); });
        MSG message{};
        while (true) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result == 0) break;
            if (result == -1) {
                m_exitCode.store(25);
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        m_stopping.store(true);
        m_reader.request_stop();
        m_reader.join();
        m_channel.close();
        return finish(m_exitCode.load());
    }

private:
    int finish(int exitCode) noexcept {
        if (m_options.pollingShim && !m_shim.uninstall()) return 27;
        return exitCode;
    }

    struct CaptureRequest {
        std::mutex mutex;
        std::condition_variable condition;
        std::uint64_t sequence{0};
        std::uint16_t probeVkey{0xffffu};
        std::optional<ProbeComparison> comparison;
        bool cancelled{false};
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message,
                                       WPARAM wParam, LPARAM lParam) {
        ControlledProbe* self = reinterpret_cast<ControlledProbe*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<ControlledProbe*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
            if (self != nullptr) self->m_hwnd.store(hwnd);
        }
        if (self != nullptr) {
            return self->windowMessage(hwnd, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT windowMessage(HWND hwnd, UINT message, WPARAM wParam,
                          LPARAM lParam) {
        (void)wParam;
        switch (message) {
        case kCaptureRequestMessage:
            handleCaptureRequest(
                reinterpret_cast<CaptureRequest*>(lParam), hwnd);
            return 0;
        case kStateChangedMessage:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_INPUT:
            handleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
            return 0;
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_CLOSE:
            m_stopping.store(true);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            m_hwnd.store(nullptr);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    bool createWindow(int showCommand) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = ControlledProbe::WindowProc;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kWindowClass;
        if (RegisterClassExW(&windowClass) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        const std::wstring title = L"HydraSeat Gate C API Probe - Seat " +
            std::to_wstring(m_options.seatId);
        const DWORD style = m_options.headless
            ? WS_OVERLAPPEDWINDOW
            : WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        const HWND window = CreateWindowExW(
            WS_EX_APPWINDOW, kWindowClass, title.c_str(), style,
            CW_USEDEFAULT, CW_USEDEFAULT, 760, 580,
            nullptr, nullptr, m_instance, this);
        if (window == nullptr) return false;
        m_hwnd.store(window);
        if (!m_options.headless) {
            ShowWindow(window, showCommand);
            UpdateWindow(window);
        }
        return true;
    }

    void destroyWindow() noexcept {
        if (const HWND window = m_hwnd.exchange(nullptr); window != nullptr) {
            DestroyWindow(window);
        }
    }

    std::vector<RAWINPUTDEVICE> queryRawRegistrations(bool& succeeded) {
        succeeded = false;
        UINT count = 0;
        SetLastError(ERROR_SUCCESS);
        if (GetRegisteredRawInputDevices(
                nullptr, &count, sizeof(RAWINPUTDEVICE)) ==
            static_cast<UINT>(-1)) {
            m_rawInputSnapshot.lastSystemError = GetLastError();
            return {};
        }
        std::vector<RAWINPUTDEVICE> values(count);
        UINT capacity = count;
        if (count != 0 && GetRegisteredRawInputDevices(
                              values.data(), &capacity,
                              sizeof(RAWINPUTDEVICE)) ==
                              static_cast<UINT>(-1)) {
            m_rawInputSnapshot.lastSystemError = GetLastError();
            return {};
        }
        values.resize(capacity);
        succeeded = true;
        return values;
    }

    bool initializeRawInput() {
        m_rawInputSnapshot = {};
        m_rawInputSnapshot.enabled = true;
        const HWND windowA = m_hwnd.load();
        if (windowA == nullptr) return false;
        const HWND windowB = CreateWindowExW(
            0, L"STATIC", L"HydraSeat Raw registration B", WS_OVERLAPPED,
            0, 0, 32, 32, nullptr, nullptr, m_instance, nullptr);
        const HWND windowC = CreateWindowExW(
            0, L"STATIC", L"HydraSeat Raw registration C", WS_OVERLAPPED,
            0, 0, 32, 32, nullptr, nullptr, m_instance, nullptr);
        if (windowB == nullptr || windowC == nullptr) {
            if (windowB != nullptr) DestroyWindow(windowB);
            if (windowC != nullptr) DestroyWindow(windowC);
            return false;
        }
        const auto registerOne = [](USHORT usage, DWORD flags,
                                    HWND target) {
            RAWINPUTDEVICE device{};
            device.usUsagePage = HID_USAGE_PAGE_GENERIC;
            device.usUsage = usage;
            device.dwFlags = flags;
            device.hwndTarget = target;
            return RegisterRawInputDevices(
                       &device, 1, sizeof(device)) != FALSE;
        };
        const auto removeOne = [&](USHORT usage) {
            return registerOne(usage, RIDEV_REMOVE, nullptr);
        };
        const auto findUsage = [](const std::vector<RAWINPUTDEVICE>& values,
                                  USHORT usage) -> const RAWINPUTDEVICE* {
            const auto found = std::find_if(
                values.begin(), values.end(), [usage](const auto& value) {
                    return value.usUsagePage == HID_USAGE_PAGE_GENERIC &&
                           value.usUsage == usage;
                });
            return found == values.end() ? nullptr : &*found;
        };

        bool ok = registerOne(
            HID_USAGE_GENERIC_KEYBOARD,
            RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, windowA) &&
            registerOne(HID_USAGE_GENERIC_MOUSE, RIDEV_DEVNOTIFY, windowA);
        bool queried = false;
        auto values = queryRawRegistrations(queried);
        const auto* initialKeyboard = findUsage(
            values, HID_USAGE_GENERIC_KEYBOARD);
        const auto* initialMouse = findUsage(values, HID_USAGE_GENERIC_MOUSE);
        ok = ok && queried && values.size() == 2 &&
            initialKeyboard != nullptr && initialMouse != nullptr &&
            initialKeyboard->dwFlags == RIDEV_INPUTSINK &&
            initialMouse->dwFlags == 0;

        ok = ok && registerOne(HID_USAGE_GENERIC_KEYBOARD, 0, windowB);
        values = queryRawRegistrations(queried);
        const auto* replacedKeyboard = findUsage(
            values, HID_USAGE_GENERIC_KEYBOARD);
        const auto* retainedMouse = findUsage(values, HID_USAGE_GENERIC_MOUSE);
        ok = ok && queried && replacedKeyboard != nullptr &&
            retainedMouse != nullptr &&
            replacedKeyboard->hwndTarget == windowB &&
            retainedMouse->hwndTarget == windowA;

        ok = ok && removeOne(HID_USAGE_GENERIC_KEYBOARD);
        values = queryRawRegistrations(queried);
        ok = ok && queried && values.size() == 1 &&
            findUsage(values, HID_USAGE_GENERIC_MOUSE) != nullptr;

        ok = ok && registerOne(HID_USAGE_GENERIC_KEYBOARD, 0, windowB);
        const auto destroyedValue = windowB;
        ok = ok && DestroyWindow(windowB) != FALSE;
        values = queryRawRegistrations(queried);
        const auto* staleKeyboard = findUsage(
            values, HID_USAGE_GENERIC_KEYBOARD);
        ok = ok && queried && staleKeyboard != nullptr &&
            staleKeyboard->hwndTarget == destroyedValue;

        ok = ok && registerOne(HID_USAGE_GENERIC_KEYBOARD, 0, windowC);
        values = queryRawRegistrations(queried);
        const auto* freshKeyboard = findUsage(
            values, HID_USAGE_GENERIC_KEYBOARD);
        ok = ok && queried && freshKeyboard != nullptr &&
            freshKeyboard->hwndTarget == windowC;

        RAWINPUTDEVICE finalDevices[2]{};
        finalDevices[0] = {HID_USAGE_PAGE_GENERIC,
                           HID_USAGE_GENERIC_KEYBOARD,
                           RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, windowA};
        finalDevices[1] = {HID_USAGE_PAGE_GENERIC,
                           HID_USAGE_GENERIC_MOUSE,
                           RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, windowA};
        ok = ok && RegisterRawInputDevices(
                       finalDevices, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
        values = queryRawRegistrations(queried);
        const auto* finalKeyboard = findUsage(
            values, HID_USAGE_GENERIC_KEYBOARD);
        const auto* finalMouse = findUsage(values, HID_USAGE_GENERIC_MOUSE);
        ok = ok && queried && values.size() == 2 &&
            finalKeyboard != nullptr && finalMouse != nullptr &&
            finalKeyboard->hwndTarget == windowA &&
            finalMouse->hwndTarget == windowA &&
            finalKeyboard->dwFlags == RIDEV_INPUTSINK &&
            finalMouse->dwFlags == RIDEV_INPUTSINK;
        DestroyWindow(windowC);
        m_rawInputSnapshot.registrationLifecyclePassed = ok;
        m_rawInputSnapshot.registrationQueryPassed = queried;
        m_rawInputSnapshot.registeredCount =
            static_cast<std::uint32_t>(values.size());
        if (!ok) {
            ++m_rawInputSnapshot.apiFailures;
            m_rawInputSnapshot.lastSystemError = GetLastError();
        }
        return ok;
    }

    void recordRawFailure() {
        ++m_rawInputSnapshot.apiFailures;
        m_rawInputSnapshot.lastSystemError = GetLastError();
    }

    void countRawPacket(const RAWINPUT& raw, bool fromBuffer) {
        if (raw.header.dwType == RIM_TYPEKEYBOARD) {
            const USHORT expected = m_options.seatId == 1 ? 0x41u : 0x42u;
            if (raw.data.keyboard.VKey == expected) {
                ++m_rawInputSnapshot.keyboardExpected;
            } else {
                ++m_rawInputSnapshot.keyboardCross;
            }
        } else if (raw.header.dwType == RIM_TYPEMOUSE) {
            const bool expected = m_options.seatId == 1
                ? ((raw.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0 &&
                   raw.data.mouse.lLastX == 5 &&
                   raw.data.mouse.lLastY == 7 &&
                   static_cast<SHORT>(raw.data.mouse.usButtonData) == 120)
                : ((raw.data.mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0 &&
                   raw.data.mouse.lLastX == -8 &&
                   raw.data.mouse.lLastY == -9 &&
                   static_cast<SHORT>(raw.data.mouse.usButtonData) == -120);
            if (expected) {
                ++m_rawInputSnapshot.mouseExpected;
            } else {
                ++m_rawInputSnapshot.mouseCross;
            }
        }
        if (fromBuffer) ++m_rawInputSnapshot.bufferPackets;
    }

    void handleRawInput(HRAWINPUT handle) {
        if (!m_options.rawInputShim) return;
        UINT required = 0;
        if (GetRawInputData(handle, RID_HEADER, nullptr, &required,
                            sizeof(RAWINPUTHEADER)) ==
                static_cast<UINT>(-1) ||
            required != sizeof(RAWINPUTHEADER)) {
            recordRawFailure();
            return;
        }
        RAWINPUTHEADER header{};
        UINT headerBytes = sizeof(header);
        if (GetRawInputData(handle, RID_HEADER, &header, &headerBytes,
                            sizeof(RAWINPUTHEADER)) != sizeof(header)) {
            recordRawFailure();
            return;
        }
        m_rawInputSnapshot.dataQueryPassed = true;
        if (header.dwType == RIM_TYPEKEYBOARD) {
            UINT bytes = 0;
            if (GetRawInputData(handle, RID_INPUT, nullptr, &bytes,
                                sizeof(RAWINPUTHEADER)) ==
                    static_cast<UINT>(-1) ||
                bytes < sizeof(RAWINPUTHEADER) ||
                bytes > hydra::gatec::kVirtualRawMaximumPayloadBytes) {
                recordRawFailure();
                return;
            }
            std::vector<std::byte> storage(bytes);
            UINT capacity = bytes;
            if (GetRawInputData(handle, RID_INPUT, storage.data(), &capacity,
                                sizeof(RAWINPUTHEADER)) != bytes ||
                capacity != bytes) {
                recordRawFailure();
                return;
            }
            RAWINPUT raw{};
            std::memcpy(&raw, storage.data(),
                        (std::min)(storage.size(), sizeof(raw)));
            countRawPacket(raw, false);
            ++m_rawInputSnapshot.dataReads;
            return;
        }

        UINT bufferBytes = 0;
        if (GetRawInputBuffer(nullptr, &bufferBytes,
                              sizeof(RAWINPUTHEADER)) ==
                static_cast<UINT>(-1) ||
            bufferBytes == 0 ||
            bufferBytes > hydra::gatec::kVirtualRawMaximumPayloadBytes) {
            recordRawFailure();
            return;
        }
        std::vector<std::byte> storage(bufferBytes);
        UINT capacity = bufferBytes;
        const UINT packetCount = GetRawInputBuffer(
            reinterpret_cast<PRAWINPUT>(storage.data()), &capacity,
            sizeof(RAWINPUTHEADER));
        if (packetCount == static_cast<UINT>(-1) || packetCount == 0) {
            recordRawFailure();
            return;
        }
        std::size_t offset = 0;
        for (UINT index = 0; index < packetCount; ++index) {
            if (offset + sizeof(RAWINPUTHEADER) > storage.size()) {
                recordRawFailure();
                return;
            }
            RAWINPUT raw{};
            std::memcpy(&raw, storage.data() + offset,
                        (std::min)(sizeof(raw), storage.size() - offset));
            if (raw.header.dwSize < sizeof(RAWINPUTHEADER) ||
                raw.header.dwSize > storage.size() - offset) {
                recordRawFailure();
                return;
            }
            countRawPacket(raw, true);
            const auto next =
                (static_cast<std::size_t>(raw.header.dwSize) + 7u) & ~7u;
            if (next == 0 || next > storage.size() - offset) {
                recordRawFailure();
                return;
            }
            offset += next;
        }
        m_rawInputSnapshot.bufferReadPassed = true;
    }

    bool connectAndHandshake() {
        std::string error;
        std::uint32_t systemError = 0;
        m_channel = hydra::gatec::connectGateCClient(
            m_options.pipeName, 10000, &error, &systemError);
        if (!m_channel.valid()) return false;

        HelloMessage hello;
        hello.token = m_options.token;
        hello.seatId = m_options.seatId;
        hello.processId = GetCurrentProcessId();
        const auto architecture = hydra::gatec::detectProcessArchitecture(
            GetCurrentProcess());
        if (!architecture) return false;
        hello.architectureBits =
            static_cast<std::uint16_t>(architecture.architecture);
        hello.targetWindow = runtimeWindowValue(m_hwnd.load());
        if (!m_channel.writeFrame(hydra::gatec::encodeHello(1, hello),
                                  kIoTimeoutMs, &error, &systemError)) {
            return false;
        }
        const auto response = m_channel.readFrame(kIoTimeoutMs);
        if (!response || !response.frame) return false;
        HelloAckMessage ack;
        if (!hydra::gatec::decodeHelloAck(*response.frame, ack, &error) ||
            response.frame->sequence != 1 || !ack.accepted) {
            return false;
        }
        const auto requiredCapabilities = m_options.xinputControlled
            ? hydra::gatec::kControlledXInputProbeCapabilities
            : (m_options.rawInputShim
            ? hydra::gatec::kControlledRawInputProbeCapabilities
            : (m_options.cursorFocusShim
            ? hydra::gatec::kControlledCursorFocusProbeCapabilities
            : (m_options.pollingShim
                   ? hydra::gatec::kControlledPollingProbeCapabilities
                   : hydra::gatec::kControlledApiProbeCapabilities)));
        if ((ack.grantedCapabilities &
             hydra::gatec::testCapabilityBits(requiredCapabilities)) !=
            hydra::gatec::testCapabilityBits(requiredCapabilities)) {
            return false;
        }
        m_connected.store(true);
        return true;
    }

    void readerLoop(std::stop_token stopToken) {
        while (!stopToken.stop_requested() && !m_stopping.load()) {
            const auto result = m_channel.readFrame(kReadPollMs);
            if (result.status == TransportStatus::Timeout) continue;
            if (!result || !result.frame) {
                if (!m_shutdownReceived.load()) {
                    m_shim.markUnavailable();
                    m_exitCode.store(24);
                }
                break;
            }
            if (!processFrame(*result.frame)) {
                m_shim.markUnavailable();
                m_exitCode.store(23);
                break;
            }
        }
        m_stopping.store(true);
        if (const HWND window = m_hwnd.load(); window != nullptr) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    }

    bool controllerMappingMatches(
        const hydra::gatec::ControllerUpdateMessage& message) {
        HydraGateCAdapterXInputStateV4 state{};
        state.struct_size = sizeof(state);
        state.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
        const auto result = hydra_gate_c_adapter_xinput_get_state(
            m_adapter.get(), message.logicalSlot, &state);
        return (result == HYDRA_GATE_C_ADAPTER_OK ||
                result == HYDRA_GATE_C_ADAPTER_XINPUT_DISCONNECTED) &&
               state.source_kind ==
                   static_cast<std::uint8_t>(message.source.kind) &&
               state.runtime_xinput_slot_hint ==
                   message.source.runtimeXInputSlotHint &&
               state.source_key == message.source.sourceKey;
    }

    HydraGateCAdapterResult applyControllerUpdate(
        std::uint64_t sequence,
        const hydra::gatec::ControllerUpdateMessage& message) {
        using hydra::gatec::ControllerUpdateKind;
        if (message.kind == ControllerUpdateKind::Map) {
            auto value = toAdapterMapping(message);
            return hydra_gate_c_adapter_xinput_map_slot(
                m_adapter.get(), sequence, &value);
        }
        if (message.kind == ControllerUpdateKind::Unmap) {
            return hydra_gate_c_adapter_xinput_unmap_slot(
                m_adapter.get(), sequence, message.logicalSlot);
        }
        if (!controllerMappingMatches(message)) {
            return HYDRA_GATE_C_ADAPTER_XINPUT_MAPPING_MISMATCH;
        }
        if (message.kind == ControllerUpdateKind::State) {
            const auto value = toAdapterXInputState(message);
            return hydra_gate_c_adapter_xinput_apply_state(
                m_adapter.get(), sequence, &value);
        }
        if (message.kind == ControllerUpdateKind::Capabilities) {
            const auto value = toAdapterXInputCapabilities(message);
            return hydra_gate_c_adapter_xinput_apply_capabilities(
                m_adapter.get(), sequence, &value);
        }
        if (message.kind == ControllerUpdateKind::Battery) {
            const auto value = toAdapterXInputBattery(message);
            return hydra_gate_c_adapter_xinput_apply_battery(
                m_adapter.get(), sequence, &value);
        }
        if (message.kind == ControllerUpdateKind::Disconnect) {
            const auto value = toAdapterXInputSource(message);
            return hydra_gate_c_adapter_xinput_disconnect(
                m_adapter.get(), sequence, &value);
        }
        return HYDRA_GATE_C_ADAPTER_INVALID_STATE;
    }

    hydra::gatec::ControllerSnapshotMessage controllerSnapshot(
        std::uint64_t sequence,
        const hydra::gatec::ControllerQueryMessage& query) {
        using namespace hydra::gatec;
        ControllerSnapshotMessage result;
        result.seatId = m_options.seatId;
        result.logicalSlot = query.logicalSlot;

        if (query.kind == ControllerQueryKind::Vibration) {
            HydraGateCAdapterXInputVibrationV4 vibration{};
            vibration.struct_size = sizeof(vibration);
            vibration.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
            vibration.logical_slot = query.logicalSlot;
            vibration.source_generation =
                query.expectedSourceGeneration;
            vibration.mapping_generation =
                query.expectedMappingGeneration;
            vibration.left_motor = query.leftMotor;
            vibration.right_motor = query.rightMotor;
            const auto vibrationResult =
                hydra_gate_c_adapter_xinput_route_vibration(
                    m_adapter.get(), sequence, &vibration);
            result.vibrationResult =
                toVirtualXInputResult(vibrationResult);
            if (vibrationResult == HYDRA_GATE_C_ADAPTER_OK) {
                result.vibration.logicalSlot = vibration.logical_slot;
                result.vibration.source = {
                    static_cast<ControllerSourceKind>(
                        vibration.source_kind),
                    vibration.runtime_xinput_slot_hint,
                    vibration.source_key};
                result.vibration.sourceGeneration =
                    vibration.source_generation;
                result.vibration.mappingGeneration =
                    vibration.mapping_generation;
                result.vibration.commandSequence =
                    vibration.command_sequence;
                result.vibration.routeCount = vibration.route_count;
                result.vibration.leftMotor = vibration.left_motor;
                result.vibration.rightMotor = vibration.right_motor;
            }
        } else {
            result.vibrationResult = VirtualXInputResult::Disconnected;
        }

        HydraGateCAdapterXInputStateV4 state{};
        state.struct_size = sizeof(state);
        state.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
        const auto stateResult = hydra_gate_c_adapter_xinput_get_state(
            m_adapter.get(), query.logicalSlot, &state);
        result.stateResult = toVirtualXInputResult(stateResult);
        if (stateResult == HYDRA_GATE_C_ADAPTER_OK ||
            stateResult == HYDRA_GATE_C_ADAPTER_XINPUT_DISCONNECTED) {
            result.state.mapping.logicalSlot = state.logical_slot;
            result.state.mapping.source = {
                static_cast<ControllerSourceKind>(state.source_kind),
                state.runtime_xinput_slot_hint, state.source_key};
            result.state.mapping.sourceGeneration =
                state.source_generation;
            result.state.mapping.mappingGeneration =
                state.mapping_generation;
            result.state.connected = state.connected != 0;
            result.state.packetNumber = state.packet_number;
            result.state.gamepad = {
                state.buttons, state.left_trigger, state.right_trigger,
                state.thumb_lx, state.thumb_ly, state.thumb_rx,
                state.thumb_ry};
        }

        HydraGateCAdapterXInputCapabilitiesV4 capabilities{};
        capabilities.struct_size = sizeof(capabilities);
        capabilities.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
        const auto capabilitiesResult =
            hydra_gate_c_adapter_xinput_get_capabilities(
                m_adapter.get(), query.logicalSlot, &capabilities);
        result.capabilitiesResult =
            toVirtualXInputResult(capabilitiesResult);
        if (capabilitiesResult == HYDRA_GATE_C_ADAPTER_OK) {
            result.capabilities.mapping = result.state.mapping;
            result.capabilities.capabilities.type =
                static_cast<XInputCapabilityType>(capabilities.type);
            result.capabilities.capabilities.subtype =
                capabilities.subtype;
            result.capabilities.capabilities.flags = capabilities.flags;
            result.capabilities.capabilities.gamepad = {
                capabilities.buttons, capabilities.left_trigger,
                capabilities.right_trigger, capabilities.thumb_lx,
                capabilities.thumb_ly, capabilities.thumb_rx,
                capabilities.thumb_ry};
            result.capabilities.capabilities.vibrationSupported =
                capabilities.vibration_supported != 0;
            result.capabilities.capabilities.leftMotorMaximum =
                capabilities.left_motor_maximum;
            result.capabilities.capabilities.rightMotorMaximum =
                capabilities.right_motor_maximum;
        }

        HydraGateCAdapterXInputBatteryV4 battery{};
        battery.struct_size = sizeof(battery);
        battery.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
        const auto batteryResult =
            hydra_gate_c_adapter_xinput_get_battery(
                m_adapter.get(), query.logicalSlot, &battery);
        result.batteryResult = toVirtualXInputResult(batteryResult);
        if (batteryResult == HYDRA_GATE_C_ADAPTER_OK) {
            result.battery.mapping = result.state.mapping;
            result.battery.battery = {
                battery.available != 0,
                static_cast<XInputBatteryDeviceType>(battery.device_type),
                static_cast<XInputBatteryType>(battery.battery_type),
                static_cast<XInputBatteryLevel>(battery.battery_level)};
        }
        return result;
    }

    bool processFrame(const DecodedFrame& frame) {
        std::string error;
        if (frame.type == MessageType::InputEvent) {
            InputEventMessage message;
            if (!hydra::gatec::decodeInputEvent(frame, message, &error)) {
                return sendError(frame.sequence, 1001);
            }
            const auto event = toAdapterEvent(message);
            const auto result = hydra_gate_c_adapter_apply_input(
                m_adapter.get(), frame.sequence, &event);
            if (result != HYDRA_GATE_C_ADAPTER_OK) {
                return sendError(frame.sequence,
                    1100u + static_cast<std::uint32_t>(result));
            }
            if (m_options.rawInputShim && !m_shim.dispatchRawInput()) {
                return sendError(frame.sequence, 1108);
            }

            // The host treats an InputEvent as applied only after the receiving
            // process proves that the exact protocol sequence is visible in its
            // adapter state. Keep the API-probe target on the same receipt
            // contract as hydra_gate_c_target so process self-tests cannot pass
            // on host-side routing intent alone.
            HydraGateCAdapterSnapshotV1 adapterSnapshot{};
            adapterSnapshot.struct_size = sizeof(adapterSnapshot);
            const auto snapshotResult = hydra_gate_c_adapter_get_snapshot(
                m_adapter.get(), HYDRA_GATE_C_ADAPTER_NO_PROBE_KEY,
                &adapterSnapshot);
            if (snapshotResult != HYDRA_GATE_C_ADAPTER_OK) {
                return sendError(frame.sequence,
                    1150u + static_cast<std::uint32_t>(snapshotResult));
            }
            if (adapterSnapshot.last_applied_sequence != frame.sequence) {
                return sendError(frame.sequence, 1199u);
            }
            if (!m_channel.writeFrame(
                    hydra::gatec::encodeStateSnapshot(
                        frame.sequence, fromAdapterSnapshot(adapterSnapshot)),
                    kIoTimeoutMs)) {
                return false;
            }
            notifyStateChanged();
            return true;
        }
        if (frame.type == MessageType::ControlState) {
            ControlStateMessage message;
            if (!hydra::gatec::decodeControlState(frame, message, &error)) {
                return sendError(frame.sequence, 1003);
            }
            const auto control = toAdapterControl(message);
            const auto result = hydra_gate_c_adapter_apply_control(
                m_adapter.get(), frame.sequence, &control);
            if (result != HYDRA_GATE_C_ADAPTER_OK) {
                return sendError(frame.sequence,
                    1200u + static_cast<std::uint32_t>(result));
            }
            if (m_options.cursorFocusShim) {
                RECT clip{message.clipLeft, message.clipTop,
                          message.clipRight, message.clipBottom};
                const BOOL clipResult = ClipCursor(
                    message.clipEnabled ? &clip : nullptr);
                const BOOL cursorResult = SetCursorPos(
                    message.cursorX, message.cursorY);
                const BOOL captureResult = message.virtualCapture
                    ? (SetCapture(m_hwnd.load()), TRUE)
                    : ReleaseCapture();
                if (clipResult == FALSE || cursorResult == FALSE ||
                    captureResult == FALSE) {
                    return sendError(frame.sequence, 1207);
                }
            }
            notifyStateChanged();
            return true;
        }
        if (frame.type == MessageType::QuerySnapshot) {
            QuerySnapshotMessage query;
            if (!hydra::gatec::decodeQuerySnapshot(frame, query, &error)) {
                return sendError(frame.sequence, 1005);
            }
            const auto comparison = requestUiCapture(
                frame.sequence, query.probeVkey);
            if (!comparison) return sendError(frame.sequence, 1401);
            const auto payload =
                hydra::gatec::encodeProbeComparison(*comparison);
            const auto response = hydra::gatec::encodeFrame(
                MessageType::ProbeSnapshot, frame.sequence, payload);
            if (payload.empty() || response.empty()) {
                return sendError(frame.sequence, 1402);
            }
            return m_channel.writeFrame(response, kIoTimeoutMs);
        }
        if (frame.type == MessageType::ControllerUpdate) {
            hydra::gatec::ControllerUpdateMessage message;
            if (!m_options.xinputControlled ||
                !hydra::gatec::decodeControllerUpdate(
                    frame, message, &error) ||
                !hydra::gatec::controllerSeatAuthorityMatches(
                    m_options.seatId, message.seatId)) {
                return sendError(frame.sequence, 1501);
            }
            const auto result = applyControllerUpdate(
                frame.sequence, message);
            if (result != HYDRA_GATE_C_ADAPTER_OK) {
                return sendError(
                    frame.sequence,
                    1500u + static_cast<std::uint32_t>(result));
            }
            notifyStateChanged();
            return true;
        }
        if (frame.type == MessageType::ControllerQuery) {
            hydra::gatec::ControllerQueryMessage query;
            if (!m_options.xinputControlled ||
                !hydra::gatec::decodeControllerQuery(
                    frame, query, &error) ||
                !hydra::gatec::controllerSeatAuthorityMatches(
                    m_options.seatId, query.seatId)) {
                return sendError(frame.sequence, 1502);
            }
            const auto snapshot = controllerSnapshot(
                frame.sequence, query);
            const auto response = hydra::gatec::encodeControllerSnapshot(
                frame.sequence, snapshot);
            if (response.empty()) {
                return sendError(frame.sequence, 1503);
            }
            return m_channel.writeFrame(response, kIoTimeoutMs);
        }
        if (frame.type == MessageType::Shutdown) {
            if (!hydra::gatec::decodeShutdown(frame, &error)) {
                return sendError(frame.sequence, 1006);
            }
            m_shutdownReceived.store(true);
            m_stopping.store(true);
            return true;
        }
        return sendError(frame.sequence, 1099);
    }

    std::optional<ProbeComparison> requestUiCapture(
        std::uint64_t sequence, std::uint16_t probeVkey) {
        const HWND window = m_hwnd.load();
        if (window == nullptr) return std::nullopt;
        auto request = std::make_shared<CaptureRequest>();
        request->sequence = sequence;
        request->probeVkey = probeVkey;
        {
            std::scoped_lock lock(m_captureMapMutex);
            m_captureRequests.emplace(request.get(), request);
        }
        if (!PostMessageW(window, kCaptureRequestMessage, 0,
                          reinterpret_cast<LPARAM>(request.get()))) {
            std::scoped_lock lock(m_captureMapMutex);
            m_captureRequests.erase(request.get());
            return std::nullopt;
        }

        std::unique_lock lock(request->mutex);
        const bool ready = request->condition.wait_for(
            lock, std::chrono::milliseconds(kUiCaptureTimeoutMs), [&] {
                return request->comparison.has_value() || request->cancelled;
            });
        if (!ready || !request->comparison) {
            request->cancelled = true;
            lock.unlock();
            std::scoped_lock mapLock(m_captureMapMutex);
            m_captureRequests.erase(request.get());
            return std::nullopt;
        }
        return request->comparison;
    }

    void handleCaptureRequest(CaptureRequest* key, HWND window) {
        std::shared_ptr<CaptureRequest> request;
        {
            std::scoped_lock lock(m_captureMapMutex);
            const auto found = m_captureRequests.find(key);
            if (found == m_captureRequests.end()) return;
            request = found->second;
            m_captureRequests.erase(found);
        }
        const auto comparison = captureComparison(
            m_adapter.get(), request->sequence, m_options.seatId,
            request->probeVkey, window,
            m_options.rawInputShim ? &m_rawInputSnapshot : nullptr);
        if (m_options.pollingShim && !m_shim.active()) {
            std::scoped_lock lock(request->mutex);
            request->cancelled = true;
            request->condition.notify_all();
            return;
        }
        {
            std::scoped_lock lock(request->mutex);
            if (!request->cancelled) request->comparison = comparison;
        }
        request->condition.notify_all();
        {
            std::scoped_lock lock(m_lastComparisonMutex);
            m_lastComparison = comparison;
        }
        InvalidateRect(window, nullptr, FALSE);
    }

    bool sendError(std::uint64_t sequence, std::uint32_t code) {
        std::string ignored;
        (void)m_channel.writeFrame(
            hydra::gatec::encodeError(sequence, ErrorMessage{code}),
            kIoTimeoutMs, &ignored, nullptr);
        return false;
    }

    void notifyStateChanged() {
        if (const HWND window = m_hwnd.load(); window != nullptr) {
            PostMessageW(window, kStateChangedMessage, 0, 0);
        }
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH background = CreateSolidBrush(RGB(15, 23, 42));
        FillRect(dc, &client, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(226, 232, 240));

        std::optional<ProbeComparison> last;
        {
            std::scoped_lock lock(m_lastComparisonMutex);
            last = m_lastComparison;
        }
        std::wostringstream text;
        text << L"HydraSeat Gate C API Baseline — Seat "
             << m_options.seatId
             << L"\n\nREAD-ONLY BASELINE — NO API INTERPOSITION INSTALLED"
             << L"\n\nProcess ID: " << GetCurrentProcessId();
        if (last) {
            text << L"\nProbe UI thread ID: " << last->threadId
                 << L"\nSequence: " << last->sequence
                 << L"\nProbe key: " << last->probeVkey
                 << L"\nOS GetAsyncKeyState: 0x" << std::hex
                 << static_cast<std::uint16_t>(last->os.asyncKeyState)
                 << L"\nOS GetKeyState: 0x"
                 << static_cast<std::uint16_t>(last->os.keyState)
                 << L"\nAdapter async state: 0x"
                 << last->adapter.asyncKeyState
                 << L"\nAdapter key state: 0x"
                 << last->adapter.keyState << std::dec
                 << L"\nOS cursor: (" << last->os.cursorX << L", "
                 << last->os.cursorY << L")"
                 << L"\nAdapter cursor: (" << last->adapter.cursorX
                 << L", " << last->adapter.cursorY << L")"
                 << L"\nOS foreground is target: "
                 << (last->osForegroundIsTarget ? L"true" : L"false")
                 << L"\nAdapter virtual foreground: "
                 << (last->adapter.virtualForeground ? L"true" : L"false")
                 << L"\nForeground views match: "
                 << (last->foregroundMatches ? L"true" : L"false")
                 << L"\nOS focus is target: "
                 << (last->osFocusIsTarget ? L"true" : L"false")
                 << L"\nOS capture is target: "
                 << (last->osCaptureIsTarget ? L"true" : L"false")
                 << L"\nAdapter virtual capture: "
                 << (last->adapter.virtualCapture ? L"true" : L"false");
        }
        auto content = text.str();
        client.left += 24;
        client.top += 20;
        client.right -= 24;
        DrawTextW(dc, content.c_str(), -1, &client,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        EndPaint(hwnd, &paint);
    }

    ProbeOptions m_options;
    HINSTANCE m_instance{nullptr};
    std::atomic<HWND> m_hwnd{nullptr};
    PipeChannel m_channel;
    AdapterOwner m_adapter;
    ShimOwner m_shim;
    std::jthread m_reader;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_shutdownReceived{false};
    std::atomic<int> m_exitCode{0};
    std::mutex m_captureMapMutex;
    std::unordered_map<CaptureRequest*, std::shared_ptr<CaptureRequest>>
        m_captureRequests;
    std::mutex m_lastComparisonMutex;
    std::optional<ProbeComparison> m_lastComparison;
    hydra::gatec::RawInputApiSnapshot m_rawInputSnapshot;
};

#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    bool valid = false;
    const auto options = parseOptions(argc, argv, valid);
    if (!valid || options.showHelp) {
        printUsage(valid ? std::cout : std::cerr);
        return valid ? EXIT_SUCCESS : 2;
    }
    if (options.baselineSelfTest && options.pipeName.empty()) {
        return runLocalBaselineSelfTest(GetModuleHandleW(nullptr));
    }
    if (options.pollingShimSelfTest) {
        return runLocalPollingShimSelfTest(
            GetModuleHandleW(nullptr), options.shimPath);
    }
    if (options.cursorFocusShimSelfTest) {
        return runLocalCursorFocusShimSelfTest(
            GetModuleHandleW(nullptr), options.shimPath);
    }
    if (options.rawInputShimSelfTest) {
        return runLocalRawInputShimSelfTest(
            GetModuleHandleW(nullptr), options.shimPath);
    }
    if (options.xinputSelfTest) {
        return runLocalXInputSelfTest();
    }
    ControlledProbe probe(options);
    return probe.run(GetModuleHandleW(nullptr), SW_SHOWNORMAL);
}
#else
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    printUsage(std::cerr);
    return 3;
}
#endif
