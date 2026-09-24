#include "hydra/production_activation_bridges.hpp"

#include "hydra/production_input_authority.hpp"
#include "production_gate_c_bridge_protocol.hpp"

#include "hydra/crash_journal.hpp"
#include "hydra/gate_c_adapter.h"
#include "hydra/gate_c_architecture.hpp"
#include "hydra/gate_c_external_profile.hpp"
#include "hydra/gate_c_recovery.hpp"
#include "hydra/gate_c_shim_api.h"
#include "hydra/gate_c_transport.hpp"
#include "hydra/hidhide_session_recovery.hpp"
#include "hydra/input_router.hpp"
#include "hydra/reset_actions.hpp"
#include "hydra/watchdog_protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(HYDRA_PRODUCTION_GATE_C_BRIDGE_BUILD) && defined(_WIN32)

namespace {

using hydra::production::detail::ProductionBridgeDllState;
using hydra::production::detail::ProductionBridgeMappingV1;
constexpr wchar_t kBridgeWindowClass[] = L"HydraSeat.ProductionGateC.Window";
constexpr std::uint32_t kBridgeIoTimeoutMs = 250u;

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
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    return CreateWindowExW(0, kBridgeWindowClass,
                           L"HydraSeat production Gate C bridge",
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
    (void)EnumWindows(findWindowCallback, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

struct MappingView {
    HANDLE handle{nullptr};
    ProductionBridgeMappingV1* value{nullptr};
    MappingView() = default;
    MappingView(const MappingView&) = delete;
    MappingView& operator=(const MappingView&) = delete;
    MappingView(MappingView&& other) noexcept
        : handle(std::exchange(other.handle, nullptr)),
          value(std::exchange(other.value, nullptr)) {}
    ~MappingView() {
        if (value != nullptr) UnmapViewOfFile(value);
        if (handle != nullptr) CloseHandle(handle);
    }
};

std::optional<MappingView> openBridgeMapping() {
    MappingView result;
    const auto name = hydra::production::detail::productionBridgeMappingName(
        GetCurrentProcessId());
    result.handle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                     name.c_str());
    if (result.handle == nullptr) return std::nullopt;
    result.value = static_cast<ProductionBridgeMappingV1*>(
        MapViewOfFile(result.handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                      sizeof(ProductionBridgeMappingV1)));
    if (result.value == nullptr) return std::nullopt;
    const auto& config = *result.value;
    if (config.structSize != sizeof(config) ||
        config.magic != hydra::production::detail::kProductionBridgeMappingMagic ||
        config.version != hydra::production::detail::kProductionBridgeMappingVersion ||
        config.seatId == 0 || config.reserved0 != 0 || config.reserved1 != 0 ||
        !hydra::gatec::validProfiledShimMask(config.requiredApiMask) ||
        config.pipeName[0] == L'\0' ||
        config.pipeName[hydra::production::detail::kProductionBridgePipeNameChars - 1u] != L'\0') {
        return std::nullopt;
    }
    return result;
}

HydraGateCAdapterInputEventV1 adapterInput(
    const hydra::gatec::InputEventMessage& input) {
    HydraGateCAdapterInputEventV1 value{};
    value.struct_size = sizeof(value);
    value.kind = static_cast<std::uint8_t>(
        input.kind == hydra::gatec::InputKind::Keyboard
            ? HYDRA_GATE_C_ADAPTER_INPUT_KEYBOARD
            : HYDRA_GATE_C_ADAPTER_INPUT_MOUSE);
    switch (input.keyTransition) {
        case hydra::gatec::KeyTransition::Down:
            value.key_transition = HYDRA_GATE_C_ADAPTER_KEY_DOWN;
            break;
        case hydra::gatec::KeyTransition::Up:
            value.key_transition = HYDRA_GATE_C_ADAPTER_KEY_UP;
            break;
        default:
            value.key_transition = HYDRA_GATE_C_ADAPTER_KEY_NONE;
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
        state.logical_foreground_window = reinterpret_cast<std::uint64_t>(target);
        state.logical_active_window = reinterpret_cast<std::uint64_t>(target);
        state.logical_focus_window = reinterpret_cast<std::uint64_t>(target);
    }
    if (capture) state.virtual_capture_window = reinterpret_cast<std::uint64_t>(target);
    return hydra_gate_c_adapter_configure_window_state(adapter, &state) ==
           HYDRA_GATE_C_ADAPTER_OK;
}

bool writeStateSnapshot(hydra::gatec::PipeChannel& channel,
                        std::uint64_t sequence,
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
        kBridgeIoTimeoutMs, &error);
}

DWORD WINAPI bridgeWorker(void*) {
    auto mapping = openBridgeMapping();
    if (!mapping) return 10u;
    auto* state = mapping->value;
    hydra::production::detail::publishBridgeState(
        state, ProductionBridgeDllState::Starting, 0, false);

    HWND bootstrap = createBootstrapWindow();
    if (bootstrap == nullptr) {
        hydra::production::detail::publishBridgeState(
            state, ProductionBridgeDllState::Failed, 11, false);
        return 11u;
    }
    HydraGateCAdapterHandle adapter = hydra_gate_c_adapter_create();
    if (adapter == nullptr) {
        DestroyWindow(bootstrap);
        hydra::production::detail::publishBridgeState(
            state, ProductionBridgeDllState::Failed, 12, false);
        return 12u;
    }

    HydraGateCShimConfigV3 shim{};
    shim.struct_size = sizeof(shim);
    shim.api_version = HYDRA_GATE_C_SHIM_API_VERSION;
    shim.seat_id = state->seatId;
    shim.process_id = GetCurrentProcessId();
    shim.required_api_mask = state->requiredApiMask;
    shim.target_window = reinterpret_cast<std::uint64_t>(bootstrap);
    if ((state->requiredApiMask & HYDRA_GATE_C_SHIM_CURSOR_FOCUS_API_MASK) != 0) {
        shim.flags |= HYDRA_GATE_C_SHIM_ENABLE_CURSOR_FOCUS;
    }
    if ((state->requiredApiMask & HYDRA_GATE_C_SHIM_RAW_INPUT_API_MASK) != 0) {
        shim.flags |= HYDRA_GATE_C_SHIM_ENABLE_RAW_INPUT;
    }
    if (hydra_gate_c_shim_install_v3(adapter, &shim) != HYDRA_GATE_C_SHIM_OK) {
        hydra_gate_c_adapter_destroy(adapter);
        DestroyWindow(bootstrap);
        hydra::production::detail::publishBridgeState(
            state, ProductionBridgeDllState::Failed, 13, false);
        return 13u;
    }

    std::string error;
    auto channel = hydra::gatec::connectGateCClient(state->pipeName, 5'000u, &error);
    if (!channel.valid()) {
        const bool restored = hydra_gate_c_shim_uninstall() == HYDRA_GATE_C_SHIM_OK;
        hydra_gate_c_adapter_destroy(adapter);
        DestroyWindow(bootstrap);
        hydra::production::detail::publishBridgeState(
            state, ProductionBridgeDllState::Failed, 14, restored);
        return 14u;
    }

    hydra::gatec::HelloMessage hello{};
    hello.token = state->token;
    hello.seatId = state->seatId;
    hello.processId = GetCurrentProcessId();
    hello.architectureBits = static_cast<std::uint16_t>(sizeof(void*) * 8u);
    hello.targetWindow = reinterpret_cast<std::uint64_t>(bootstrap);
    if (!channel.writeFrame(hydra::gatec::encodeHello(1u, hello), 5'000u, &error)) {
        channel.close();
        const bool restored = hydra_gate_c_shim_uninstall() == HYDRA_GATE_C_SHIM_OK;
        hydra_gate_c_adapter_destroy(adapter);
        DestroyWindow(bootstrap);
        hydra::production::detail::publishBridgeState(
            state, ProductionBridgeDllState::Failed, 15, restored);
        return 15u;
    }
    const auto ackFrame = channel.readFrame(5'000u);
    hydra::gatec::HelloAckMessage ack{};
    if (!ackFrame || !ackFrame.frame ||
        !hydra::gatec::decodeHelloAck(*ackFrame.frame, ack, &error) ||
        !ack.accepted) {
        channel.close();
        const bool restored = hydra_gate_c_shim_uninstall() == HYDRA_GATE_C_SHIM_OK;
        hydra_gate_c_adapter_destroy(adapter);
        DestroyWindow(bootstrap);
        hydra::production::detail::publishBridgeState(
            state, ProductionBridgeDllState::Failed, 16, restored);
        return 16u;
    }

    hydra::production::detail::publishBridgeState(
        state, ProductionBridgeDllState::Active, 0, false);
    HWND applicationWindow = nullptr;
    bool virtualForeground = true;
    bool virtualCapture = false;
    std::uint64_t lastSequence = 1u;
    bool shutdownRequested = false;
    bool loopHealthy = true;
    while (!shutdownRequested) {
        HWND discovered = findApplicationWindow(bootstrap);
        if (discovered != nullptr && discovered != applicationWindow) {
            applicationWindow = discovered;
            if (!configureWindow(adapter, applicationWindow,
                                 virtualForeground, virtualCapture)) {
                loopHealthy = false;
                break;
            }
        }
        const auto read = channel.readFrame(kBridgeIoTimeoutMs);
        if (read.status == hydra::gatec::TransportStatus::Timeout) continue;
        if (!read || !read.frame || read.frame->sequence <= lastSequence) {
            loopHealthy = false;
            break;
        }
        const auto& frame = *read.frame;
        lastSequence = frame.sequence;
        if (frame.type == hydra::gatec::MessageType::InputEvent) {
            hydra::gatec::InputEventMessage input{};
            if (!hydra::gatec::decodeInputEvent(frame, input, &error)) {
                loopHealthy = false;
                break;
            }
            const auto native = adapterInput(input);
            if (hydra_gate_c_adapter_apply_input(adapter, frame.sequence, &native) !=
                HYDRA_GATE_C_ADAPTER_OK) {
                loopHealthy = false;
                break;
            }
            if ((state->requiredApiMask & HYDRA_GATE_C_SHIM_RAW_INPUT_API_MASK) != 0 &&
                hydra_gate_c_shim_dispatch_raw_input() != HYDRA_GATE_C_SHIM_OK) {
                loopHealthy = false;
                break;
            }
            continue;
        }
        if (frame.type == hydra::gatec::MessageType::ControlState) {
            hydra::gatec::ControlStateMessage control{};
            if (!hydra::gatec::decodeControlState(frame, control, &error)) {
                loopHealthy = false;
                break;
            }
            virtualForeground = control.virtualForeground;
            virtualCapture = control.virtualCapture;
            const auto native = adapterControl(control);
            if (hydra_gate_c_adapter_apply_control(adapter, frame.sequence, &native) !=
                HYDRA_GATE_C_ADAPTER_OK) {
                loopHealthy = false;
                break;
            }
            HWND target = applicationWindow != nullptr ? applicationWindow : bootstrap;
            if (!configureWindow(adapter, target, virtualForeground, virtualCapture)) {
                loopHealthy = false;
                break;
            }
            continue;
        }
        if (frame.type == hydra::gatec::MessageType::QuerySnapshot) {
            hydra::gatec::QuerySnapshotMessage query{};
            if (!hydra::gatec::decodeQuerySnapshot(frame, query, &error) ||
                !writeStateSnapshot(channel, frame.sequence, adapter, query.probeVkey)) {
                loopHealthy = false;
                break;
            }
            continue;
        }
        if (frame.type == hydra::gatec::MessageType::Shutdown) {
            if (!hydra::gatec::decodeShutdown(frame, &error)) {
                loopHealthy = false;
                break;
            }
            shutdownRequested = true;
            continue;
        }
        loopHealthy = false;
        break;
    }

    hydra::production::detail::publishBridgeState(
        state, ProductionBridgeDllState::Stopping, loopHealthy ? 0 : 20, false);
    (void)hydra_gate_c_shim_mark_adapter_unavailable();
    const bool restored = hydra_gate_c_shim_uninstall() == HYDRA_GATE_C_SHIM_OK;
    channel.close();
    hydra_gate_c_adapter_destroy(adapter);
    DestroyWindow(bootstrap);
    hydra::production::detail::publishBridgeState(
        state,
        restored && shutdownRequested ? ProductionBridgeDllState::Stopped
                                      : ProductionBridgeDllState::Failed,
        restored ? (loopHealthy ? 0 : 20) : 21,
        restored);
    return restored && shutdownRequested ? 0u : 21u;
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

namespace hydra::production {
namespace {

constexpr std::uint32_t kProcessRecoveryActionBase = 0x5100u;
constexpr std::uint32_t kHidHideRecoveryActionBase = 0x5200u;
constexpr std::uint64_t kHidHideResourceNamespace = 0x485950524f440000ull;
constexpr std::size_t kV1SeatLimit = runtime::kV1MaximumActiveSeats;

std::wstring foldedPath(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
#ifdef _WIN32
    if (!value.empty()) {
        std::wstring full(32768u, L'\0');
        const DWORD copied = GetFullPathNameW(value.c_str(),
                                              static_cast<DWORD>(full.size()),
                                              full.data(), nullptr);
        if (copied != 0 && copied < full.size()) {
            full.resize(copied);
            value = std::move(full);
        }
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    }
#else
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
#endif
    return value;
}

std::wstring foldedIdentity(std::wstring value) {
#ifdef _WIN32
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
#else
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
#endif
    return value;
}

bool exactPathAllowed(const process::ProcessIdentity& process,
                      const ProductionGateCProfile& profile) {
    const auto actual = foldedPath(process.executablePath);
    return std::any_of(profile.allowedProcessExecutablePaths.begin(),
                       profile.allowedProcessExecutablePaths.end(),
                       [&](const std::wstring& candidate) {
                           return foldedPath(candidate) == actual;
                       });
}

std::vector<std::wstring> assignedStableDeviceIds(
    const launch::SeatActivationPlan& plan) {
    std::vector<std::wstring> result;
    if (plan.target.requirements.keyboard) {
        result.insert(result.end(), plan.seat.keyboardIds.begin(),
                      plan.seat.keyboardIds.end());
    }
    if (plan.target.requirements.mouse) {
        result.insert(result.end(), plan.seat.mouseIds.begin(),
                      plan.seat.mouseIds.end());
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return foldedIdentity(left) < foldedIdentity(right);
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left,
                                                              const auto& right) {
                     return foldedIdentity(left) == foldedIdentity(right);
                 }), result.end());
    return result;
}

bool contextEpochMatchesPlan(const launch::SeatActivationPlan& plan,
                             const ProductionActivationContextSnapshot& snapshot,
                             const ProductionActivationContextHandle& context,
                             std::string& error) {
    if (!context || !snapshot.epoch.valid() || snapshot.epoch.seatId != plan.seatId ||
        snapshot.epoch.activationFingerprint != plan.fingerprint ||
        !context->validatesEpoch(snapshot.epoch)) {
        error = "production activation context does not match immutable Seat/session/game epoch";
        return false;
    }
    return true;
}

bool currentProcessContext(const launch::SeatActivationPlan& plan,
                           const ProductionActivationContextHandle& context,
                           ProductionProcessActivatedContext& processContext,
                           std::string& error) {
    if (!context) {
        error = "production activation context is unavailable";
        return false;
    }
    const auto snapshot = context->snapshot();
    if (!contextEpochMatchesPlan(plan, snapshot, context, error)) return false;
    if (snapshot.stage != ProductionActivationContextStage::ProcessActive ||
        !snapshot.process || !snapshot.process->valid() ||
        !context->validatesCurrentProcess(*snapshot.process)) {
        error = "exact Seat-owned process context is not currently authoritative";
        return false;
    }
    processContext = *snapshot.process;
    return true;
}

bool sameGateRequestAuthority(const ProductionGateCSessionRequest& left,
                              const ProductionGateCSessionRequest& right) noexcept {
    return left.epoch == right.epoch &&
           left.process.authoritativeProcess.sameInstance(
               right.process.authoritativeProcess) &&
           left.process.handoffGeneration == right.process.handoffGeneration &&
           left.seat.seatId == right.seat.seatId && left.profile == right.profile &&
           left.assignedStableDeviceIds == right.assignedStableDeviceIds;
}

std::filesystem::path seatRecoveryRoot(const std::filesystem::path& base,
                                       SeatId seatId) {
    return base / "production-activation" / ("seat-" + std::to_string(seatId));
}

std::filesystem::path currentExecutableSibling(std::wstring_view fileName) {
#ifdef _WIN32
    if (fileName.empty()) return {};
    std::vector<wchar_t> buffer(32768u, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0u || length >= buffer.size()) return {};
    std::filesystem::path executable(std::wstring(buffer.data(), length));
    return executable.parent_path() / std::filesystem::path(fileName);
#else
    (void)fileName;
    return {};
#endif
}

std::uint64_t hidHideResourceId(const ProductionActivationEpoch& epoch,
                                std::uint64_t recoveryEpoch) noexcept {
    std::uint64_t value = kHidHideResourceNamespace;
    value ^= (static_cast<std::uint64_t>(epoch.seatId) & 0xffu) << 40u;
    value ^= (epoch.seatGameGeneration & 0xffffu) << 16u;
    value ^= recoveryEpoch & 0xffffu;
    return value == 0 ? 1u : value;
}

std::uint32_t processActionId(SeatId seatId) noexcept {
    return kProcessRecoveryActionBase + seatId;
}

std::uint32_t hidHideActionId(SeatId seatId) noexcept {
    return kHidHideRecoveryActionBase + seatId;
}

watchdog::ProcessIdentity watchdogIdentity(
    const process::ProcessIdentity& identity) noexcept {
    return {identity.processId, identity.creationTime100ns};
}

bool processExactStillRunning(const watchdog::ProcessIdentity& identity) noexcept {
#ifdef _WIN32
    if (identity.processId == 0 || identity.creationTime100ns == 0) return false;
    watchdog::ProcessIdentity observed;
    std::uint32_t systemError = 0;
    return watchdog::queryProcessIdentity(identity.processId, observed, &systemError) &&
           observed == identity;
#else
    (void)identity;
    return false;
#endif
}

const ProductionGateCProfile* findProfile(
    const ProductionActivationBridgeConfig& config, std::string_view gameId) {
    const auto found = std::find_if(config.gateCProfiles.begin(),
                                    config.gateCProfiles.end(),
                                    [&](const ProductionGateCProfile& profile) {
                                        return profile.gameId == gameId;
                                    });
    return found == config.gateCProfiles.end() ? nullptr : &*found;
}

bool validateGateCProfile(const ProductionGateCProfile& profile,
                          std::string& error) {
    if (profile.gameId.empty() || profile.gameId.size() > launch::kMaximumGameIdBytes) {
        error = "production Gate-C profile has an invalid game ID";
        return false;
    }
    if (!profile.runtimeAttachApproved ||
        !gatec::validProfiledShimMask(profile.requiredApiMask)) {
        error = "production Gate-C profile does not explicitly authorize the runtime attach/API mask";
        return false;
    }
    if (profile.allowedProcessExecutablePaths.empty() ||
        profile.allowedProcessExecutablePaths.size() >
            kMaximumProductionGateCExecutablePaths ||
        profile.bridgeLibraryPath.empty()) {
        error = "production Gate-C profile lacks bounded exact process/bridge evidence";
        return false;
    }
    if (profile.physicalCloakingRequired &&
        (!profile.nativeHidHideMutationApproved ||
         profile.hidHideAllowedApplications.empty() ||
         profile.hidHideAllowedApplications.size() >
             kHidHideSessionMaxRequestedApplications ||
         profile.hidHideExpiryMilliseconds < kHidHideSessionMinExpiryMs ||
         profile.hidHideExpiryMilliseconds > kHidHideSessionMaxExpiryMs)) {
        error = "production physical input profile lacks explicit guarded HidHide policy";
        return false;
    }
    return true;
}

bool resolvePhysicalScope(
    const launch::SeatActivationPlan& plan,
    const ProductionActivationBridgeConfig& config,
    std::vector<std::wstring>& stableIds,
    std::vector<std::wstring>& instanceIds,
    std::string& error) {
    stableIds = assignedStableDeviceIds(plan);
    instanceIds.clear();
    if (stableIds.empty()) {
        error = "input-required Seat has no assigned keyboard/mouse identities";
        return false;
    }
    if (config.inputEvidenceClass != ProductionInputEvidenceClass::Physical ||
        !config.physicalAcceptanceEvidence) {
        error = "production physical input mutation requires typed Physical P3-HW evidence; Controlled/Synthetic evidence is non-authoritative";
        return false;
    }

    const auto& evidence = *config.physicalAcceptanceEvidence;
    std::set<std::wstring> expected;
    for (const auto& id : stableIds) expected.insert(foldedIdentity(id));
    std::set<std::wstring> matched;
    for (const auto& item : evidence.nativeScope()) {
        if (item.seatId != plan.seatId) continue;
        const auto stable = foldedIdentity(item.stableDeviceId);
        if (!expected.contains(stable)) continue;
        const bool keyboardAssigned = std::any_of(
            plan.seat.keyboardIds.begin(), plan.seat.keyboardIds.end(),
            [&](const auto& id) { return foldedIdentity(id) == stable; });
        const bool mouseAssigned = std::any_of(
            plan.seat.mouseIds.begin(), plan.seat.mouseIds.end(),
            [&](const auto& id) { return foldedIdentity(id) == stable; });
        if ((item.category == Phase3InputDeviceCategory::Keyboard && !keyboardAssigned) ||
            (item.category == Phase3InputDeviceCategory::Mouse && !mouseAssigned)) {
            error = "typed P3-HW device category does not match immutable Seat assignment";
            return false;
        }
        if (!matched.insert(stable).second) {
            error = "typed P3-HW evidence ambiguously maps one Seat device";
            return false;
        }
        instanceIds.push_back(item.deviceInstanceId);
    }
    if (matched != expected) {
        error = "typed P3-HW evidence does not exactly cover this Seat's assigned keyboard/mouse set";
        return false;
    }
    if (!validatePhase3HardwareAcceptanceEvidenceForSeatDevices(
            evidence, plan.seatId, instanceIds, 0u, &error)) {
        error = "typed P3-HW evidence failed fresh exact Seat-scoped device validation: " + error;
        return false;
    }
    return true;
}

std::optional<recovery::RecoveryProcessAttachmentRegistration>
makeRecoveryRegistration(
    const ProductionProcessActivatedContext& processContext,
    std::uint64_t recoveryEpoch,
    std::span<const watchdog::RollbackActionDescriptor> additionalActions,
    std::uint32_t leaseTimeoutMilliseconds,
    std::uint32_t rollbackTimeoutMilliseconds,
    std::uint32_t actionTimeoutMilliseconds,
    std::string& error) {
    if (!processContext.valid() || recoveryEpoch == 0) {
        error = "recovery registration requires current exact process authority";
        return std::nullopt;
    }
    const auto token = gatec::generateSessionToken();
    if (!token) {
        error = "cryptographic recovery lease session generation failed";
        return std::nullopt;
    }
    gatec::GateCRecoveryTarget target;
    target.actionId = processActionId(processContext.epoch.seatId);
    target.activationOrdinal = 1u;
    target.generation = recoveryEpoch;
    target.process = watchdogIdentity(processContext.authoritativeProcess);
    auto manifest = gatec::makeGateCRecoveryPlan(
        *token, recoveryEpoch, leaseTimeoutMilliseconds,
        rollbackTimeoutMilliseconds,
        std::span<const gatec::GateCRecoveryTarget>(&target, 1u), &error);
    if (!manifest) return std::nullopt;
    manifest->actions.front().timeoutMilliseconds = actionTimeoutMilliseconds;
    for (const auto& additional : additionalActions) {
        if (additional.generation != recoveryEpoch || additional.actionId == 0 ||
            additional.activationOrdinal <= 1u) {
            error = "additional recovery action is not bound to the exact recovery epoch/order";
            return std::nullopt;
        }
        manifest->actions.push_back(additional);
    }
    if (!watchdog::validateRollbackPlan(*manifest, &error)) return std::nullopt;

    recovery::RecoveryProcessAttachmentRegistration registration;
    registration.identity.seatId = processContext.epoch.seatId;
    registration.identity.hostSessionId = processContext.epoch.sessionId;
    registration.identity.sessionGeneration = processContext.epoch.sessionGeneration;
    registration.identity.seatGameGeneration = processContext.epoch.seatGameGeneration;
    registration.identity.process = watchdogIdentity(processContext.authoritativeProcess);
    registration.identity.recoveryEpoch = recoveryEpoch;
    registration.manifest = std::move(*manifest);
    if (!recovery::validateRecoveryProcessAttachmentRegistration(registration, &error)) {
        return std::nullopt;
    }
    return registration;
}

#ifdef _WIN32

class WatchdogProcessClient {
public:
    WatchdogProcessClient(std::filesystem::path executable,
                          std::uint32_t handshakeTimeoutMilliseconds)
        : executable_(std::move(executable)),
          handshakeTimeoutMilliseconds_(handshakeTimeoutMilliseconds) {}
    ~WatchdogProcessClient() { abandon(); }
    WatchdogProcessClient(const WatchdogProcessClient&) = delete;
    WatchdogProcessClient& operator=(const WatchdogProcessClient&) = delete;

    bool start(const watchdog::RollbackPlanManifest& manifest,
               const std::filesystem::path& recoveryDirectory,
               std::string& error) {
        abandon();
        if (executable_.empty() || !std::filesystem::is_regular_file(executable_)) {
            error = "production watchdog executable is unavailable";
            return false;
        }
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        HANDLE controlRead = nullptr, controlWrite = nullptr;
        HANDLE statusRead = nullptr, statusWrite = nullptr;
        if (CreatePipe(&controlRead, &controlWrite, &security, 0) == FALSE ||
            CreatePipe(&statusRead, &statusWrite, &security, 0) == FALSE) {
            if (controlRead) CloseHandle(controlRead);
            if (controlWrite) CloseHandle(controlWrite);
            if (statusRead) CloseHandle(statusRead);
            if (statusWrite) CloseHandle(statusWrite);
            error = "production watchdog pipe creation failed";
            return false;
        }
        if (SetHandleInformation(controlWrite, HANDLE_FLAG_INHERIT, 0) == FALSE ||
            SetHandleInformation(statusRead, HANDLE_FLAG_INHERIT, 0) == FALSE) {
            CloseHandle(controlRead); CloseHandle(controlWrite);
            CloseHandle(statusRead); CloseHandle(statusWrite);
            error = "production watchdog inherited-handle policy failed";
            return false;
        }
        watchdog::ProcessIdentity hostIdentity;
        std::uint32_t identityError = 0;
        if (!watchdog::queryProcessIdentity(GetCurrentProcessId(), hostIdentity,
                                            &identityError)) {
            CloseHandle(controlRead); CloseHandle(controlWrite);
            CloseHandle(statusRead); CloseHandle(statusWrite);
            error = "production watchdog could not bind exact Host process identity";
            return false;
        }
        auto quote = [](std::wstring_view value) {
            std::wstring result = L"\"";
            std::size_t slashes = 0;
            for (const wchar_t ch : value) {
                if (ch == L'\\') ++slashes;
                else if (ch == L'\"') {
                    result.append(slashes * 2u + 1u, L'\\');
                    result.push_back(L'\"');
                    slashes = 0;
                } else {
                    result.append(slashes, L'\\');
                    slashes = 0;
                    result.push_back(ch);
                }
            }
            result.append(slashes * 2u, L'\\');
            result.push_back(L'\"');
            return result;
        };
        auto sessionHex = [](const watchdog::SessionId& id) {
            constexpr wchar_t digits[] = L"0123456789abcdef";
            std::wstring result;
            result.reserve(id.size() * 2u);
            for (const auto byte : id) {
                result.push_back(digits[(byte >> 4u) & 0x0fu]);
                result.push_back(digits[byte & 0x0fu]);
            }
            return result;
        };
        auto handleNumber = [](HANDLE handle) {
            return std::to_wstring(static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(handle)));
        };
        std::wstring command = quote(executable_.wstring()) +
            L" --control-handle " + handleNumber(controlRead) +
            L" --status-handle " + handleNumber(statusWrite) +
            L" --host-pid " + std::to_wstring(hostIdentity.processId) +
            L" --host-created " + std::to_wstring(hostIdentity.creationTime100ns) +
            L" --session " + sessionHex(manifest.lease.sessionId) +
            L" --recovery-dir " + quote(recoveryDirectory.wstring());
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        SIZE_T attributeBytes = 0;
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        if (attributeBytes == 0) {
            CloseHandle(controlRead); CloseHandle(controlWrite);
            CloseHandle(statusRead); CloseHandle(statusWrite);
            error = "production watchdog handle allowlist sizing failed";
            return false;
        }
        std::vector<std::byte> storage(attributeBytes);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                              &attributeBytes) == FALSE) {
            CloseHandle(controlRead); CloseHandle(controlWrite);
            CloseHandle(statusRead); CloseHandle(statusWrite);
            error = "production watchdog handle allowlist initialization failed";
            return false;
        }
        const std::array<HANDLE, 2> inherited{controlRead, statusWrite};
        if (UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                      PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      const_cast<HANDLE*>(inherited.data()),
                                      inherited.size() * sizeof(HANDLE), nullptr, nullptr) == FALSE) {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            CloseHandle(controlRead); CloseHandle(controlWrite);
            CloseHandle(statusRead); CloseHandle(statusWrite);
            error = "production watchdog inherited-handle allowlist failed";
            return false;
        }
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            executable_.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            nullptr, nullptr, &startup.StartupInfo, &process);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(controlRead);
        CloseHandle(statusWrite);
        if (created == FALSE) {
            CloseHandle(controlWrite); CloseHandle(statusRead);
            error = "production hydra_watchdog launch failed";
            return false;
        }
        CloseHandle(process.hThread);
        process_ = process.hProcess;
        controlWrite_ = controlWrite;
        statusRead_ = statusRead;
        manifest_ = manifest;
        sequence_ = 1u;
        if (!sendFrame(watchdog::encodeRegisterPlan(sequence_, manifest_), error)) {
            abandon();
            return false;
        }
        watchdog::WatchdogStatus status;
        if (!waitStatus(status, handshakeTimeoutMilliseconds_, error) ||
            status.state != watchdog::WatchdogRunState::Armed ||
            status.sessionId != manifest_.lease.sessionId ||
            status.generation != manifest_.lease.generation) {
            if (error.empty()) error = "production watchdog did not confirm exact armed lease";
            abandon();
            return false;
        }
        return true;
    }

    bool renew(std::string& error) {
        if (!running()) {
            error = "production watchdog process is not running";
            return false;
        }
        return sendFrame(watchdog::encodeLeaseRenewal(++sequence_, manifest_.lease), error);
    }

    bool disarm(std::string& error) {
        if (!running()) {
            error = "production watchdog process is not running for exact disarm";
            return false;
        }
        if (!sendFrame(watchdog::encodeDisarm(++sequence_, manifest_.lease), error)) return false;
        watchdog::WatchdogStatus status;
        if (!waitStatus(status, handshakeTimeoutMilliseconds_, error) ||
            status.state != watchdog::WatchdogRunState::Disarmed ||
            status.reason != watchdog::WatchdogTriggerReason::CleanDisarm ||
            status.sessionId != manifest_.lease.sessionId ||
            status.generation != manifest_.lease.generation) {
            if (error.empty()) error = "production watchdog refused exact clean disarm";
            return false;
        }
        if (WaitForSingleObject(process_, handshakeTimeoutMilliseconds_) != WAIT_OBJECT_0) {
            error = "production watchdog did not exit after clean disarm";
            return false;
        }
        closeHandles();
        return true;
    }

    bool running() const noexcept {
        return process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }

    void abandon() noexcept {
        closePipeHandles();
        if (process_ != nullptr) {
            if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
                (void)TerminateProcess(process_, 0x50525744u);
                (void)WaitForSingleObject(process_, 2'000u);
            }
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

private:
    bool writeAll(const void* bytes, std::size_t size) {
        const auto* cursor = static_cast<const std::byte*>(bytes);
        std::size_t remaining = size;
        while (remaining != 0) {
            const DWORD chunk = static_cast<DWORD>((std::min)(
                remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (WriteFile(controlWrite_, cursor, chunk, &written, nullptr) == FALSE || written == 0)
                return false;
            cursor += written;
            remaining -= written;
        }
        return true;
    }

    bool sendFrame(const std::vector<std::byte>& frame, std::string& error) {
        if (controlWrite_ == nullptr || frame.empty() ||
            frame.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            error = "production watchdog frame is invalid";
            return false;
        }
        const auto count = static_cast<std::uint32_t>(frame.size());
        const std::array<std::byte, 4> prefix{
            static_cast<std::byte>(count & 0xffu),
            static_cast<std::byte>((count >> 8u) & 0xffu),
            static_cast<std::byte>((count >> 16u) & 0xffu),
            static_cast<std::byte>((count >> 24u) & 0xffu)};
        if (!writeAll(prefix.data(), prefix.size()) || !writeAll(frame.data(), frame.size())) {
            error = "production watchdog frame write failed";
            return false;
        }
        return true;
    }

    bool waitStatus(watchdog::WatchdogStatus& status,
                    std::uint32_t timeoutMilliseconds,
                    std::string& error) {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeoutMilliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (statusRead_ == nullptr) break;
            DWORD available = 0;
            if (PeekNamedPipe(statusRead_, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
                error = "production watchdog status pipe failed";
                return false;
            }
            if (available < 4u) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            std::array<std::byte, 4> prefix{};
            DWORD read = 0;
            if (ReadFile(statusRead_, prefix.data(), 4u, &read, nullptr) == FALSE || read != 4u) {
                error = "production watchdog status prefix read failed";
                return false;
            }
            const auto frameBytes = static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(prefix[0])) |
                (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[1])) << 8u) |
                (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[2])) << 16u) |
                (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[3])) << 24u);
            if (frameBytes < watchdog::kWatchdogFrameHeaderBytes ||
                frameBytes > watchdog::kWatchdogMaxFrameBytes) {
                error = "production watchdog status frame length is invalid";
                return false;
            }
            std::vector<std::byte> frame(frameBytes);
            if (ReadFile(statusRead_, frame.data(), frameBytes, &read, nullptr) == FALSE ||
                read != frameBytes) {
                error = "production watchdog status frame read failed";
                return false;
            }
            const auto decoded = watchdog::decodeWatchdogFrame(frame);
            std::string decodeError;
            if (!decoded || !decoded.frame ||
                !watchdog::decodeWatchdogStatus(*decoded.frame, status, &decodeError)) {
                error = decodeError.empty() ? "production watchdog status decode failed"
                                            : decodeError;
                return false;
            }
            return true;
        }
        error = "production watchdog status timed out";
        return false;
    }

    void closePipeHandles() noexcept {
        if (controlWrite_ != nullptr) { CloseHandle(controlWrite_); controlWrite_ = nullptr; }
        if (statusRead_ != nullptr) { CloseHandle(statusRead_); statusRead_ = nullptr; }
    }
    void closeHandles() noexcept {
        closePipeHandles();
        if (process_ != nullptr) { CloseHandle(process_); process_ = nullptr; }
    }

    std::filesystem::path executable_;
    std::uint32_t handshakeTimeoutMilliseconds_{0};
    HANDLE process_{nullptr};
    HANDLE controlWrite_{nullptr};
    HANDLE statusRead_{nullptr};
    watchdog::RollbackPlanManifest manifest_;
    std::uint64_t sequence_{0};
};

#endif

class NativeProductionRecoveryLeaseRuntime final
    : public IProductionRecoveryLeaseRuntime {
public:
    NativeProductionRecoveryLeaseRuntime(
        std::filesystem::path recoveryRoot,
        std::filesystem::path watchdogExecutable,
        std::uint32_t watchdogHandshakeTimeoutMilliseconds)
        : recoveryRoot_(std::move(recoveryRoot)),
          watchdogExecutable_(std::move(watchdogExecutable)),
          watchdogHandshakeTimeoutMilliseconds_(watchdogHandshakeTimeoutMilliseconds) {}

    bool arm(const recovery::RecoveryProcessAttachmentRegistration& registration,
             std::string& error) override {
        std::lock_guard lock(mutex_);
        if (!recovery::validateRecoveryProcessAttachmentRegistration(registration, &error)) {
            return false;
        }
        const auto seatId = registration.identity.seatId;
        if (seatId == 0 || seatId > kV1SeatLimit) {
            error = "production recovery runtime rejects non-v1 Seat identity";
            return false;
        }
        if (const auto found = active_.find(seatId); found != active_.end()) {
            if (found->second->registration == registration && found->second->armed) {
                return verifyArmedLocked(*found->second, error);
            }
            error = "production recovery runtime already owns a different exact Seat attachment";
            return false;
        }
        if (active_.size() >= kV1SeatLimit) {
            error = "production recovery runtime exceeded the v1 two-Seat bound";
            return false;
        }
        if (recoveryRoot_.empty()) {
            error = "production recovery root is unavailable";
            return false;
        }

        auto record = std::make_unique<ActiveLease>(
            seatRecoveryRoot(recoveryRoot_, seatId), watchdogExecutable_,
            watchdogHandshakeTimeoutMilliseconds_, registration);
        const auto startup = record->journalStore.assessStartupAndEnterSafeMode();
        if (startup.state != recovery::StartupRecoveryState::Clean) {
            error = "Seat recovery root is not clean before production attachment: " +
                    startup.diagnostic;
            return false;
        }
        const auto existingRegistration = record->resetStore.load();
        if (existingRegistration.status != reset::RuntimeRegistrationReadStatus::Missing) {
            error = "Seat recovery root contains an existing runtime reset registration";
            return false;
        }

#ifdef _WIN32
        std::uint32_t ownerError = 0;
        if (!watchdog::queryProcessIdentity(GetCurrentProcessId(),
                                            record->resetRegistration.ownerProcess,
                                            &ownerError)) {
            error = "production recovery runtime could not identify the exact Host owner";
            return false;
        }
#else
        error = "production recovery runtime is Windows-only";
        return false;
#endif
        record->resetRegistration.manifest = registration.manifest;
        record->resetRegistration.attachment = registration.identity;
        if (!record->resetStore.write(record->resetRegistration, &error)) return false;
        record->resetWritten = true;

#ifdef _WIN32
        if (!record->watchdog.start(registration.manifest, record->root, error)) {
            std::string removeError;
            if (record->resetStore.remove(&removeError)) record->resetWritten = false;
            return false;
        }
        record->watchdogStarted = true;
#else
        return false;
#endif

        const auto attachmentSnapshot = recovery::makeRecoveryProcessAttachmentSnapshot(
            registration.identity, &error);
        if (!attachmentSnapshot) {
            return failArmLocked(std::move(record), error);
        }
        const std::array snapshots{*attachmentSnapshot};
        record->journal.emplace(record->journalStore, registration.manifest,
                                registration.identity.recoveryEpoch);
        if (!record->journal->begin(snapshots, &error)) {
            return failArmLocked(std::move(record), error);
        }
        for (const auto& action : registration.manifest.actions) {
            if (!record->journal->prepareAction(action.actionId, &error)) {
                return failArmLocked(std::move(record), error);
            }
        }
        const auto processId = processActionId(seatId);
        if (!record->journal->markActionApplied(processId, &error) ||
            !record->journal->markActionVerified(processId, &error)) {
            return failArmLocked(std::move(record), error);
        }
        record->activeActionIds.insert(processId);
        if (registration.manifest.actions.size() == 1u) {
            if (!record->journal->commitActivation(&error)) {
                return failArmLocked(std::move(record), error);
            }
            record->committed = true;
        }
        record->armed = true;
        active_.emplace(seatId, std::move(record));
        error.clear();
        return true;
    }

    bool verifyArmed(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::string& error) override {
        std::lock_guard lock(mutex_);
        const auto found = active_.find(registration.identity.seatId);
        if (found == active_.end() || found->second->registration != registration) {
            error = "production recovery runtime does not own the exact requested attachment";
            return false;
        }
        return verifyArmedLocked(*found->second, error);
    }

    bool markActionActive(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::uint32_t actionId,
        std::string& error) override {
        std::lock_guard lock(mutex_);
        auto* record = exactRecordLocked(registration, error);
        if (record == nullptr || !record->journal || !record->armed) return false;
        if (record->activeActionIds.contains(actionId)) {
            error.clear();
            return true;
        }
        const auto action = std::find_if(
            registration.manifest.actions.begin(), registration.manifest.actions.end(),
            [&](const auto& value) { return value.actionId == actionId; });
        if (action == registration.manifest.actions.end() ||
            action->kind == watchdog::RollbackActionKind::TerminateOwnedProcess) {
            error = "production recovery runtime cannot activate an unknown/process action through the external-action path";
            return false;
        }
        if (!record->journal->markActionApplied(actionId, &error) ||
            !record->journal->markActionVerified(actionId, &error)) {
            return false;
        }
        record->activeActionIds.insert(actionId);
        return true;
    }

    bool commit(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::string& error) override {
        std::lock_guard lock(mutex_);
        auto* record = exactRecordLocked(registration, error);
        if (record == nullptr || !record->journal || !record->armed) return false;
        if (record->committed) {
            error.clear();
            return true;
        }
        if (record->activeActionIds.size() != registration.manifest.actions.size()) {
            error = "production recovery activation cannot commit before every prepared recovery action is active";
            return false;
        }
        if (!record->journal->commitActivation(&error)) return false;
        record->committed = true;
        return true;
    }

    bool disarm(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::span<const std::uint32_t> verifiedRolledBackActionIds,
        std::string& error) override {
        std::lock_guard lock(mutex_);
        const auto found = active_.find(registration.identity.seatId);
        if (found == active_.end()) {
            error.clear();
            return true;
        }
        auto& record = *found->second;
        if (record.registration != registration) {
            error = "stale/wrong recovery attachment cannot disarm another exact Seat lease";
            return false;
        }
        if (!record.journal || !record.armed) {
            error = "production recovery attachment is not in a verifiably armed state";
            return false;
        }
        if (processExactStillRunning(registration.identity.process)) {
            error = "exact owned process is still live; recovery attachment cannot be cleanly disarmed";
            return false;
        }
        std::set<std::uint32_t> externallySafe(verifiedRolledBackActionIds.begin(),
                                                verifiedRolledBackActionIds.end());
        if (!record.rollbackStarted) {
            if (!record.journal->beginRollback(&error)) return false;
            record.rollbackStarted = true;
        }
        for (const auto& action : registration.manifest.actions) {
            if (record.rolledBackActionIds.contains(action.actionId)) continue;
            if (action.kind != watchdog::RollbackActionKind::TerminateOwnedProcess &&
                !externallySafe.contains(action.actionId)) {
                error = "non-process recovery action lacks verified rollback evidence";
                return false;
            }
            if (!record.journal->markActionRolledBack(action.actionId, &error)) return false;
            record.rolledBackActionIds.insert(action.actionId);
        }
        if (!record.journal->verifyRollback(&error)) return false;
#ifdef _WIN32
        if (!record.watchdog.disarm(error)) return false;
        record.watchdogStarted = false;
#endif
        if (!record.journal->markCleanStop(&error)) return false;
        if (!record.resetStore.remove(&error)) return false;
        record.resetWritten = false;
        record.armed = false;
        active_.erase(found);
        error.clear();
        return true;
    }

    bool verifyDisarmed(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::string& error) override {
        std::lock_guard lock(mutex_);
        const auto found = active_.find(registration.identity.seatId);
        if (found == active_.end()) {
            error.clear();
            return true;
        }
        if (found->second->registration != registration) {
            error = "another exact Seat recovery attachment is still active";
            return false;
        }
        error = "requested production recovery attachment remains armed";
        return false;
    }

private:
    struct ActiveLease {
        ActiveLease(std::filesystem::path rootValue,
                    const std::filesystem::path& watchdogExecutable,
                    std::uint32_t watchdogHandshakeTimeoutMilliseconds,
                    recovery::RecoveryProcessAttachmentRegistration registrationValue)
            : root(std::move(rootValue)),
              storage(root),
              journalStore(storage),
              resetStore(root),
#ifdef _WIN32
              watchdog(watchdogExecutable, watchdogHandshakeTimeoutMilliseconds),
#endif
              registration(std::move(registrationValue)) {}

        std::filesystem::path root;
        recovery::NativeCrashJournalStorage storage;
        recovery::CrashJournalStore journalStore;
        reset::RuntimeResetRegistrationStore resetStore;
#ifdef _WIN32
        WatchdogProcessClient watchdog;
#endif
        recovery::RecoveryProcessAttachmentRegistration registration;
        reset::RuntimeResetRegistration resetRegistration;
        std::optional<gatec::GateCRecoveryJournal> journal;
        std::set<std::uint32_t> activeActionIds;
        std::set<std::uint32_t> rolledBackActionIds;
        bool resetWritten{false};
        bool watchdogStarted{false};
        bool armed{false};
        bool committed{false};
        bool rollbackStarted{false};
    };

    bool verifyArmedLocked(ActiveLease& record, std::string& error) {
        if (!record.armed || !record.journal) {
            error = "production recovery attachment is not armed";
            return false;
        }
#ifdef _WIN32
        if (!record.watchdog.running() || !record.watchdog.renew(error)) {
            if (error.empty()) error = "production watchdog lease is not verifiably live";
            return false;
        }
#else
        error = "production recovery runtime is Windows-only";
        return false;
#endif
        const auto loaded = record.resetStore.load();
        if (loaded.status != reset::RuntimeRegistrationReadStatus::Success ||
            !loaded.registration || *loaded.registration != record.resetRegistration) {
            error = "runtime reset registration no longer matches the exact recovery attachment";
            return false;
        }
        const auto current = record.journalStore.loadCurrent(&error);
        if (!current || !recovery::validateRecoveryProcessAttachmentJournalBinding(
                            *current, record.registration.identity, &error)) {
            if (error.empty()) error = "crash journal no longer matches exact recovery attachment";
            return false;
        }
        error.clear();
        return true;
    }

    ActiveLease* exactRecordLocked(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::string& error) {
        const auto found = active_.find(registration.identity.seatId);
        if (found == active_.end() || found->second->registration != registration) {
            error = "production recovery runtime exact attachment lookup failed";
            return nullptr;
        }
        return found->second.get();
    }

    bool failArmLocked(std::unique_ptr<ActiveLease> record, std::string& error) {
        const auto primaryError = error;
        bool watchdogClean = true;
#ifdef _WIN32
        if (record->watchdogStarted) {
            std::string disarmError;
            watchdogClean = record->watchdog.disarm(disarmError);
            if (!watchdogClean && error.empty()) error = disarmError;
        }
#endif
        bool registrationClean = true;
        if (record->resetWritten && watchdogClean) {
            std::string removeError;
            registrationClean = record->resetStore.remove(&removeError);
            if (!registrationClean && error.empty()) error = removeError;
        }
        if (!watchdogClean || !registrationClean) {
            record->armed = true;
            active_.emplace(record->registration.identity.seatId, std::move(record));
            if (error.empty()) error = "recovery arm failed and cleanup could not be verified";
        } else {
            error = primaryError;
        }
        return false;
    }

    std::filesystem::path recoveryRoot_;
    std::filesystem::path watchdogExecutable_;
    std::uint32_t watchdogHandshakeTimeoutMilliseconds_{0};
    std::mutex mutex_;
    std::map<SeatId, std::unique_ptr<ActiveLease>> active_;
};

class NativeProductionGateCSessionRuntime final
    : public IProductionGateCSessionRuntime {
public:
    NativeProductionGateCSessionRuntime(std::uint32_t handshakeTimeoutMilliseconds,
                                        std::uint32_t ioTimeoutMilliseconds)
        : handshakeTimeoutMilliseconds_(handshakeTimeoutMilliseconds),
          ioTimeoutMilliseconds_(ioTimeoutMilliseconds) {}
    ~NativeProductionGateCSessionRuntime() override {
        std::vector<ProductionGateCSessionRequest> requests;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [seatId, session] : sessions_) {
                (void)seatId;
                requests.push_back(session->request);
            }
        }
        for (const auto& request : requests) {
            std::string ignored;
            (void)stop(request, ignored);
        }
    }

    bool start(const ProductionGateCSessionRequest& request,
               std::string& error) override {
        if (!request.valid() || request.epoch.seatId > kV1SeatLimit ||
            !validateGateCProfile(request.profile, error) ||
            !exactPathAllowed(request.process.authoritativeProcess, request.profile)) {
            if (error.empty()) error = "production Gate-C exact process/profile validation failed";
            return false;
        }
#ifdef _WIN32
        std::lock_guard lock(mutex_);
        if (const auto found = sessions_.find(request.epoch.seatId);
            found != sessions_.end()) {
            if (sameRequest(found->second->request, request)) {
                ProductionGateCSessionStatus status;
                return verifyLocked(*found->second, request, status, error);
            }
            error = "production Gate-C Seat already owns another exact session";
            return false;
        }
        if (sessions_.size() >= kV1SeatLimit) {
            error = "production Gate-C runtime exceeded the v1 two-Seat bound";
            return false;
        }
        auto session = std::make_unique<Session>();
        session->request = request;
        if (!openExactProcess(*session, error) ||
            !prepareTransport(*session, error) ||
            !injectBridge(*session, error) ||
            !acceptReceiver(*session, error) ||
            !startInputRouter(*session, error)) {
            cleanupFailedStart(*session);
            return false;
        }
        ProductionGateCSessionStatus status;
        if (!verifyLocked(*session, request, status, error)) {
            cleanupFailedStart(*session);
            return false;
        }
        sessions_.emplace(request.epoch.seatId, std::move(session));
        error.clear();
        return true;
#else
        error = "production Gate-C runtime is Windows-only";
        return false;
#endif
    }

    bool verify(const ProductionGateCSessionRequest& request,
                ProductionGateCSessionStatus& status,
                std::string& error) override {
#ifdef _WIN32
        std::lock_guard lock(mutex_);
        const auto found = sessions_.find(request.epoch.seatId);
        if (found == sessions_.end()) {
            error = "production Gate-C exact Seat session is absent";
            return false;
        }
        return verifyLocked(*found->second, request, status, error);
#else
        (void)request; (void)status;
        error = "production Gate-C runtime is Windows-only";
        return false;
#endif
    }

    bool inputMetricsSnapshot(
        const ProductionGateCSessionRequest& request,
        InputMetricsSnapshot& snapshot,
        std::string& error) override {
#ifdef _WIN32
        std::lock_guard lock(mutex_);
        const auto found = sessions_.find(request.epoch.seatId);
        if (found == sessions_.end() || !sameRequest(found->second->request, request)) {
            error = "production Gate-C exact Seat session is absent or stale for metrics snapshot";
            return false;
        }
        ProductionGateCSessionStatus status;
        if (!verifyLocked(*found->second, request, status, error)) return false;
        snapshot = found->second->metrics.snapshot();
        error.clear();
        return true;
#else
        (void)request; (void)snapshot;
        error = "production Gate-C runtime is Windows-only";
        return false;
#endif
    }

    bool stop(const ProductionGateCSessionRequest& request,
              std::string& error) noexcept override {
#ifdef _WIN32
        try {
            std::unique_ptr<Session> session;
            {
                std::lock_guard lock(mutex_);
                const auto found = sessions_.find(request.epoch.seatId);
                if (found == sessions_.end()) {
                    error.clear();
                    return true;
                }
                if (!sameRequest(found->second->request, request)) {
                    error = "stale production Gate-C identity cannot stop another Seat process session";
                    return false;
                }
                session = std::move(found->second);
                sessions_.erase(found);
            }
            stopRouter(*session);
            bool receiverSafe = true;
            const auto exact = watchdogIdentity(request.process.authoritativeProcess);
            if (processExactStillRunning(exact)) {
                std::lock_guard channelLock(session->channelMutex);
                if (!session->channel.valid()) {
                    receiverSafe = false;
                    error = "production Gate-C transport disappeared before receiver rollback";
                } else {
                    std::string writeError;
                    if (!session->channel.writeFrame(
                            gatec::encodeShutdown(++session->sequence),
                            ioTimeoutMilliseconds_, &writeError)) {
                        receiverSafe = false;
                        error = "production Gate-C receiver shutdown failed: " + writeError;
                    }
                }
                if (receiverSafe) {
                    receiverSafe = waitBridgeStopped(*session, error);
                }
            }
            closeSessionHandles(*session);
            return receiverSafe;
        } catch (...) {
            error = "production Gate-C rollback threw unexpectedly";
            return false;
        }
#else
        (void)request;
        error = "production Gate-C runtime is Windows-only";
        return false;
#endif
    }

private:
#ifdef _WIN32
    struct PendingInputMetric {
        std::uint64_t correlationId{0u};
        InputMetricEventClass eventClass{InputMetricEventClass::None};
    };

    struct Session {
        ProductionGateCSessionRequest request;
        HANDLE process{nullptr};
        HANDLE mapping{nullptr};
        detail::ProductionBridgeMappingV1* mappingView{nullptr};
        gatec::PipeChannel channel;
        std::mutex channelMutex;
        std::uint64_t sequence{1u};
        InputMetricsRecorder metrics{kDefaultInputMetricsCapacity,
                                     InputMetricsPrivacyMode::Redacted};
        std::map<std::uint64_t, PendingInputMetric> pendingInputMetrics;
        std::atomic<bool> transportHealthy{true};
        std::atomic<bool> devicesPresent{false};
        std::atomic<bool> stopRouterRequested{false};
        std::thread routerThread;
        std::mutex routerReadyMutex;
        std::condition_variable routerReadyCv;
        bool routerReady{false};
        bool routerStartSucceeded{false};
        std::string routerStartError;
    };

    static bool sameRequest(const ProductionGateCSessionRequest& left,
                            const ProductionGateCSessionRequest& right) {
        return left.epoch == right.epoch &&
               left.process.authoritativeProcess.sameInstance(
                   right.process.authoritativeProcess) &&
               left.process.handoffGeneration == right.process.handoffGeneration &&
               left.profile == right.profile &&
               left.assignedStableDeviceIds == right.assignedStableDeviceIds;
    }

    bool openExactProcess(Session& session, std::string& error) {
        const auto& identity = session.request.process.authoritativeProcess;
        session.process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_CREATE_THREAD |
                PROCESS_VM_OPERATION | PROCESS_VM_WRITE | SYNCHRONIZE,
            FALSE, identity.processId);
        if (session.process == nullptr) {
            error = "exact production Gate-C target process cannot be opened";
            return false;
        }
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (GetProcessTimes(session.process, &creation, &exit, &kernel, &user) == FALSE) {
            error = "exact production Gate-C target creation time cannot be verified";
            return false;
        }
        const std::uint64_t creationTime =
            (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32u) |
            creation.dwLowDateTime;
        if (creationTime != identity.creationTime100ns ||
            WaitForSingleObject(session.process, 0) != WAIT_TIMEOUT) {
            error = "production Gate-C target is stale/exited or PID-reused";
            return false;
        }
        std::wstring image(32768u, L'\0');
        DWORD imageLength = static_cast<DWORD>(image.size());
        if (QueryFullProcessImageNameW(session.process, 0, image.data(), &imageLength) == FALSE) {
            error = "production Gate-C target executable identity cannot be verified";
            return false;
        }
        image.resize(imageLength);
        if (foldedPath(image) != foldedPath(identity.executablePath) ||
            !exactPathAllowed(identity, session.request.profile)) {
            error = "production Gate-C target executable differs from exact trusted process identity";
            return false;
        }
        const auto processArchitecture = gatec::detectProcessArchitecture(session.process);
        const auto bridgeArchitecture = gatec::detectPortableExecutableArchitecture(
            session.request.profile.bridgeLibraryPath);
        const auto hostArchitecture = sizeof(void*) == 8u
            ? gatec::ProcessArchitecture::X64 : gatec::ProcessArchitecture::X86;
        if (!processArchitecture || !bridgeArchitecture ||
            processArchitecture.architecture != bridgeArchitecture.architecture ||
            processArchitecture.architecture != hostArchitecture) {
            error = "production Gate-C runtime attach requires exact same-architecture target/bridge/Host";
            return false;
        }
        return true;
    }

    bool prepareTransport(Session& session, std::string& error) {
        const auto token = gatec::generateSessionToken();
        if (!token) {
            error = "production Gate-C cryptographic session token generation failed";
            return false;
        }
        const auto pipeName = gatec::makeGateCPipeName(
            GetCurrentProcessId(), session.request.epoch.seatId, *token);
        auto server = gatec::createGateCServerPipe(pipeName, &error);
        if (!server.valid()) return false;

        const auto mappingName = detail::productionBridgeMappingName(
            session.request.process.authoritativeProcess.processId);
        session.mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(detail::ProductionBridgeMappingV1)),
            mappingName.c_str());
        if (session.mapping == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
            error = "production Gate-C exact PID mapping already exists or could not be created";
            return false;
        }
        session.mappingView = static_cast<detail::ProductionBridgeMappingV1*>(
            MapViewOfFile(session.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                          sizeof(detail::ProductionBridgeMappingV1)));
        if (session.mappingView == nullptr ||
            pipeName.size() >= detail::kProductionBridgePipeNameChars) {
            error = "production Gate-C mapping initialization failed";
            return false;
        }
        *session.mappingView = {};
        session.mappingView->seatId = session.request.epoch.seatId;
        session.mappingView->requiredApiMask = session.request.profile.requiredApiMask;
        session.mappingView->token = *token;
        std::copy(pipeName.begin(), pipeName.end(), session.mappingView->pipeName);
        session.mappingView->pipeName[pipeName.size()] = L'\0';
        if (FlushViewOfFile(session.mappingView, sizeof(*session.mappingView)) == FALSE) {
            error = "production Gate-C mapping could not be flushed";
            return false;
        }
        session.channel = std::move(server);
        return true;
    }

    bool injectBridge(Session& session, std::string& error) {
        const std::wstring library = session.request.profile.bridgeLibraryPath.wstring();
        if (library.empty() || !std::filesystem::is_regular_file(library)) {
            error = "production Gate-C bridge DLL is unavailable";
            return false;
        }
        const SIZE_T bytes = (library.size() + 1u) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(session.process, nullptr, bytes,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (remote == nullptr) {
            error = "production Gate-C bridge path allocation failed";
            return false;
        }
        SIZE_T written = 0;
        const bool wrote = WriteProcessMemory(session.process, remote, library.c_str(),
                                              bytes, &written) != FALSE && written == bytes;
        if (!wrote) {
            VirtualFreeEx(session.process, remote, 0, MEM_RELEASE);
            error = "production Gate-C bridge path write failed";
            return false;
        }
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            kernel32 != nullptr ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr);
        if (loadLibrary == nullptr) {
            VirtualFreeEx(session.process, remote, 0, MEM_RELEASE);
            error = "production Gate-C LoadLibraryW resolution failed";
            return false;
        }
        HANDLE thread = CreateRemoteThread(session.process, nullptr, 0,
                                           loadLibrary, remote, 0, nullptr);
        if (thread == nullptr) {
            VirtualFreeEx(session.process, remote, 0, MEM_RELEASE);
            error = "production Gate-C runtime bridge attach failed";
            return false;
        }
        const DWORD wait = WaitForSingleObject(thread, handshakeTimeoutMilliseconds_);
        DWORD result = 0;
        const bool loaded = wait == WAIT_OBJECT_0 &&
                            GetExitCodeThread(thread, &result) != FALSE && result != 0;
        CloseHandle(thread);
        VirtualFreeEx(session.process, remote, 0, MEM_RELEASE);
        if (!loaded) {
            error = "production Gate-C bridge DLL did not load into the exact owned process";
            return false;
        }
        return true;
    }

    bool acceptReceiver(Session& session, std::string& error) {
        if (!gatec::waitForGateCClient(session.channel, handshakeTimeoutMilliseconds_, &error)) {
            return false;
        }
        const auto frame = session.channel.readFrame(handshakeTimeoutMilliseconds_);
        gatec::HelloMessage hello{};
        if (!frame || !frame.frame || frame.frame->type != gatec::MessageType::Hello ||
            frame.frame->sequence != 1u ||
            !gatec::decodeHello(*frame.frame, hello, &error) ||
            hello.token != session.mappingView->token ||
            hello.seatId != session.request.epoch.seatId ||
            hello.processId != session.request.process.authoritativeProcess.processId ||
            hello.architectureBits != sizeof(void*) * 8u || hello.targetWindow == 0) {
            error = "production Gate-C receiver Hello identity validation failed";
            return false;
        }
        DWORD windowOwner = 0;
        if (GetWindowThreadProcessId(
                reinterpret_cast<HWND>(static_cast<std::uintptr_t>(hello.targetWindow)),
                &windowOwner) == 0 || windowOwner != hello.processId) {
            error = "production Gate-C receiver bootstrap HWND is not owned by the exact process";
            return false;
        }
        gatec::TestCapability capabilities = gatec::kControlledTargetCapabilities;
        if ((session.request.profile.requiredApiMask & HYDRA_GATE_C_SHIM_POLLING_API_MASK) != 0)
            capabilities = capabilities | gatec::TestCapability::PollingApiShim;
        if ((session.request.profile.requiredApiMask & HYDRA_GATE_C_SHIM_CURSOR_FOCUS_API_MASK) != 0)
            capabilities = capabilities | gatec::TestCapability::CursorFocusApiShim;
        if ((session.request.profile.requiredApiMask & HYDRA_GATE_C_SHIM_RAW_INPUT_API_MASK) != 0)
            capabilities = capabilities | gatec::TestCapability::RawInputApiShim;
        gatec::HelloAckMessage ack{};
        ack.accepted = true;
        ack.serverProcessId = GetCurrentProcessId();
        ack.grantedCapabilities = gatec::testCapabilityBits(capabilities);
        if (!session.channel.writeFrame(gatec::encodeHelloAck(1u, ack),
                                        ioTimeoutMilliseconds_, &error)) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(handshakeTimeoutMilliseconds_);
        while (std::chrono::steady_clock::now() < deadline) {
            if (InterlockedCompareExchange(&session.mappingView->lifecycle, 0, 0) ==
                static_cast<LONG>(detail::ProductionBridgeDllState::Active)) {
                return true;
            }
            if (InterlockedCompareExchange(&session.mappingView->lifecycle, 0, 0) ==
                static_cast<LONG>(detail::ProductionBridgeDllState::Failed)) {
                error = "production Gate-C receiver failed during process-local startup";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        error = "production Gate-C receiver did not reach active process-local state";
        return false;
    }

    bool startInputRouter(Session& session, std::string& error) {
        session.routerThread = std::thread([this, &session] {
            InputRouter router;
            auto markReady = [&](bool succeeded, std::string diagnostic) {
                {
                    std::lock_guard lock(session.routerReadyMutex);
                    session.routerStartSucceeded = succeeded;
                    session.routerStartError = std::move(diagnostic);
                    session.routerReady = true;
                }
                session.routerReadyCv.notify_all();
            };
            if (!router.initialize(0) || !router.registerRawInputDevices(true) ||
                !router.refreshConnectedDevices(false)) {
                markReady(false, "production Raw Input router initialization failed");
                return;
            }
            const auto isAssigned = [&](std::wstring_view stable) {
                const auto folded = foldedIdentity(std::wstring(stable));
                return std::any_of(session.request.assignedStableDeviceIds.begin(),
                                   session.request.assignedStableDeviceIds.end(),
                                   [&](const auto& candidate) {
                                       return foldedIdentity(candidate) == folded;
                                   });
            };
            const auto refreshPresence = [&] {
                std::set<std::wstring> online;
                for (const auto& device : router.connectedDevices()) {
                    if (device.online && isAssigned(device.deviceId))
                        online.insert(foldedIdentity(device.deviceId));
                }
                std::set<std::wstring> expected;
                for (const auto& id : session.request.assignedStableDeviceIds)
                    expected.insert(foldedIdentity(id));
                session.devicesPresent.store(online == expected, std::memory_order_release);
            };
            router.setGlobalCallback([&, this](const RawInputEvent& event) {
                if (!isAssigned(event.deviceId)) return;
                gatec::InputEventMessage input{};
                if (event.rawDevType == RIM_TYPEKEYBOARD) {
                    input.kind = gatec::InputKind::Keyboard;
                    input.vkey = static_cast<std::uint16_t>(event.vkey);
                    input.scanCode = event.scanCode;
                    input.keyboardFlags = event.keyboardFlags;
                    input.keyTransition = event.keyTransition == RawKeyTransition::Down
                        ? gatec::KeyTransition::Down
                        : event.keyTransition == RawKeyTransition::Up
                            ? gatec::KeyTransition::Up : gatec::KeyTransition::None;
                } else if (event.rawDevType == RIM_TYPEMOUSE) {
                    input.kind = gatec::InputKind::Mouse;
                    input.deltaX = event.deltaX;
                    input.deltaY = event.deltaY;
                    input.mouseButtonFlags = event.mouseButtonFlags;
                    input.wheelDelta = event.wheelDelta;
                    input.isTouchpad = event.isTouchpad;
                } else {
                    return;
                }
                input.timestampMicros = event.monotonicTimestampMicros;
                std::lock_guard channelLock(session.channelMutex);
                const auto sequence = ++session.sequence;
                const auto eventClass = classifyInputMetricEvent(event);
                const auto correlationId =
                    (static_cast<std::uint64_t>(session.request.epoch.seatId) << 56u) ^
                    sequence;
                const auto targetProcessId =
                    session.request.process.authoritativeProcess.processId;
                const auto recordStage = [&](InputMetricStage stage,
                                             std::uint64_t timestamp) {
                    if (eventClass == InputMetricEventClass::None) return;
                    InputMetricSample sample;
                    sample.correlationId = correlationId;
                    sample.timestampMicros = timestamp == 0u
                        ? monotonicInputMetricTimestampMicros() : timestamp;
                    sample.stage = stage;
                    sample.eventClass = eventClass;
                    sample.expectedSeatId = session.request.epoch.seatId;
                    sample.targetProcessId = targetProcessId;
                    (void)session.metrics.tryRecord(sample);
                };
                const auto observedAt = input.timestampMicros == 0u
                    ? monotonicInputMetricTimestampMicros() : input.timestampMicros;
                recordStage(InputMetricStage::PhysicalObserved, observedAt);
                const auto enqueuedAt = monotonicInputMetricTimestampMicros();
                recordStage(InputMetricStage::RouteEnqueued, enqueuedAt);
                recordStage(InputMetricStage::RouteDequeued, enqueuedAt);

                std::string writeError;
                if (!session.channel.valid() ||
                    !session.channel.writeFrame(
                        gatec::encodeInputEvent(sequence, input),
                        ioTimeoutMilliseconds_, &writeError)) {
                    if (eventClass != InputMetricEventClass::None) {
                        InputMetricSample dropped;
                        dropped.correlationId = correlationId;
                        dropped.timestampMicros = monotonicInputMetricTimestampMicros();
                        dropped.stage = InputMetricStage::RouteDropped;
                        dropped.eventClass = eventClass;
                        dropped.expectedSeatId = session.request.epoch.seatId;
                        dropped.targetProcessId = targetProcessId;
                        dropped.queueDroppedCount = 1u;
                        (void)session.metrics.tryRecord(dropped);
                    }
                    session.transportHealthy.store(false, std::memory_order_release);
                    return;
                }
                recordStage(InputMetricStage::RouteWritten,
                            monotonicInputMetricTimestampMicros());
                if (eventClass != InputMetricEventClass::None) {
                    session.pendingInputMetrics.emplace(
                        sequence, PendingInputMetric{correlationId, eventClass});
                }
            });
            router.setDeviceChangeCallback([&](const RawInputDeviceChange&) {
                refreshPresence();
            });
            refreshPresence();
            if (!session.devicesPresent.load(std::memory_order_acquire)) {
                router.stop();
                markReady(false, "one or more exact assigned physical input devices are absent");
                return;
            }
            markReady(true, {});
            while (!session.stopRouterRequested.load(std::memory_order_acquire)) {
                router.processMessages();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            router.stop();
        });
        std::unique_lock readyLock(session.routerReadyMutex);
        const bool ready = session.routerReadyCv.wait_for(
            readyLock, std::chrono::milliseconds(handshakeTimeoutMilliseconds_),
            [&] { return session.routerReady; });
        if (!ready || !session.routerStartSucceeded) {
            error = ready ? session.routerStartError
                          : "production Raw Input router startup timed out";
            readyLock.unlock();
            stopRouter(session);
            return false;
        }
        return true;
    }

    bool verifyLocked(Session& session,
                      const ProductionGateCSessionRequest& request,
                      ProductionGateCSessionStatus& status,
                      std::string& error) {
        status = {};
        if (!sameRequest(session.request, request) ||
            !processExactStillRunning(watchdogIdentity(request.process.authoritativeProcess)) ||
            !session.transportHealthy.load(std::memory_order_acquire) ||
            !session.devicesPresent.load(std::memory_order_acquire) ||
            session.mappingView == nullptr ||
            InterlockedCompareExchange(&session.mappingView->lifecycle, 0, 0) !=
                static_cast<LONG>(detail::ProductionBridgeDllState::Active)) {
            error = "production Gate-C receiver/process/device state is no longer authoritative";
            return false;
        }
        std::lock_guard channelLock(session.channelMutex);
        gatec::QuerySnapshotMessage query{};
        query.probeVkey = static_cast<std::uint16_t>('A');
        const auto sequence = ++session.sequence;
        if (!session.channel.writeFrame(gatec::encodeQuerySnapshot(sequence, query),
                                        ioTimeoutMilliseconds_, &error)) {
            session.transportHealthy.store(false, std::memory_order_release);
            return false;
        }
        const auto reply = session.channel.readFrame(ioTimeoutMilliseconds_);
        gatec::StateSnapshotMessage receiver{};
        if (!reply || !reply.frame || reply.frame->sequence != sequence ||
            !gatec::decodeStateSnapshot(*reply.frame, receiver, &error)) {
            session.transportHealthy.store(false, std::memory_order_release);
            if (error.empty()) error = "production Gate-C receiver snapshot verification failed";
            return false;
        }
        status.active = true;
        status.receiverVerified = true;
        status.assignedDevicesPresent = true;
        status.process = request.process.authoritativeProcess;
        status.handoffGeneration = request.process.handoffGeneration;
        status.receiverSequence = sequence;
        error.clear();
        return true;
    }

    bool waitBridgeStopped(Session& session, std::string& error) {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(handshakeTimeoutMilliseconds_);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto lifecycle = InterlockedCompareExchange(
                &session.mappingView->lifecycle, 0, 0);
            if (lifecycle == static_cast<LONG>(detail::ProductionBridgeDllState::Stopped) &&
                InterlockedCompareExchange(&session.mappingView->rollbackComplete, 0, 0) == 1) {
                error.clear();
                return true;
            }
            if (lifecycle == static_cast<LONG>(detail::ProductionBridgeDllState::Failed)) {
                error = "production Gate-C receiver rollback is not verifiably complete";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        error = "production Gate-C receiver rollback verification timed out";
        return false;
    }

    void stopRouter(Session& session) noexcept {
        session.stopRouterRequested.store(true, std::memory_order_release);
        if (session.routerThread.joinable()) session.routerThread.join();
    }

    void cleanupFailedStart(Session& session) noexcept {
        stopRouter(session);
        if (session.mappingView != nullptr && session.process != nullptr &&
            processExactStillRunning(watchdogIdentity(
                session.request.process.authoritativeProcess))) {
            std::lock_guard channelLock(session.channelMutex);
            if (session.channel.valid()) {
                std::string ignored;
                (void)session.channel.writeFrame(
                    gatec::encodeShutdown(++session.sequence),
                    ioTimeoutMilliseconds_, &ignored);
            }
        }
        closeSessionHandles(session);
    }

    static void closeSessionHandles(Session& session) noexcept {
        session.channel.close();
        if (session.mappingView != nullptr) {
            UnmapViewOfFile(session.mappingView);
            session.mappingView = nullptr;
        }
        if (session.mapping != nullptr) {
            CloseHandle(session.mapping);
            session.mapping = nullptr;
        }
        if (session.process != nullptr) {
            CloseHandle(session.process);
            session.process = nullptr;
        }
    }
#endif

    std::uint32_t handshakeTimeoutMilliseconds_{0};
    std::uint32_t ioTimeoutMilliseconds_{0};
    std::mutex mutex_;
#ifdef _WIN32
    std::map<SeatId, std::unique_ptr<Session>> sessions_;
#endif
};

class SharedHidHideCoordinator final
    : public std::enable_shared_from_this<SharedHidHideCoordinator> {
public:
    explicit SharedHidHideCoordinator(std::shared_ptr<HidHideSessionPlatform> platform)
        : platform_(std::move(platform)) {}

    std::shared_ptr<HidHideSessionPlatform> platformForSeat(SeatId seatId) {
        class SeatView final : public HidHideSessionPlatform {
        public:
            SeatView(std::shared_ptr<SharedHidHideCoordinator> owner, SeatId seatId)
                : owner_(std::move(owner)), seatId_(seatId) {}
            bool readState(HidHideSessionSnapshot& snapshot,
                           std::string& error) noexcept override {
                return owner_->readSeatState(seatId_, snapshot, error);
            }
            bool writeState(const HidHideSessionSnapshot& snapshot,
                            std::string& error) noexcept override {
                return owner_->writeSeatState(seatId_, snapshot, error);
            }
            bool addSessionBlacklist(std::span<const std::wstring> ids,
                                     std::string& error) noexcept override {
                return owner_->addSeatSessionBlacklist(seatId_, ids, error);
            }
            bool clearSessionBlacklist(std::string& error) noexcept override {
                return owner_->clearSeatSessionBlacklist(seatId_, error);
            }
            bool mutationSupported() const noexcept override {
                return owner_->mutationSupported();
            }
            bool sessionBlacklistSupported() const noexcept override {
                return owner_->sessionBlacklistSupported();
            }
        private:
            std::shared_ptr<SharedHidHideCoordinator> owner_;
            SeatId seatId_{0};
        };
        return std::make_shared<SeatView>(shared_from_this(), seatId);
    }

    bool verifyGlobal(std::string& error) noexcept {
        std::lock_guard lock(mutex_);
        if (!ensureBaseLocked(error)) return false;
        HidHideSessionSnapshot observed;
        if (!platform_->readState(observed, error)) return false;
        const auto expected = combinedStateLocked();
        if (!equivalentHidHideSessionSnapshots(observed, expected)) {
            error = "global HidHide persistent state drifted from the exact two-Seat union";
            return false;
        }
        error.clear();
        return true;
    }

    bool mutationSupported() const noexcept {
        return platform_ && platform_->mutationSupported();
    }
    bool sessionBlacklistSupported() const noexcept {
        return platform_ && platform_->sessionBlacklistSupported();
    }

private:
    static void appendUnique(std::vector<std::wstring>& target,
                             std::span<const std::wstring> source,
                             bool pathLike) {
        for (const auto& value : source) {
            const auto folded = pathLike ? foldedPath(value) : foldedIdentity(value);
            const bool exists = std::any_of(target.begin(), target.end(),
                                            [&](const auto& current) {
                return (pathLike ? foldedPath(current) : foldedIdentity(current)) == folded;
            });
            if (!exists) target.push_back(value);
        }
    }

    bool ensureBaseLocked(std::string& error) noexcept {
        if (base_) return true;
        if (!platform_) {
            error = "shared HidHide coordinator has no platform";
            return false;
        }
        HidHideSessionSnapshot snapshot;
        if (!platform_->readState(snapshot, error)) return false;
        base_ = std::move(snapshot);
        return true;
    }

    HidHideSessionSnapshot combinedStateLocked() const {
        HidHideSessionSnapshot combined = *base_;
        for (const auto& [seatId, logical] : logicalStates_) {
            (void)seatId;
            if (logical.active) combined.active = true;
            appendUnique(combined.allowedApplications, logical.allowedApplications, true);
        }
        return combined;
    }

    bool applyCombinedLocked(std::string& error) noexcept {
        const auto combined = combinedStateLocked();
        if (!platform_->writeState(combined, error)) return false;
        HidHideSessionSnapshot observed;
        if (!platform_->readState(observed, error)) return false;
        if (!equivalentHidHideSessionSnapshots(observed, combined)) {
            error = "shared HidHide coordinator could not verify exact persistent union";
            return false;
        }
        return true;
    }

    bool readSeatState(SeatId seatId, HidHideSessionSnapshot& snapshot,
                       std::string& error) noexcept {
        std::lock_guard lock(mutex_);
        if (!ensureBaseLocked(error)) return false;
        const auto found = logicalStates_.find(seatId);
        snapshot = found == logicalStates_.end() ? *base_ : found->second;
        error.clear();
        return true;
    }

    bool writeSeatState(SeatId seatId, const HidHideSessionSnapshot& snapshot,
                        std::string& error) noexcept {
        std::lock_guard lock(mutex_);
        if (!ensureBaseLocked(error)) return false;
        const auto prior = logicalStates_.find(seatId);
        const std::optional<HidHideSessionSnapshot> priorValue =
            prior == logicalStates_.end() ? std::nullopt : std::optional{prior->second};
        logicalStates_[seatId] = snapshot;
        if (!applyCombinedLocked(error)) {
            if (priorValue) logicalStates_[seatId] = *priorValue;
            else logicalStates_.erase(seatId);
            return false;
        }
        if (!sessionScopes_.contains(seatId) &&
            equivalentHidHideSessionSnapshots(snapshot, *base_)) {
            logicalStates_.erase(seatId);
        }
        error.clear();
        return true;
    }

    bool addSeatSessionBlacklist(SeatId seatId, std::span<const std::wstring> ids,
                                 std::string& error) noexcept {
        std::lock_guard lock(mutex_);
        if (!ensureBaseLocked(error) || !platform_->sessionBlacklistSupported()) {
            if (error.empty()) error = "shared HidHide session blacklist is unsupported";
            return false;
        }
        if (ids.empty()) {
            error = "shared HidHide Seat session scope is empty";
            return false;
        }
        const auto found = sessionScopes_.find(seatId);
        if (found != sessionScopes_.end()) {
            if (std::vector<std::wstring>(ids.begin(), ids.end()) == found->second) {
                error.clear();
                return true;
            }
            error = "shared HidHide Seat session scope cannot change while active";
            return false;
        }
        if (!platform_->addSessionBlacklist(ids, error)) return false;
        sessionScopes_[seatId] = std::vector<std::wstring>(ids.begin(), ids.end());
        error.clear();
        return true;
    }

    bool clearSeatSessionBlacklist(SeatId seatId, std::string& error) noexcept {
        std::lock_guard lock(mutex_);
        if (!ensureBaseLocked(error)) return false;
        const auto found = sessionScopes_.find(seatId);
        if (found == sessionScopes_.end()) {
            error.clear();
            return true;
        }
        const auto removed = found->second;
        sessionScopes_.erase(found);
        if (!platform_->clearSessionBlacklist(error)) {
            sessionScopes_[seatId] = removed;
            return false;
        }
        std::vector<std::wstring> remaining;
        for (const auto& [otherSeat, ids] : sessionScopes_) {
            (void)otherSeat;
            appendUnique(remaining, ids, false);
        }
        if (!remaining.empty() && !platform_->addSessionBlacklist(remaining, error)) {
            // The driver list is now uncertain. Keep logical ownership so every
            // Seat enters recovery rather than pretending isolation survived.
            sessionScopes_[seatId] = removed;
            return false;
        }
        error.clear();
        return true;
    }

    std::shared_ptr<HidHideSessionPlatform> platform_;
    mutable std::mutex mutex_;
    std::optional<HidHideSessionSnapshot> base_;
    std::map<SeatId, HidHideSessionSnapshot> logicalStates_;
    std::map<SeatId, std::vector<std::wstring>> sessionScopes_;
};

std::uint64_t recoveryEpochFor(
    const ProductionProcessActivatedContext& processContext) noexcept {
    // RecoveryProcessAttachmentAuthority requires strictly increasing recovery
    // epochs only within the same Host-session + Seat-game generation. The
    // ProcessGroup handoff generation is already monotonic in exactly that
    // domain, so do not hash it into a non-monotonic lease number.
    return processContext.handoffGeneration ==
            (std::numeric_limits<std::uint64_t>::max)()
        ? processContext.handoffGeneration
        : processContext.handoffGeneration + 1u;
}

std::uint64_t monotonicMilliseconds() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct SeatBridgeState {
    launch::SeatActivationPlan plan;
    ProductionActivationContextHandle context;
    ProductionActivationBridgeConfig config;
    ProductionActivationBridgeDependencies dependencies;
    std::optional<ProductionGateCProfile> gateCProfile;
    std::shared_ptr<SharedHidHideCoordinator> hidHideCoordinator;
    std::shared_ptr<HidHideSessionPlatform> hidHideSeatPlatform;
    std::shared_ptr<std::atomic<bool>> globalPhysicalRecoveryRequired;

    mutable std::mutex mutex;
    std::optional<recovery::RecoveryProcessAttachmentRegistration> recoveryRegistration;
    std::vector<std::uint32_t> verifiedRolledBackActionIds;
    // Exact live Gate-C observation authority. Populated only after Input activation
    // and recovery commit succeed; cleared before/with rollback terminal state.
    std::optional<ProductionGateCSessionRequest> activeGateRequest;
    bool inputExpected{false};
    bool inputAttempted{false};
    bool inputActive{false};
    bool inputTerminal{false};
    bool recoveryFaulted{false};
    bool forceRecoveryTrigger{false};
    std::string diagnostic;

    bool registerRecoveryLocked(
        const ProductionProcessActivatedContext& processContext,
        std::span<const watchdog::RollbackActionDescriptor> additionalActions,
        std::string& error) {
        if (recoveryRegistration) {
            const auto& identity = recoveryRegistration->identity;
            const auto current = watchdogIdentity(processContext.authoritativeProcess);
            if (identity.seatId == processContext.epoch.seatId &&
                identity.hostSessionId == processContext.epoch.sessionId &&
                identity.sessionGeneration == processContext.epoch.sessionGeneration &&
                identity.seatGameGeneration == processContext.epoch.seatGameGeneration &&
                identity.process == current) {
                const auto authority = dependencies.recoveryAttachmentAuthority->verifyArmed(
                    recoveryRegistration->identity, recoveryRegistration->manifest.lease);
                if (!authority.succeeded()) {
                    error = "exact recovery attachment authority is no longer armed: " +
                            authority.diagnostic;
                    return false;
                }
                return dependencies.recoveryLeaseRuntime->verifyArmed(
                    *recoveryRegistration, error);
            }
            error = "another exact process recovery registration must be retired before handoff re-attachment";
            return false;
        }
        const auto recoveryEpoch = recoveryEpochFor(processContext);
        auto registration = makeRecoveryRegistration(
            processContext, recoveryEpoch, additionalActions,
            config.recoveryLeaseTimeoutMilliseconds,
            config.recoveryRollbackTimeoutMilliseconds,
            config.recoveryActionTimeoutMilliseconds, error);
        if (!registration) return false;
        const auto authorityResult = dependencies.recoveryAttachmentAuthority->registerAttachment(
            *registration);
        if (!authorityResult.succeeded()) {
            error = "exact recovery attachment authority rejected registration: " +
                    authorityResult.diagnostic;
            return false;
        }
        if (!dependencies.recoveryLeaseRuntime->arm(*registration, error)) {
            std::string disarmedError;
            if (dependencies.recoveryLeaseRuntime->verifyDisarmed(
                    *registration, disarmedError)) {
                const auto ignored = dependencies.recoveryAttachmentAuthority->disarm(
                    registration->identity, registration->manifest.lease);
                (void)ignored;
            } else {
                // The runtime may have crossed a side-effect boundary before
                // reporting failure. Retain the exact authority/manifest so
                // reverse rollback can clean it after Process teardown.
                recoveryRegistration = *registration;
                verifiedRolledBackActionIds.clear();
                if (!disarmedError.empty()) {
                    error += "; cleanup state is not verifiably disarmed: " +
                             disarmedError;
                }
            }
            return false;
        }
        if (!dependencies.recoveryLeaseRuntime->verifyArmed(*registration, error)) {
            recoveryRegistration = *registration;
            verifiedRolledBackActionIds.clear();
            return false;
        }
        recoveryRegistration = std::move(*registration);
        verifiedRolledBackActionIds.clear();
        return true;
    }

    bool disarmRecoveryLocked(std::string& error) {
        if (!recoveryRegistration) {
            error.clear();
            return true;
        }
        const auto registration = *recoveryRegistration;
        if (!dependencies.recoveryLeaseRuntime->disarm(
                registration, verifiedRolledBackActionIds, error) ||
            !dependencies.recoveryLeaseRuntime->verifyDisarmed(registration, error)) {
            return false;
        }
        const auto authorityResult =
            dependencies.recoveryAttachmentAuthority->disarm(
                registration.identity, registration.manifest.lease);
        if (!authorityResult.succeeded()) {
            error = "exact recovery attachment authority rejected clean disarm: " +
                    authorityResult.diagnostic;
            return false;
        }
        if (dependencies.recoveryAttachmentAuthority->verifyArmed(
                registration.identity, registration.manifest.lease).code !=
            recovery::RecoveryAttachmentCode::NotArmed) {
            error = "exact recovery attachment authority still reports the disarmed lease";
            return false;
        }
        recoveryRegistration.reset();
        verifiedRolledBackActionIds.clear();
        return true;
    }

    bool recoveryMatchesProcessLocked(
        const ProductionProcessActivatedContext& processContext) const noexcept {
        if (!recoveryRegistration) return false;
        return recoveryRegistration->identity.seatId == processContext.epoch.seatId &&
               recoveryRegistration->identity.hostSessionId == processContext.epoch.sessionId &&
               recoveryRegistration->identity.sessionGeneration ==
                   processContext.epoch.sessionGeneration &&
               recoveryRegistration->identity.seatGameGeneration ==
                   processContext.epoch.seatGameGeneration &&
               recoveryRegistration->identity.process ==
                   watchdogIdentity(processContext.authoritativeProcess);
    }
};

class ProductionRecoveryResource final : public launch::ISeatActivationResource {
public:
    explicit ProductionRecoveryResource(std::shared_ptr<SeatBridgeState> state)
        : state_(std::move(state)) {}
    ~ProductionRecoveryResource() override { stopMonitor(); }

    launch::ResourceKind kind() const noexcept override {
        return launch::ResourceKind::Recovery;
    }

    bool prepare(const launch::SeatActivationPlan& plan,
                 const runtime::SeatGameBinding& binding,
                 std::string& error) override {
        if (binding.gameId != plan.target.gameId || plan.seatId != state_->plan.seatId ||
            plan.fingerprint != state_->plan.fingerprint || !state_->context ||
            !state_->dependencies.recoveryAttachmentAuthority ||
            !state_->dependencies.recoveryLeaseRuntime) {
            error = "production Recovery bridge is missing immutable plan/context/recovery authority";
            return false;
        }
        const auto snapshot = state_->context->snapshot();
        if (!contextEpochMatchesPlan(plan, snapshot, state_->context, error) ||
            snapshot.stage != ProductionActivationContextStage::PreProcess ||
            snapshot.process || snapshot.handoffState) {
            if (error.empty()) error = "production Recovery prepare requires explicit PreProcess context";
            return false;
        }
        prepared_ = true;
        error.clear();
        return true;
    }

    bool activate(std::string& error) override {
        if (!prepared_ || monitor_.joinable()) {
            error = "production Recovery resource is not prepared or is already activated";
            return false;
        }
        stopRequested_.store(false, std::memory_order_release);
        monitor_ = std::thread([this] { monitorLoop(); });
        activated_ = true;
        error.clear();
        return true;
    }

    bool verifyActive(std::string& error) override {
        if (!activated_) {
            error = "production Recovery resource is not in its staged active lifecycle";
            return false;
        }
        std::lock_guard lock(state_->mutex);
        if (state_->recoveryFaulted || state_->forceRecoveryTrigger ||
            (state_->globalPhysicalRecoveryRequired &&
             state_->globalPhysicalRecoveryRequired->load(std::memory_order_acquire))) {
            error = state_->diagnostic.empty()
                ? "production Recovery monitor entered an unverifiable state"
                : state_->diagnostic;
            return false;
        }
        // Recovery is ordered before Process. PreProcess/AwaitingExactProcess is
        // intentionally a valid staged resource state, not a fake process lease.
        error.clear();
        return true;
    }

    bool rollback(std::string& error) noexcept override {
        stopMonitor();
        try {
            std::lock_guard lock(state_->mutex);
            if (state_->recoveryRegistration) {
                if (processExactStillRunning(state_->recoveryRegistration->identity.process)) {
                    error = "Recovery rollback reached the exact attachment before Process teardown";
                    return false;
                }
                if (!state_->disarmRecoveryLocked(error)) {
                    state_->recoveryFaulted = true;
                    state_->diagnostic = error;
                    return false;
                }
            }
            activated_ = false;
            error.clear();
            return true;
        } catch (...) {
            error = "production Recovery rollback threw unexpectedly";
            return false;
        }
    }

    bool verifySafe(std::string& error) noexcept override {
        try {
            std::lock_guard lock(state_->mutex);
            if (state_->recoveryRegistration) {
                error = "production Recovery attachment remains armed after rollback";
                return false;
            }
            if (state_->recoveryFaulted && !state_->diagnostic.empty()) {
                error = state_->diagnostic;
                return false;
            }
            error.clear();
            return true;
        } catch (...) {
            error = "production Recovery safe-state verification threw unexpectedly";
            return false;
        }
    }

    bool active() const noexcept override {
        std::lock_guard lock(state_->mutex);
        return activated_ && !state_->recoveryFaulted &&
               state_->recoveryRegistration.has_value();
    }

private:
    void stopMonitor() noexcept {
        stopRequested_.store(true, std::memory_order_release);
        if (monitor_.joinable()) monitor_.join();
    }

    void monitorLoop() noexcept {
        const auto interval = std::chrono::milliseconds(
            (std::max)(state_->config.monitorIntervalMilliseconds, 1u));
        while (!stopRequested_.load(std::memory_order_acquire)) {
            try {
                const auto snapshot = state_->context->snapshot();
                std::lock_guard lock(state_->mutex);
                if (state_->forceRecoveryTrigger ||
                    (state_->globalPhysicalRecoveryRequired &&
                     state_->globalPhysicalRecoveryRequired->load(
                         std::memory_order_acquire))) {
                    // Stop renewing. The already-armed watchdog/reset plan now
                    // owns emergency rollback for this exact Seat/process.
                    return;
                }
                if (!state_->context->validatesEpoch(snapshot.epoch)) {
                    if (state_->recoveryRegistration &&
                        processExactStillRunning(state_->recoveryRegistration->identity.process)) {
                        state_->recoveryFaulted = true;
                        state_->diagnostic = "activation epoch invalidated while exact recovery attachment remains live";
                        return;
                    }
                }
                if (snapshot.stage == ProductionActivationContextStage::ProcessActive &&
                    snapshot.process &&
                    state_->context->validatesCurrentProcess(*snapshot.process)) {
                    if (state_->recoveryRegistration &&
                        !state_->recoveryMatchesProcessLocked(*snapshot.process)) {
                        if (state_->inputActive ||
                            processExactStillRunning(state_->recoveryRegistration->identity.process)) {
                            // Do not guess/replace while old exact ownership is live.
                        } else {
                            std::string disarmError;
                            if (!state_->disarmRecoveryLocked(disarmError)) {
                                state_->recoveryFaulted = true;
                                state_->diagnostic = disarmError;
                                return;
                            }
                        }
                    }
                    if (state_->recoveryRegistration) {
                        std::string verifyError;
                        if (!state_->dependencies.recoveryLeaseRuntime->verifyArmed(
                                *state_->recoveryRegistration, verifyError)) {
                            state_->recoveryFaulted = true;
                            state_->diagnostic = verifyError;
                            return;
                        }
                    } else if (!(state_->inputExpected && !state_->inputAttempted)) {
                        std::string armError;
                        if (!state_->registerRecoveryLocked(*snapshot.process, {}, armError)) {
                            state_->recoveryFaulted = true;
                            state_->diagnostic = armError;
                            return;
                        }
                    }
                } else if ((snapshot.stage == ProductionActivationContextStage::ProcessExited ||
                            snapshot.stage == ProductionActivationContextStage::ProcessInvalidated) &&
                           state_->recoveryRegistration && !state_->inputActive &&
                           !processExactStillRunning(state_->recoveryRegistration->identity.process)) {
                    std::string disarmError;
                    if (!state_->disarmRecoveryLocked(disarmError)) {
                        state_->recoveryFaulted = true;
                        state_->diagnostic = disarmError;
                        return;
                    }
                } else if (snapshot.stage == ProductionActivationContextStage::ProcessUnverifiable ||
                           snapshot.stage == ProductionActivationContextStage::UnsupportedContainment) {
                    state_->recoveryFaulted = true;
                    state_->diagnostic = "production process authority became unverifiable while Recovery was active";
                    return;
                }
            } catch (...) {
                std::lock_guard lock(state_->mutex);
                state_->recoveryFaulted = true;
                state_->diagnostic = "production Recovery monitor threw unexpectedly";
                return;
            }
            std::this_thread::sleep_for(interval);
        }
    }

    std::shared_ptr<SeatBridgeState> state_;
    std::atomic<bool> stopRequested_{false};
    std::thread monitor_;
    bool prepared_{false};
    bool activated_{false};
};

class ProductionInputResource final : public launch::ISeatActivationResource {
public:
    explicit ProductionInputResource(std::shared_ptr<SeatBridgeState> state)
        : state_(std::move(state)) {}
    ~ProductionInputResource() override { stopMonitor(); }

    launch::ResourceKind kind() const noexcept override {
        return launch::ResourceKind::Input;
    }

    bool prepare(const launch::SeatActivationPlan& plan,
                 const runtime::SeatGameBinding& binding,
                 std::string& error) override {
        if (binding.gameId != plan.target.gameId || plan.seatId != state_->plan.seatId ||
            plan.fingerprint != state_->plan.fingerprint || !state_->context ||
            !state_->gateCProfile || !state_->dependencies.gateCSessionRuntime ||
            !state_->dependencies.recoveryAttachmentAuthority ||
            !state_->dependencies.recoveryLeaseRuntime) {
            error = "production Input bridge is missing immutable plan/profile/runtime authority";
            return false;
        }
        const auto snapshot = state_->context->snapshot();
        if (!contextEpochMatchesPlan(plan, snapshot, state_->context, error) ||
            snapshot.stage != ProductionActivationContextStage::PreProcess || snapshot.process) {
            if (error.empty()) error = "production Input prepare requires explicit PreProcess context";
            return false;
        }
        if (!validateGateCProfile(*state_->gateCProfile, error) ||
            !resolvePhysicalScope(plan, state_->config, stableDeviceIds_,
                                  deviceInstanceIds_, error)) {
            return false;
        }
        prepared_ = true;
        error.clear();
        return true;
    }

    bool activate(std::string& error) override {
        if (!prepared_) {
            error = "production Input resource was not prepared";
            return false;
        }
        ProductionProcessActivatedContext processContext;
        if (!currentProcessContext(state_->plan, state_->context, processContext, error) ||
            !exactPathAllowed(processContext.authoritativeProcess, *state_->gateCProfile)) {
            if (error.empty()) error = "production Input activation lacks exact allowed process authority";
            return false;
        }
        gateRequest_ = ProductionGateCSessionRequest{
            processContext.epoch, processContext, state_->plan.seat,
            *state_->gateCProfile, stableDeviceIds_};
        if (!state_->dependencies.gateCSessionRuntime->start(*gateRequest_, error)) {
            return false;
        }
        ProductionGateCSessionStatus gateStatus;
        if (!state_->dependencies.gateCSessionRuntime->verify(
                *gateRequest_, gateStatus, error) || !gateStatus.active ||
            !gateStatus.receiverVerified || !gateStatus.assignedDevicesPresent ||
            !gateStatus.process.sameInstance(processContext.authoritativeProcess) ||
            gateStatus.handoffGeneration != processContext.handoffGeneration) {
            if (error.empty()) error = "Gate-C receiver-side verification did not prove exact current process/input ownership";
            std::string stopError;
            (void)state_->dependencies.gateCSessionRuntime->stop(*gateRequest_, stopError);
            return false;
        }

        const auto recoveryEpoch = recoveryEpochFor(processContext);
        std::vector<watchdog::RollbackActionDescriptor> recoveryActions;
        if (state_->gateCProfile->physicalCloakingRequired) {
            if (!state_->hidHideSeatPlatform || !state_->hidHideCoordinator) {
                error = "production physical input profile requires a coordinated HidHide platform";
                cleanupGateOnly();
                return false;
            }
            hidHide_ = std::make_unique<GuardedHidHideSession>(
                state_->hidHideSeatPlatform,
                seatRecoveryRoot(state_->config.recoveryRoot, state_->plan.seatId),
                hidHideResourceId(processContext.epoch, recoveryEpoch),
                hidHideActionId(state_->plan.seatId), 2u,
                state_->config.recoveryActionTimeoutMilliseconds);
            HidHideSessionRequest request;
            request.deviceInstanceIds = deviceInstanceIds_;
            request.allowedApplications = state_->gateCProfile->hidHideAllowedApplications;
            request.replacementPathVerified = gateStatus.receiverVerified &&
                                              gateStatus.assignedDevicesPresent;
            request.recoveryReady = true;
            request.spareRecoveryInputPresent =
                state_->gateCProfile->spareRecoveryInputPresent;
            request.physicalAcceptanceEvidence = state_->config.physicalAcceptanceEvidence;
            request.physicalEvidenceSeatId = state_->plan.seatId;
            request.nativeMutationApproved =
                state_->gateCProfile->nativeHidHideMutationApproved;
            request.expiryMilliseconds = state_->gateCProfile->hidHideExpiryMilliseconds;
            request.generation = recoveryEpoch;
            const auto prepared = hidHide_->prepare(std::move(request), monotonicMilliseconds());
            if (!prepared.succeeded() || prepared.phase != HidHideSessionPhase::Prepared ||
                !hidHide_->rollbackAction()) {
                error = "guarded HidHide prepare failed: " + prepared.diagnostic;
                cleanupGateOnly();
                return false;
            }
            recoveryActions.push_back(*hidHide_->rollbackAction());
        }

        {
            std::lock_guard lock(state_->mutex);
            state_->inputAttempted = true;
            if (state_->recoveryRegistration) {
                error = "production Input activation found a pre-existing recovery plan; full physical plan cannot be upgraded in place";
                cleanupGateOnly();
                return false;
            }
            if (!state_->registerRecoveryLocked(processContext, recoveryActions, error)) {
                cleanupGateOnly();
                return false;
            }
        }

        if (hidHide_) {
            std::optional<recovery::RecoveryProcessAttachmentRegistration> registration;
            {
                std::lock_guard lock(state_->mutex);
                registration = state_->recoveryRegistration;
            }
            if (!registration ||
                !hidHide_->confirmRecoveryArmed(*hidHide_->rollbackAction(), &error) ||
                !state_->dependencies.recoveryLeaseRuntime->markActionActive(
                    *registration, hidHide_->rollbackAction()->actionId, error)) {
                return failActivationAfterRecoveryArm(error);
            }
            const auto activated = hidHide_->activate(monotonicMilliseconds());
            if (!activated.succeeded() || activated.phase != HidHideSessionPhase::Active) {
                error = "guarded HidHide activation/verification failed: " + activated.diagnostic;
                return failActivationAfterRecoveryArm(error);
            }
            if (!state_->dependencies.recoveryLeaseRuntime->commit(*registration, error)) {
                return failActivationAfterRecoveryArm(error);
            }
        } else {
            std::optional<recovery::RecoveryProcessAttachmentRegistration> registration;
            {
                std::lock_guard lock(state_->mutex);
                registration = state_->recoveryRegistration;
            }
            if (!registration ||
                !state_->dependencies.recoveryLeaseRuntime->commit(*registration, error)) {
                return failActivationAfterRecoveryArm(error);
            }
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->activeGateRequest = *gateRequest_;
            state_->inputActive = true;
            state_->inputTerminal = false;
            state_->diagnostic.clear();
        }
        active_.store(true, std::memory_order_release);
        startMonitor();
        error.clear();
        return true;
    }

    bool verifyActive(std::string& error) override {
        if (!active_.load(std::memory_order_acquire) || !gateRequest_) {
            error = "production Input resource is not active";
            return false;
        }
        ProductionProcessActivatedContext current;
        if (!currentProcessContext(state_->plan, state_->context, current, error) ||
            !current.authoritativeProcess.sameInstance(
                gateRequest_->process.authoritativeProcess) ||
            current.handoffGeneration != gateRequest_->process.handoffGeneration) {
            if (error.empty()) error = "production Input process authority changed after activation";
            return false;
        }
        ProductionGateCSessionStatus status;
        if (!state_->dependencies.gateCSessionRuntime->verify(
                *gateRequest_, status, error) || !status.receiverVerified ||
            !status.assignedDevicesPresent) {
            return false;
        }
        if (state_->globalPhysicalRecoveryRequired &&
            state_->globalPhysicalRecoveryRequired->load(std::memory_order_acquire)) {
            error = "shared physical input state is recovery-required";
            return false;
        }
        if (hidHide_ && state_->hidHideCoordinator &&
            !state_->hidHideCoordinator->verifyGlobal(error)) {
            return false;
        }
        std::lock_guard lock(state_->mutex);
        if (!state_->recoveryRegistration) {
            error = "production Input no longer has an exact recovery attachment";
            return false;
        }
        const auto authority = state_->dependencies.recoveryAttachmentAuthority->verifyArmed(
            state_->recoveryRegistration->identity,
            state_->recoveryRegistration->manifest.lease);
        const bool runtimeVerified = authority.succeeded() &&
            state_->dependencies.recoveryLeaseRuntime->verifyArmed(
                *state_->recoveryRegistration, error);
        if (!authority.succeeded() || !runtimeVerified) {
            if (error.empty()) {
                error = authority.diagnostic.empty()
                    ? "exact recovery lease verification failed while Input was active"
                    : authority.diagnostic;
            }
            // Input and Recovery share the same exact attachment authority. Once
            // Input observes that authority as unverifiable, Recovery must expose
            // the loss synchronously rather than waiting for a later monitor tick.
            state_->recoveryFaulted = true;
            state_->diagnostic = error;
            return false;
        }
        error.clear();
        return true;
    }

    bool rollback(std::string& error) noexcept override {
        stopMonitor();
        try {
            return rollbackInternal(error, false);
        } catch (...) {
            error = "production Input rollback threw unexpectedly";
            return false;
        }
    }

    bool verifySafe(std::string& error) noexcept override {
        try {
            if (active_.load(std::memory_order_acquire)) {
                error = "production Input still reports active after rollback";
                return false;
            }
            if (!lastRollbackVerified_) {
                error = lastError_.empty()
                    ? "production Input rollback has not been authoritatively verified"
                    : lastError_;
                return false;
            }
            error.clear();
            return true;
        } catch (...) {
            error = "production Input safe-state verification threw unexpectedly";
            return false;
        }
    }

    bool active() const noexcept override {
        return active_.load(std::memory_order_acquire);
    }

private:
    void startMonitor() {
        stopRequested_.store(false, std::memory_order_release);
        monitor_ = std::thread([this] {
            const auto interval = std::chrono::milliseconds(
                (std::max)(state_->config.monitorIntervalMilliseconds, 1u));
            while (!stopRequested_.load(std::memory_order_acquire)) {
                std::string error;
                bool healthy = verifyActive(error);
                if (!healthy) {
                    std::lock_guard resourceLock(resourceMutex_);
                    if (!stopRequested_.load(std::memory_order_acquire)) {
                        std::string rollbackError;
                        const bool safe = rollbackInternal(rollbackError, true);
                        std::lock_guard stateLock(state_->mutex);
                        state_->inputTerminal = true;
                        state_->diagnostic = safe ? error : rollbackError;
                        if (!safe) state_->forceRecoveryTrigger = true;
                    }
                    return;
                }
                std::this_thread::sleep_for(interval);
            }
        });
    }

    void stopMonitor() noexcept {
        stopRequested_.store(true, std::memory_order_release);
        if (monitor_.joinable() && monitor_.get_id() != std::this_thread::get_id())
            monitor_.join();
    }

    bool rollbackInternal(std::string& error, bool monitorPath) noexcept {
        if (!monitorPath) {
            std::lock_guard resourceLock(resourceMutex_);
            return rollbackLocked(error);
        }
        return rollbackLocked(error);
    }

    bool rollbackLocked(std::string& error) noexcept {
        {
            std::lock_guard lock(state_->mutex);
            // Stop exposing observation authority before any receiver/cloaking
            // teardown starts. A snapshot racing with rollback must fail closed.
            state_->activeGateRequest.reset();
            state_->inputActive = false;
            state_->inputTerminal = true;
        }
        bool hidHideSafe = true;
        bool gateSafe = true;
        std::string firstError;
        if (hidHide_) {
            const auto result = hidHide_->rollback();
            hidHideSafe = result.succeeded() && result.phase == HidHideSessionPhase::Idle;
            std::string coordinatorError;
            if (hidHideSafe && state_->hidHideCoordinator) {
                hidHideSafe = state_->hidHideCoordinator->verifyGlobal(coordinatorError);
            }
            if (hidHideSafe && hidHide_->rollbackAction()) {
                std::lock_guard lock(state_->mutex);
                const auto actionId = hidHide_->rollbackAction()->actionId;
                if (std::find(state_->verifiedRolledBackActionIds.begin(),
                              state_->verifiedRolledBackActionIds.end(), actionId) ==
                    state_->verifiedRolledBackActionIds.end()) {
                    state_->verifiedRolledBackActionIds.push_back(actionId);
                }
            } else if (!hidHideSafe) {
                firstError = !coordinatorError.empty()
                    ? coordinatorError
                    : result.diagnostic.empty()
                        ? "guarded HidHide rollback could not be verified"
                        : result.diagnostic;
            }
        }
        if (gateRequest_) {
            std::string gateError;
            gateSafe = state_->dependencies.gateCSessionRuntime->stop(
                *gateRequest_, gateError);
            if (!gateSafe && firstError.empty()) firstError = gateError;
        }
        active_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(state_->mutex);
            state_->inputActive = false;
            state_->inputTerminal = true;
            if (!hidHideSafe || !gateSafe) {
                state_->diagnostic = firstError;
                state_->forceRecoveryTrigger = true;
                if (!hidHideSafe && state_->globalPhysicalRecoveryRequired) {
                    // HidHide persistent/session state is shared by the Host
                    // process. If one Seat cannot prove restoration, every Seat
                    // must stop lease renewal and let exact watchdog plans enter
                    // emergency rollback; isolating the failure would be false.
                    state_->globalPhysicalRecoveryRequired->store(
                        true, std::memory_order_release);
                }
            }
        }
        lastRollbackVerified_ = hidHideSafe && gateSafe;
        lastError_ = firstError;
        error = firstError;
        return lastRollbackVerified_;
    }

    bool failActivationAfterRecoveryArm(std::string& error) {
        std::lock_guard resourceLock(resourceMutex_);
        return failActivationAfterRecoveryArmUnlocked(error);
    }

    bool failActivationAfterRecoveryArmUnlocked(std::string& error) {
        const auto activationError = error;
        std::string rollbackError;
        const bool safe = rollbackLocked(rollbackError);
        if (!safe) {
            error = activationError + "; rollback verification failed: " + rollbackError;
        } else {
            error = activationError;
        }
        return false;
    }

    void cleanupGateOnly() noexcept {
        if (!gateRequest_) return;
        std::string ignored;
        (void)state_->dependencies.gateCSessionRuntime->stop(*gateRequest_, ignored);
        gateRequest_.reset();
    }

    std::shared_ptr<SeatBridgeState> state_;
    std::vector<std::wstring> stableDeviceIds_;
    std::vector<std::wstring> deviceInstanceIds_;
    std::optional<ProductionGateCSessionRequest> gateRequest_;
    std::unique_ptr<GuardedHidHideSession> hidHide_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread monitor_;
    std::mutex resourceMutex_;
    bool prepared_{false};
    bool lastRollbackVerified_{false};
    std::string lastError_;
};

} // namespace

class ProductionActivationResourceBridges::Impl {
public:
    Impl(ProductionActivationBridgeConfig config,
         ProductionActivationBridgeDependencies dependencies)
        : config_(std::move(config)), dependencies_(std::move(dependencies)) {
        if (!dependencies_.recoveryAttachmentAuthority) {
            dependencies_.recoveryAttachmentAuthority =
                std::make_shared<recovery::RecoveryProcessAttachmentAuthority>();
        }
        if (!dependencies_.recoveryLeaseRuntime) {
            dependencies_.recoveryLeaseRuntime = makeNativeProductionRecoveryLeaseRuntime(
                config_.recoveryRoot, config_.watchdogExecutablePath,
                config_.gateCHandshakeTimeoutMilliseconds);
        }
        if (!dependencies_.gateCSessionRuntime) {
            dependencies_.gateCSessionRuntime = makeNativeProductionGateCSessionRuntime(
                config_.gateCHandshakeTimeoutMilliseconds,
                config_.gateCIoTimeoutMilliseconds);
        }
        if (!config_.hidHidePlatform) {
            config_.hidHidePlatform = makeNativeHidHideSessionPlatform();
        }
        hidHideCoordinator_ =
            std::make_shared<SharedHidHideCoordinator>(config_.hidHidePlatform);
        globalPhysicalRecoveryRequired_ = std::make_shared<std::atomic<bool>>(false);
    }

    std::shared_ptr<SeatBridgeState> stateFor(
        const launch::SeatActivationPlan& plan,
        ProductionActivationContextHandle context,
        bool requireInputProfile,
        std::string& error) {
        if (!context || plan.seatId == 0 || plan.seatId > kV1SeatLimit ||
            plan.fingerprint == 0 || config_.gateCProfiles.size() >
                kMaximumProductionGateCProfiles) {
            error = "production activation bridge received invalid bounded Seat configuration";
            return {};
        }
        const auto snapshot = context->snapshot();
        if (!contextEpochMatchesPlan(plan, snapshot, context, error) ||
            snapshot.stage != ProductionActivationContextStage::PreProcess) {
            if (error.empty()) error = "production activation bridge creation requires PreProcess context";
            return {};
        }

        std::set<std::string> gameIds;
        for (const auto& candidate : config_.gateCProfiles) {
            if (!gameIds.insert(candidate.gameId).second) {
                error = "production Gate-C profile table contains duplicate game authority";
                return {};
            }
            if (!validateGateCProfile(candidate, error)) return {};
        }

        const bool inputExpected = plan.target.requirements.keyboard ||
                                   plan.target.requirements.mouse;
        const ProductionGateCProfile* profile =
            inputExpected ? findProfile(config_, plan.target.gameId) : nullptr;
        if (inputExpected && !plan.target.requirements.recovery) {
            error = "production physical Gate-C input requires the generic Recovery resource so exact watchdog cleanup outlives Input rollback";
            return {};
        }
        if (requireInputProfile && inputExpected && profile == nullptr) {
            error = "no trusted production Gate-C profile exists for this exact game";
            return {};
        }

        const auto key = std::make_pair(plan.seatId, plan.fingerprint);
        std::lock_guard lock(mutex_);
        if (const auto found = states_.find(key); found != states_.end()) {
            if (const auto existing = found->second.lock()) {
                if (existing->context != context) {
                    error = "same immutable Seat activation key was presented with a different context authority";
                    return {};
                }
                error.clear();
                return existing;
            }
            states_.erase(found);
        }
        for (auto iterator = states_.begin(); iterator != states_.end();) {
            if (iterator->second.expired()) iterator = states_.erase(iterator);
            else ++iterator;
        }
        auto state = std::make_shared<SeatBridgeState>();
        state->plan = plan;
        state->context = std::move(context);
        state->config = config_;
        state->dependencies = dependencies_;
        state->inputExpected = inputExpected;
        state->hidHideCoordinator = hidHideCoordinator_;
        state->hidHideSeatPlatform = hidHideCoordinator_->platformForSeat(plan.seatId);
        state->globalPhysicalRecoveryRequired = globalPhysicalRecoveryRequired_;
        if (profile != nullptr) state->gateCProfile = *profile;
        states_[key] = state;
        error.clear();
        return state;
    }

    bool inputMetricsSnapshot(const launch::SeatActivationPlan& plan,
                              InputMetricsSnapshot& snapshot,
                              std::string& error) {
        snapshot = {};
        if (plan.seatId == 0 || plan.seatId > kV1SeatLimit || plan.fingerprint == 0) {
            error = "production input metrics observation requires an exact bounded Seat plan";
            return false;
        }

        std::shared_ptr<SeatBridgeState> state;
        {
            std::lock_guard lock(mutex_);
            const auto found = states_.find(std::make_pair(plan.seatId, plan.fingerprint));
            if (found == states_.end()) {
                error = "no live production activation state exists for this Seat/fingerprint";
                return false;
            }
            state = found->second.lock();
        }
        if (!state) {
            error = "production activation state expired before input metrics observation";
            return false;
        }

        ProductionGateCSessionRequest request;
        ProductionActivationContextHandle context;
        std::shared_ptr<IProductionGateCSessionRuntime> runtime;
        {
            std::lock_guard lock(state->mutex);
            if (state->plan.seatId != plan.seatId ||
                state->plan.fingerprint != plan.fingerprint ||
                !state->inputActive || state->inputTerminal ||
                !state->activeGateRequest ||
                !state->dependencies.gateCSessionRuntime) {
                error = "production input metrics observation requires one exact active Gate-C session";
                return false;
            }
            request = *state->activeGateRequest;
            context = state->context;
            runtime = state->dependencies.gateCSessionRuntime;
        }

        ProductionProcessActivatedContext current;
        if (!currentProcessContext(plan, context, current, error) ||
            !current.authoritativeProcess.sameInstance(
                request.process.authoritativeProcess) ||
            current.handoffGeneration != request.process.handoffGeneration ||
            current.epoch != request.epoch) {
            if (error.empty()) {
                error = "production input metrics process authority changed before observation";
            }
            return false;
        }

        ProductionGateCSessionStatus status;
        if (!runtime->verify(request, status, error) || !status.active ||
            !status.receiverVerified || !status.assignedDevicesPresent ||
            !status.process.sameInstance(request.process.authoritativeProcess) ||
            status.handoffGeneration != request.process.handoffGeneration) {
            if (error.empty()) {
                error = "production input metrics receiver verification failed";
            }
            return false;
        }

        InputMetricsSnapshot candidate;
        if (!runtime->inputMetricsSnapshot(request, candidate, error)) {
            snapshot = {};
            return false;
        }

        ProductionProcessActivatedContext after;
        if (!currentProcessContext(plan, context, after, error) ||
            !after.authoritativeProcess.sameInstance(
                request.process.authoritativeProcess) ||
            after.handoffGeneration != request.process.handoffGeneration ||
            after.epoch != request.epoch) {
            snapshot = {};
            if (error.empty()) {
                error = "production input metrics process authority changed during observation";
            }
            return false;
        }
        {
            std::lock_guard lock(state->mutex);
            if (!state->inputActive || state->inputTerminal ||
                !state->activeGateRequest ||
                !sameGateRequestAuthority(*state->activeGateRequest, request)) {
                snapshot = {};
                error = "production input metrics session became stale during observation";
                return false;
            }
        }
        snapshot = std::move(candidate);
        error.clear();
        return true;
    }

private:
    ProductionActivationBridgeConfig config_;
    ProductionActivationBridgeDependencies dependencies_;
    std::shared_ptr<SharedHidHideCoordinator> hidHideCoordinator_;
    std::shared_ptr<std::atomic<bool>> globalPhysicalRecoveryRequired_;
    std::mutex mutex_;
    std::map<std::pair<SeatId, std::uint64_t>, std::weak_ptr<SeatBridgeState>> states_;
};

ProductionActivationResourceBridges::ProductionActivationResourceBridges(
    ProductionActivationBridgeConfig config,
    ProductionActivationBridgeDependencies dependencies)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(dependencies))) {}

ProductionActivationResourceBridges::~ProductionActivationResourceBridges() = default;

std::unique_ptr<launch::ISeatActivationResource>
ProductionActivationResourceBridges::createRecoveryResource(
    const launch::SeatActivationPlan& plan,
    ProductionActivationContextHandle context,
    std::string& error) {
    const auto state = impl_->stateFor(plan, std::move(context), false, error);
    if (!state) return {};
    error.clear();
    return std::make_unique<ProductionRecoveryResource>(state);
}

std::unique_ptr<launch::ISeatActivationResource>
ProductionActivationResourceBridges::createInputResource(
    const launch::SeatActivationPlan& plan,
    ProductionActivationContextHandle context,
    std::string& error) {
    const auto state = impl_->stateFor(plan, std::move(context), true, error);
    if (!state) return {};
    if (!state->inputExpected || !state->gateCProfile) {
        error = "production Input bridge was requested for a plan without keyboard/mouse Gate-C requirements";
        return {};
    }
    error.clear();
    return std::make_unique<ProductionInputResource>(state);
}

bool ProductionActivationResourceBridges::inputMetricsSnapshot(
    const launch::SeatActivationPlan& plan,
    InputMetricsSnapshot& snapshot,
    std::string& error) {
    return impl_->inputMetricsSnapshot(plan, snapshot, error);
}

std::shared_ptr<ProductionActivationResourceBridges>
makeProductionActivationResourceBridges(
    ProductionActivationBridgeConfig config,
    ProductionActivationBridgeDependencies dependencies) {
    return std::make_shared<ProductionActivationResourceBridges>(
        std::move(config), std::move(dependencies));
}

std::shared_ptr<ProductionActivationResourceBridges>
makeDefaultProductionActivationResourceBridges(std::string* error) {
#ifdef _WIN32
    std::uint32_t systemError = 0u;
    const auto recoveryRoot = recovery::defaultCrashJournalDirectory(&systemError);
    if (!recoveryRoot) {
        if (error != nullptr) {
            *error = "canonical production recovery root is unavailable";
            if (systemError != 0u) {
                *error += " (system error " + std::to_string(systemError) + ")";
            }
        }
        return {};
    }
    const auto watchdog = currentExecutableSibling(L"hydra_watchdog.exe");
    if (watchdog.empty()) {
        if (error != nullptr) {
            *error = "sibling hydra_watchdog.exe path could not be resolved";
        }
        return {};
    }

    ProductionActivationBridgeConfig config;
    config.recoveryRoot = *recoveryRoot;
    config.watchdogExecutablePath = watchdog;
    // Physical P3-HW evidence is loaded only through the typed per-user selection
    // source and is revalidated on every Host composition. Exact Game Gate-C
    // profiles remain release-owned; while P3-E-02 has none, Input still rejects
    // every keyboard/mouse activation rather than inferring authority from catalog.
    auto inputAuthority = loadDefaultProductionInputAuthoritySnapshot();
    config.gateCProfiles = std::move(inputAuthority.gateCProfiles);
    config.inputEvidenceClass = inputAuthority.inputEvidenceClass;
    config.physicalAcceptanceEvidence =
        std::move(inputAuthority.physicalAcceptanceEvidence);
    auto result = makeProductionActivationResourceBridges(std::move(config));
    if (error != nullptr) error->clear();
    return result;
#else
    if (error != nullptr) {
        *error = "default production activation bridges are Windows-only";
    }
    return {};
#endif
}

std::shared_ptr<IProductionRecoveryLeaseRuntime>
makeNativeProductionRecoveryLeaseRuntime(
    std::filesystem::path recoveryRoot,
    std::filesystem::path watchdogExecutablePath,
    std::uint32_t watchdogHandshakeTimeoutMilliseconds) {
    return std::make_shared<NativeProductionRecoveryLeaseRuntime>(
        std::move(recoveryRoot), std::move(watchdogExecutablePath),
        watchdogHandshakeTimeoutMilliseconds);
}

std::shared_ptr<IProductionGateCSessionRuntime>
makeNativeProductionGateCSessionRuntime(
    std::uint32_t handshakeTimeoutMilliseconds,
    std::uint32_t ioTimeoutMilliseconds) {
    return std::make_shared<NativeProductionGateCSessionRuntime>(
        handshakeTimeoutMilliseconds, ioTimeoutMilliseconds);
}

std::string_view productionInputEvidenceClassName(
    ProductionInputEvidenceClass value) noexcept {
    switch (value) {
        case ProductionInputEvidenceClass::None: return "none";
        case ProductionInputEvidenceClass::Controlled: return "controlled";
        case ProductionInputEvidenceClass::Synthetic: return "synthetic";
        case ProductionInputEvidenceClass::Physical: return "physical";
    }
    return "unknown";
}

} // namespace hydra::production

#endif
