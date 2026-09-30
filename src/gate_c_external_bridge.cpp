#include "hydra/gate_c_external_profile.hpp"

#include "hydra/gate_c_adapter.h"
#include "hydra/gate_c_protocol.hpp"
#include "hydra/gate_c_shim_api.h"
#include "hydra/gate_c_transport.hpp"

#ifdef _WIN32

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using hydra::gatec::DecodedFrame;
using hydra::gatec::ExternalBridgeConfigV1;
using hydra::gatec::InputEventMessage;
using hydra::gatec::MessageType;
using hydra::gatec::PipeChannel;

constexpr wchar_t kBridgeWindowClass[] = L"HydraSeat.P3E.ExternalBridge.Window";
constexpr std::uint32_t kIoTimeoutMs = 250;

LRESULT CALLBACK bridgeWindowProc(HWND window, UINT message,
                                  WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND createBootstrapWindow() {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = bridgeWindowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kBridgeWindowClass;
    const ATOM atom = RegisterClassExW(&cls);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    return CreateWindowExW(0, kBridgeWindowClass, L"HydraSeat P3-E bridge",
                           WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr,
                           cls.hInstance, nullptr);
}

struct WindowSearch {
    DWORD processId{0};
    HWND bootstrap{nullptr};
    HWND found{nullptr};
};

BOOL CALLBACK findWindowCallback(HWND window, LPARAM opaque) {
    auto& search = *reinterpret_cast<WindowSearch*>(opaque);
    DWORD processId = 0;
    if (window == search.bootstrap ||
        GetWindowThreadProcessId(window, &processId) == 0 ||
        processId != search.processId || IsWindowVisible(window) == FALSE ||
        GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    search.found = window;
    return FALSE;
}

HWND findApplicationWindow(HWND bootstrap) {
    WindowSearch search{GetCurrentProcessId(), bootstrap, nullptr};
    (void)EnumWindows(findWindowCallback,
                      reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool readConfig(ExternalBridgeConfigV1& config) {
    const auto name = hydra::gatec::externalBridgeMappingName(
        GetCurrentProcessId());
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (mapping == nullptr) return false;
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(config));
    if (view == nullptr) {
        CloseHandle(mapping);
        return false;
    }
    std::memcpy(&config, view, sizeof(config));
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    if (config.structSize != sizeof(config) ||
        config.magic != hydra::gatec::kExternalBridgeConfigMagic ||
        config.version != hydra::gatec::kExternalBridgeConfigVersion ||
        config.seatId == 0 || config.reserved0 != 0 ||
        !hydra::gatec::validProfiledShimMask(config.requiredApiMask) ||
        config.pipeName[0] == L'\0' ||
        config.pipeName[hydra::gatec::kExternalBridgePipeNameChars - 1] !=
            L'\0') {
        return false;
    }
    return true;
}

HydraGateCAdapterInputEventV1 adapterInput(
    const InputEventMessage& input) {
    HydraGateCAdapterInputEventV1 value{};
    value.struct_size = sizeof(value);
    value.kind = static_cast<std::uint8_t>(
        input.kind == hydra::gatec::InputKind::Keyboard
            ? HYDRA_GATE_C_ADAPTER_INPUT_KEYBOARD
            : HYDRA_GATE_C_ADAPTER_INPUT_MOUSE);
    switch (input.keyTransition) {
    case hydra::gatec::KeyTransition::Down:
        value.key_transition = static_cast<std::uint8_t>(
            HYDRA_GATE_C_ADAPTER_KEY_DOWN);
        break;
    case hydra::gatec::KeyTransition::Up:
        value.key_transition = static_cast<std::uint8_t>(
            HYDRA_GATE_C_ADAPTER_KEY_UP);
        break;
    default:
        value.key_transition = static_cast<std::uint8_t>(
            HYDRA_GATE_C_ADAPTER_KEY_NONE);
        break;
    }
    value.is_touchpad = input.isTouchpad ? 1u : 0u;
    value.timestamp_micros = input.timestampMicros;
    value.vkey = input.vkey;
    value.scan_code = input.scanCode;
    value.keyboard_flags = input.keyboardFlags;
    value.delta_x = input.deltaX;
    value.delta_y = input.deltaY;
    value.mouse_button_flags = input.mouseButtonFlags;
    value.wheel_delta = input.wheelDelta;
    return value;
}

HydraGateCAdapterControlStateV1 adapterControl(
    const hydra::gatec::ControlStateMessage& input) {
    HydraGateCAdapterControlStateV1 value{};
    value.struct_size = sizeof(value);
    value.cursor_x = input.cursorX;
    value.cursor_y = input.cursorY;
    value.clip_enabled = input.clipEnabled ? 1u : 0u;
    value.virtual_foreground = input.virtualForeground ? 1u : 0u;
    value.virtual_capture = input.virtualCapture ? 1u : 0u;
    value.clip_left = input.clipLeft;
    value.clip_top = input.clipTop;
    value.clip_right = input.clipRight;
    value.clip_bottom = input.clipBottom;
    return value;
}

bool configureWindow(HydraGateCAdapterHandle adapter, HWND target,
                     bool foreground, bool capture) {
    if (adapter == nullptr || target == nullptr) return false;
    HydraGateCAdapterWindowStateV2 state{};
    state.struct_size = sizeof(state);
    state.api_version = HYDRA_GATE_C_ADAPTER_API_VERSION;
    state.process_id = GetCurrentProcessId();
    state.target_window = reinterpret_cast<std::uint64_t>(target);
    if (foreground) {
        state.logical_foreground_window =
            reinterpret_cast<std::uint64_t>(target);
        state.logical_active_window = reinterpret_cast<std::uint64_t>(target);
        state.logical_focus_window = reinterpret_cast<std::uint64_t>(target);
    }
    if (capture) {
        state.virtual_capture_window = reinterpret_cast<std::uint64_t>(target);
    }
    return hydra_gate_c_adapter_configure_window_state(adapter, &state) ==
           HYDRA_GATE_C_ADAPTER_OK;
}

bool writeStateSnapshot(PipeChannel& channel, std::uint64_t sequence,
                        HydraGateCAdapterHandle adapter,
                        std::uint16_t probeVkey) {
    HydraGateCAdapterSnapshotV1 native{};
    native.struct_size = sizeof(native);
    if (hydra_gate_c_adapter_get_snapshot(adapter, probeVkey, &native) !=
        HYDRA_GATE_C_ADAPTER_OK) {
        return false;
    }
    hydra::gatec::StateSnapshotMessage snapshot{};
    snapshot.lastAppliedSequence = native.last_applied_sequence;
    std::copy(std::begin(native.key_down_bits), std::end(native.key_down_bits),
              snapshot.keyDownBits.begin());
    std::copy(std::begin(native.key_pressed_edge_bits),
              std::end(native.key_pressed_edge_bits),
              snapshot.keyPressedEdgeBits.begin());
    snapshot.mouseButtonsDown = native.mouse_buttons_down;
    snapshot.wheelAccumulator = native.wheel_accumulator;
    snapshot.probeVkey = native.probe_vkey;
    snapshot.asyncKeyStateValue = native.async_key_state_value;
    snapshot.keyboardStateByte = native.keyboard_state_byte;
    snapshot.cursorX = native.cursor_x;
    snapshot.cursorY = native.cursor_y;
    snapshot.clipEnabled = native.clip_enabled != 0;
    snapshot.virtualForeground = native.virtual_foreground != 0;
    snapshot.virtualCapture = native.virtual_capture != 0;
    snapshot.clipLeft = native.clip_left;
    snapshot.clipTop = native.clip_top;
    snapshot.clipRight = native.clip_right;
    snapshot.clipBottom = native.clip_bottom;
    std::string error;
    return channel.writeFrame(
        hydra::gatec::encodeStateSnapshot(sequence, snapshot),
        kIoTimeoutMs, &error);
}

DWORD WINAPI bridgeWorker(void*) {
    ExternalBridgeConfigV1 config{};
    if (!readConfig(config)) return 10;

    HWND bootstrap = createBootstrapWindow();
    if (bootstrap == nullptr) return 11;

    HydraGateCAdapterHandle adapter = hydra_gate_c_adapter_create();
    if (adapter == nullptr) {
        DestroyWindow(bootstrap);
        return 12;
    }

    HydraGateCShimConfigV3 shim{};
    shim.struct_size = sizeof(shim);
    shim.api_version = HYDRA_GATE_C_SHIM_API_VERSION;
    shim.seat_id = config.seatId;
    shim.process_id = GetCurrentProcessId();
    shim.required_api_mask = config.requiredApiMask;
    shim.target_window = reinterpret_cast<std::uint64_t>(bootstrap);
    if ((config.requiredApiMask & HYDRA_GATE_C_SHIM_CURSOR_FOCUS_API_MASK) != 0) {
        shim.flags |= HYDRA_GATE_C_SHIM_ENABLE_CURSOR_FOCUS;
    }
    if ((config.requiredApiMask & HYDRA_GATE_C_SHIM_RAW_INPUT_API_MASK) != 0) {
        shim.flags |= HYDRA_GATE_C_SHIM_ENABLE_RAW_INPUT;
    }

    if (hydra_gate_c_shim_install_v3(adapter, &shim) !=
        HYDRA_GATE_C_SHIM_OK) {
        hydra_gate_c_adapter_destroy(adapter);
        DestroyWindow(bootstrap);
        return 13;
    }

    std::string error;
    auto channel = hydra::gatec::connectGateCClient(
        config.pipeName, 5000, &error);
    if (!channel.valid()) {
        (void)hydra_gate_c_shim_uninstall();
        hydra_gate_c_adapter_destroy(adapter);
        DestroyWindow(bootstrap);
        return 14;
    }

    hydra::gatec::HelloMessage hello{};
    hello.token = config.token;
    hello.seatId = config.seatId;
    hello.processId = GetCurrentProcessId();
    hello.architectureBits = static_cast<std::uint16_t>(sizeof(void*) * 8u);
    hello.targetWindow = reinterpret_cast<std::uint64_t>(bootstrap);
    if (!channel.writeFrame(hydra::gatec::encodeHello(1, hello), 5000,
                            &error)) {
        (void)hydra_gate_c_shim_uninstall();
        hydra_gate_c_adapter_destroy(adapter);
        DestroyWindow(bootstrap);
        return 15;
    }
    const auto ackFrame = channel.readFrame(5000);
    hydra::gatec::HelloAckMessage ack{};
    if (!ackFrame || !ackFrame.frame ||
        !hydra::gatec::decodeHelloAck(*ackFrame.frame, ack, &error) ||
        !ack.accepted) {
        (void)hydra_gate_c_shim_uninstall();
        hydra_gate_c_adapter_destroy(adapter);
        DestroyWindow(bootstrap);
        return 16;
    }

    HWND applicationWindow = nullptr;
    bool virtualForeground = true;
    bool virtualCapture = false;
    std::uint64_t lastSequence = 1;
    bool rawDiagnosticWritten = false;
    bool running = true;
    while (running) {
        HWND discovered = findApplicationWindow(bootstrap);
        if (discovered != nullptr && discovered != applicationWindow) {
            applicationWindow = discovered;
            if (!configureWindow(adapter, applicationWindow,
                                 virtualForeground, virtualCapture)) {
                break;
            }
        }

        const auto read = channel.readFrame(kIoTimeoutMs);
        if (read.status == hydra::gatec::TransportStatus::Timeout) continue;
        if (!read || !read.frame) break;
        const DecodedFrame& frame = *read.frame;
        if (frame.sequence <= lastSequence) break;
        lastSequence = frame.sequence;

        if (frame.type == MessageType::InputEvent) {
            InputEventMessage input{};
            if (!hydra::gatec::decodeInputEvent(frame, input, &error)) break;
            const auto native = adapterInput(input);
            if (hydra_gate_c_adapter_apply_input(adapter, frame.sequence,
                                                 &native) !=
                HYDRA_GATE_C_ADAPTER_OK) {
                break;
            }
            if ((config.requiredApiMask & HYDRA_GATE_C_SHIM_RAW_INPUT_API_MASK) != 0) {
                if (!rawDiagnosticWritten &&
                    input.kind == hydra::gatec::InputKind::Mouse) {
                    std::uint32_t registrationCount = 0;
                    const auto registrationResult =
                        hydra_gate_c_adapter_raw_get_registered(
                            adapter, nullptr, &registrationCount);
                    std::fprintf(stderr,
                                 "HydraSeat P3-E bridge seat=%u raw_registrations=%u raw_query_result=%u\n",
                                 config.seatId, registrationCount,
                                 static_cast<unsigned>(registrationResult));
                    std::fflush(stderr);
                    rawDiagnosticWritten = true;
                }
                const auto dispatchResult = hydra_gate_c_shim_dispatch_raw_input();
                if (dispatchResult != HYDRA_GATE_C_SHIM_OK) {
                    std::fprintf(stderr,
                                 "HydraSeat P3-E bridge seat=%u raw_dispatch_result=%u\n",
                                 config.seatId,
                                 static_cast<unsigned>(dispatchResult));
                    std::fflush(stderr);
                    break;
                }
            }
            continue;
        }
        if (frame.type == MessageType::ControlState) {
            hydra::gatec::ControlStateMessage control{};
            if (!hydra::gatec::decodeControlState(frame, control, &error)) break;
            virtualForeground = control.virtualForeground;
            virtualCapture = control.virtualCapture;
            const auto native = adapterControl(control);
            if (hydra_gate_c_adapter_apply_control(adapter, frame.sequence,
                                                   &native) !=
                HYDRA_GATE_C_ADAPTER_OK) {
                break;
            }
            HWND target = applicationWindow != nullptr ? applicationWindow
                                                       : bootstrap;
            if (!configureWindow(adapter, target, virtualForeground,
                                 virtualCapture)) {
                break;
            }
            continue;
        }
        if (frame.type == MessageType::QuerySnapshot) {
            hydra::gatec::QuerySnapshotMessage query{};
            if (!hydra::gatec::decodeQuerySnapshot(frame, query, &error) ||
                !writeStateSnapshot(channel, frame.sequence, adapter,
                                    query.probeVkey)) {
                break;
            }
            continue;
        }
        if (frame.type == MessageType::Shutdown) {
            if (!hydra::gatec::decodeShutdown(frame, &error)) break;
            running = false;
            continue;
        }
        break;
    }

    (void)hydra_gate_c_shim_mark_adapter_unavailable();
    const bool restored = hydra_gate_c_shim_uninstall() ==
                          HYDRA_GATE_C_SHIM_OK;
    if (applicationWindow != nullptr && IsWindow(applicationWindow) != FALSE) {
        (void)PostMessageW(applicationWindow, WM_CLOSE, 0, 0);
    }
    channel.close();
    hydra_gate_c_adapter_destroy(adapter);
    DestroyWindow(bootstrap);
    return restored ? 0 : 17;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE worker = CreateThread(nullptr, 0, bridgeWorker, nullptr, 0, nullptr);
        if (worker == nullptr) return FALSE;
        CloseHandle(worker);
    }
    return TRUE;
}

#else

int hydra_gate_c_external_bridge_non_windows_stub = 0;

#endif
