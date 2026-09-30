#include "hydra/gate_c_adapter.h"
#include "hydra/gate_c_architecture.hpp"
#include "hydra/gate_c_protocol.hpp"
#include "hydra/gate_c_transport.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
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
    void reset() noexcept {
        hydra_gate_c_adapter_destroy(m_handle);
        m_handle = nullptr;
    }

private:
    HydraGateCAdapterHandle m_handle{nullptr};
};

HydraGateCAdapterInputEventV1 toAdapterEvent(
    const InputEventMessage& message) noexcept {
    HydraGateCAdapterInputEventV1 result{};
    result.struct_size = sizeof(result);
    const auto adapterKind = message.kind == hydra::gatec::InputKind::Keyboard
                                 ? HYDRA_GATE_C_ADAPTER_INPUT_KEYBOARD
                                 : HYDRA_GATE_C_ADAPTER_INPUT_MOUSE;
    result.kind = static_cast<std::uint8_t>(adapterKind);
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

HydraGateCAdapterControlStateV1 toAdapterControl(
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

bool adapterSnapshotKeyDown(const HydraGateCAdapterSnapshotV1& snapshot,
                            std::uint32_t vkey) noexcept {
    if (vkey >= 256u) return false;
    const auto byteIndex = static_cast<std::size_t>(vkey / 8u);
    const auto mask = static_cast<std::uint8_t>(1u << (vkey % 8u));
    return (snapshot.key_down_bits[byteIndex] & mask) != 0;
}

[[maybe_unused]] std::string adapterResultMessage(HydraGateCAdapterResult result) {
    return "adapter result " + std::to_string(static_cast<int>(result));
}

struct TargetOptions {
    std::wstring pipeName;
    std::uint32_t seatId{0};
    SessionToken token{};
    bool tokenSet{false};
    bool headless{false};
    bool selfTest{false};
    bool testAdapterFailureAfterHandshake{false};
    bool showHelp{false};
};

void printUsage(std::ostream& output) {
    output
        << "HydraSeat Gate C controlled target\n\n"
        << "Usage:\n"
        << "  hydra_gate_c_target --pipe <name> --seat <id> --token <32-hex> [--headless]\n"
        << "  hydra_gate_c_target --self-test\n\n"
        << "This executable is a HydraSeat-owned test process. It does not hook\n"
        << "Windows APIs or attach to a commercial game.\n";
}

int runLocalSelfTest() {
    AdapterOwner adapter;
    if (!adapter || hydra_gate_c_adapter_api_version() !=
                        HYDRA_GATE_C_ADAPTER_API_VERSION) {
        return 9;
    }

    ControlStateMessage control;
    control.cursorX = 10;
    control.cursorY = 20;
    control.virtualForeground = true;
    control.clipEnabled = true;
    control.clipLeft = 0;
    control.clipTop = 0;
    control.clipRight = 100;
    control.clipBottom = 100;
    const auto adapterControl = toAdapterControl(control);
    if (hydra_gate_c_adapter_apply_control(
            adapter.get(), 1, &adapterControl) != HYDRA_GATE_C_ADAPTER_OK) {
        return 10;
    }

    InputEventMessage key;
    key.kind = hydra::gatec::InputKind::Keyboard;
    key.keyTransition = hydra::gatec::KeyTransition::Down;
    key.vkey = 0x41;
    const auto adapterKey = toAdapterEvent(key);
    if (hydra_gate_c_adapter_apply_input(
            adapter.get(), 2, &adapterKey) != HYDRA_GATE_C_ADAPTER_OK) {
        return 11;
    }

    HydraGateCAdapterSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    if (hydra_gate_c_adapter_get_snapshot(
            adapter.get(), 0x41, &snapshot) != HYDRA_GATE_C_ADAPTER_OK ||
        !adapterSnapshotKeyDown(snapshot, 0x41) ||
        snapshot.async_key_state_value != 0x8001u) {
        return 12;
    }

    std::cout << "HydraSeat Gate C target/adapter self-test passed.\n";
    return EXIT_SUCCESS;
}

#ifdef _WIN32

constexpr wchar_t kWindowClass[] = L"HydraSeatGateCControlledTarget";
constexpr UINT kStateChangedMessage = WM_APP + 0x43;
constexpr std::uint32_t kIoTimeoutMs = 5000;
constexpr std::uint32_t kReadPollMs = 250;

std::wstring asciiToWide(std::string_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (const auto character : text) {
        result.push_back(static_cast<wchar_t>(
            static_cast<unsigned char>(character)));
    }
    return result;
}

bool parseUnsigned(std::wstring_view text, std::uint32_t& value) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const auto character : text) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        parsed = parsed * 10 + static_cast<std::uint64_t>(character - L'0');
        if (parsed > ~std::uint32_t{0}) {
            return false;
        }
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

std::string wideAsciiToString(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const auto character : text) {
        const auto code = static_cast<std::uint32_t>(character);
        if (code > 0x7fu) {
            return {};
        }
        result.push_back(static_cast<char>(code));
    }
    return result;
}

TargetOptions parseOptions(int argc, wchar_t** argv, bool& valid) {
    TargetOptions options;
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
        } else if (argument == L"--self-test") {
            options.selfTest = true;
        } else if (argument == L"--test-adapter-failure-after-handshake") {
            options.testAdapterFailureAfterHandshake = true;
        } else if (argument == L"--help" || argument == L"-h") {
            options.showHelp = true;
        } else {
            valid = false;
        }
    }

    if (!options.selfTest && !options.showHelp &&
        (options.pipeName.empty() || options.seatId == 0 ||
         !options.tokenSet)) {
        valid = false;
    }
    return options;
}

class ControlledTarget {
public:
    explicit ControlledTarget(TargetOptions options)
        : m_options(std::move(options)) {}

    int run(HINSTANCE instance, int showCommand) {
        m_instance = instance;
        if (!m_adapter || hydra_gate_c_adapter_api_version() !=
                              HYDRA_GATE_C_ADAPTER_API_VERSION) {
            return 19;
        }
        if (!m_options.headless && !createWindow(showCommand)) {
            return 20;
        }
        if (!connectAndHandshake()) {
            if (const HWND window = m_hwnd.load(); window != nullptr) {
                DestroyWindow(window);
                m_hwnd.store(nullptr);
            }
            return 21;
        }
        if (m_options.testAdapterFailureAfterHandshake) {
            m_adapter.reset();
            HydraGateCAdapterSnapshotV1 snapshot{};
            snapshot.struct_size = sizeof(snapshot);
            const auto result = hydra_gate_c_adapter_get_snapshot(
                m_adapter.get(), 0x41u, &snapshot);
            if (const HWND window = m_hwnd.load(); window != nullptr) {
                DestroyWindow(window);
                m_hwnd.store(nullptr);
            }
            m_channel.close();
            return result == HYDRA_GATE_C_ADAPTER_INVALID_ARGUMENT ? 82 : 83;
        }

        if (m_options.headless) {
            readerLoop(std::stop_token{});
            return m_exitCode.load();
        }

        m_reader = std::jthread(
            [this](std::stop_token stopToken) { readerLoop(stopToken); });

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        m_stopping.store(true);
        m_reader.request_stop();
        m_reader.join();
        m_channel.close();
        return m_exitCode.load();
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message,
                                       WPARAM wParam, LPARAM lParam) {
        ControlledTarget* self = reinterpret_cast<ControlledTarget*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<ControlledTarget*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
            if (self != nullptr) {
                self->m_hwnd.store(hwnd);
            }
        }
        if (self != nullptr) {
            return self->windowMessage(hwnd, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT windowMessage(HWND hwnd, UINT message, WPARAM wParam,
                          LPARAM lParam) {
        (void)wParam;
        (void)lParam;
        switch (message) {
        case kStateChangedMessage:
            InvalidateRect(hwnd, nullptr, FALSE);
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
        windowClass.lpfnWndProc = ControlledTarget::WindowProc;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kWindowClass;
        if (RegisterClassExW(&windowClass) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        const std::wstring title =
            L"HydraSeat Gate C Target - Seat " +
            std::to_wstring(m_options.seatId);
        const HWND window = CreateWindowExW(
            WS_EX_APPWINDOW, kWindowClass, title.c_str(),
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            650, 520, nullptr, nullptr, m_instance, this);
        if (window == nullptr) {
            return false;
        }
        m_hwnd.store(window);
        ShowWindow(window, showCommand);
        UpdateWindow(window);
        return true;
    }

    bool connectAndHandshake() {
        std::string error;
        std::uint32_t systemError = 0;
        m_channel = hydra::gatec::connectGateCClient(
            m_options.pipeName, 10000, &error, &systemError);
        if (!m_channel.valid()) {
            setProtocolError("connect failed: " + error, systemError);
            return false;
        }

        HelloMessage hello;
        hello.token = m_options.token;
        hello.seatId = m_options.seatId;
        hello.processId = GetCurrentProcessId();
        const auto architecture = hydra::gatec::detectProcessArchitecture(
            GetCurrentProcess());
        if (!architecture) {
            setProtocolError("process architecture detection failed: " +
                                 architecture.error,
                             architecture.systemError);
            return false;
        }
        hello.architectureBits =
            static_cast<std::uint16_t>(architecture.architecture);
        hello.targetWindow = reinterpret_cast<std::uint64_t>(m_hwnd.load());
        if (!m_channel.writeFrame(hydra::gatec::encodeHello(1, hello),
                                  kIoTimeoutMs, &error, &systemError)) {
            setProtocolError("Hello write failed: " + error, systemError);
            return false;
        }

        const auto response = m_channel.readFrame(kIoTimeoutMs);
        if (!response || !response.frame) {
            setProtocolError("HelloAck read failed: " + response.error,
                             response.systemError);
            return false;
        }
        HelloAckMessage ack;
        if (!hydra::gatec::decodeHelloAck(*response.frame, ack, &error) ||
            response.frame->sequence != 1 || !ack.accepted) {
            setProtocolError(
                error.empty() ? "HelloAck rejected the controlled target"
                              : "HelloAck decode failed: " + error,
                ack.errorCode);
            return false;
        }
        if ((ack.grantedCapabilities &
             hydra::gatec::testCapabilityBits(
                 hydra::gatec::kControlledTargetCapabilities)) !=
            hydra::gatec::testCapabilityBits(
                hydra::gatec::kControlledTargetCapabilities)) {
            setProtocolError("HelloAck omitted required controlled-test capabilities",
                             0);
            return false;
        }

        m_connected.store(true);
        return true;
    }

    void readerLoop(std::stop_token stopToken) {
        while (!stopToken.stop_requested() && !m_stopping.load()) {
            const auto result = m_channel.readFrame(kReadPollMs);
            if (result.status == TransportStatus::Timeout) {
                continue;
            }
            if (!result || !result.frame) {
                if (result.status != TransportStatus::Disconnected) {
                    setProtocolError(result.error, result.systemError);
                    m_exitCode.store(22);
                }
                break;
            }
            if (!processFrame(*result.frame)) {
                m_exitCode.store(23);
                break;
            }
        }

        m_stopping.store(true);
        if (const HWND window = m_hwnd.load(); window != nullptr) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    }

    bool processFrame(const DecodedFrame& frame) {
        std::string error;
        if (frame.type == MessageType::InputEvent) {
            InputEventMessage message;
            if (!hydra::gatec::decodeInputEvent(frame, message, &error)) {
                return sendError(frame.sequence, 1001, error);
            }
            const auto adapterEvent = toAdapterEvent(message);
            const auto result = hydra_gate_c_adapter_apply_input(
                m_adapter.get(), frame.sequence, &adapterEvent);
            if (result != HYDRA_GATE_C_ADAPTER_OK) {
                return sendError(frame.sequence,
                                 1100u + static_cast<std::uint32_t>(result),
                                 adapterResultMessage(result));
            }

            // Do not acknowledge host routing intent merely because apply_input
            // returned success. Read the adapter state back in the receiving
            // process and return the existing StateSnapshot contract as the
            // exact-sequence receipt. The authenticated channel is already bound
            // to this target's validated Seat/PID by Hello.
            HydraGateCAdapterSnapshotV1 adapterSnapshot{};
            adapterSnapshot.struct_size = sizeof(adapterSnapshot);
            const auto snapshotResult = hydra_gate_c_adapter_get_snapshot(
                m_adapter.get(), HYDRA_GATE_C_ADAPTER_NO_PROBE_KEY,
                &adapterSnapshot);
            if (snapshotResult != HYDRA_GATE_C_ADAPTER_OK) {
                return sendError(frame.sequence,
                                 1150u + static_cast<std::uint32_t>(snapshotResult),
                                 adapterResultMessage(snapshotResult));
            }
            if (adapterSnapshot.last_applied_sequence != frame.sequence) {
                return sendError(frame.sequence, 1199u,
                                 "target adapter snapshot did not observe the exact applied input sequence");
            }

            const auto snapshot = fromAdapterSnapshot(adapterSnapshot);
            std::uint32_t systemError = 0;
            if (!m_channel.writeFrame(
                    hydra::gatec::encodeStateSnapshot(frame.sequence, snapshot),
                    kIoTimeoutMs, &error, &systemError)) {
                setProtocolError("input verification snapshot write failed: " + error,
                                 systemError);
                return false;
            }
            notifyStateChanged();
            return true;
        }
        if (frame.type == MessageType::ControlState) {
            ControlStateMessage message;
            if (!hydra::gatec::decodeControlState(frame, message, &error)) {
                return sendError(frame.sequence, 1003, error);
            }
            const auto adapterControl = toAdapterControl(message);
            const auto result = hydra_gate_c_adapter_apply_control(
                m_adapter.get(), frame.sequence, &adapterControl);
            if (result != HYDRA_GATE_C_ADAPTER_OK) {
                return sendError(frame.sequence,
                                 1200u + static_cast<std::uint32_t>(result),
                                 adapterResultMessage(result));
            }
            notifyStateChanged();
            return true;
        }
        if (frame.type == MessageType::QuerySnapshot) {
            QuerySnapshotMessage query;
            if (!hydra::gatec::decodeQuerySnapshot(frame, query, &error)) {
                return sendError(frame.sequence, 1005, error);
            }

            HydraGateCAdapterSnapshotV1 adapterSnapshot{};
            adapterSnapshot.struct_size = sizeof(adapterSnapshot);
            const auto result = hydra_gate_c_adapter_get_snapshot(
                m_adapter.get(), query.probeVkey, &adapterSnapshot);
            if (result != HYDRA_GATE_C_ADAPTER_OK) {
                return sendError(frame.sequence,
                                 1300u + static_cast<std::uint32_t>(result),
                                 adapterResultMessage(result));
            }
            const auto snapshot = fromAdapterSnapshot(adapterSnapshot);
            return m_channel.writeFrame(
                hydra::gatec::encodeStateSnapshot(frame.sequence, snapshot),
                kIoTimeoutMs, &error, nullptr);
        }
        if (frame.type == MessageType::Shutdown) {
            if (!hydra::gatec::decodeShutdown(frame, &error)) {
                return sendError(frame.sequence, 1006, error);
            }
            m_stopping.store(true);
            return true;
        }

        return sendError(frame.sequence, 1099,
                         "unexpected message type: " +
                             std::string(hydra::gatec::messageTypeName(frame.type)));
    }

    bool sendError(std::uint64_t sequence, std::uint32_t code,
                   std::string_view message) {
        setProtocolError(std::string(message), code);
        std::string writeError;
        (void)m_channel.writeFrame(
            hydra::gatec::encodeError(sequence, ErrorMessage{code}),
            kIoTimeoutMs, &writeError, nullptr);
        // Any malformed, stale, or unexpected frame terminates the controlled
        // session after a best-effort Error response. Continuing would let the
        // host and target disagree about process-local state.
        return false;
    }

    void notifyStateChanged() {
        if (const HWND window = m_hwnd.load(); window != nullptr) {
            PostMessageW(window, kStateChangedMessage, 0, 0);
        }
    }

    void setProtocolError(std::string message, std::uint32_t systemError) {
        std::scoped_lock lock(m_errorMutex);
        m_lastError = std::move(message);
        m_lastSystemError = systemError;
        notifyStateChanged();
    }

    void paint(HWND hwnd) {
        HydraGateCAdapterSnapshotV1 adapterSnapshot{};
        adapterSnapshot.struct_size = sizeof(adapterSnapshot);
        const auto adapterResult = hydra_gate_c_adapter_get_snapshot(
            m_adapter.get(), HYDRA_GATE_C_ADAPTER_NO_PROBE_KEY,
            &adapterSnapshot);
        const StateSnapshotMessage snapshot =
            adapterResult == HYDRA_GATE_C_ADAPTER_OK
                ? fromAdapterSnapshot(adapterSnapshot)
                : StateSnapshotMessage{};
        std::string lastError;
        std::uint32_t systemError = 0;
        {
            std::scoped_lock lock(m_errorMutex);
            lastError = m_lastError;
            systemError = m_lastSystemError;
        }
        if (adapterResult != HYDRA_GATE_C_ADAPTER_OK) {
            if (!lastError.empty()) lastError += "; ";
            lastError += "adapter snapshot failed: " +
                         adapterResultMessage(adapterResult);
        }

        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH background = CreateSolidBrush(RGB(15, 23, 42));
        FillRect(dc, &client, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);

        HFONT titleFont = CreateFontW(
            28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT bodyFont = CreateFontW(
            18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        const auto oldFont = SelectObject(dc, titleFont);

        RECT textRect = client;
        textRect.left += 24;
        textRect.top += 20;
        textRect.right -= 24;
        SetTextColor(dc, RGB(226, 232, 240));
        const std::wstring title =
            L"Controlled Target Process — Seat " +
            std::to_wstring(m_options.seatId);
        DrawTextW(dc, title.c_str(), -1, &textRect,
                  DT_LEFT | DT_TOP | DT_SINGLELINE);
        textRect.top += 46;

        SelectObject(dc, bodyFont);
        SetTextColor(dc, RGB(248, 113, 113));
        const wchar_t* warning =
            L"TEST ADAPTER STATE ONLY — NO WINDOWS API HOOK IS INSTALLED";
        DrawTextW(dc, warning, -1, &textRect,
                  DT_LEFT | DT_TOP | DT_SINGLELINE);
        textRect.top += 40;

        const bool actualForeground = GetForegroundWindow() == hwnd;
        std::wostringstream status;
        status << L"Connected: " << (m_connected.load() ? L"yes" : L"no")
               << L"\nProcess ID: " << GetCurrentProcessId()
               << L"\nLast applied sequence: " << snapshot.lastAppliedSequence
               << L"\nVirtual foreground: "
               << (snapshot.virtualForeground ? L"true" : L"false")
               << L"\nActual OS foreground: "
               << (actualForeground ? L"true" : L"false")
               << L"\nVirtual capture: "
               << (snapshot.virtualCapture ? L"true" : L"false")
               << L"\nVirtual cursor: (" << snapshot.cursorX << L", "
               << snapshot.cursorY << L")"
               << L"\nVirtual clip: "
               << (snapshot.clipEnabled ? L"enabled" : L"disabled")
               << L" [" << snapshot.clipLeft << L", " << snapshot.clipTop
               << L", " << snapshot.clipRight << L", "
               << snapshot.clipBottom << L"]"
               << L"\nMouse buttons: 0x" << std::hex
               << snapshot.mouseButtonsDown << std::dec
               << L"\nWheel accumulator: " << snapshot.wheelAccumulator
               << L"\nKey A down: "
               << (adapterSnapshotKeyDown(adapterSnapshot, 0x41)
                       ? L"true" : L"false")
               << L"\nKey B down: "
               << (adapterSnapshotKeyDown(adapterSnapshot, 0x42)
                       ? L"true" : L"false");
        if (!lastError.empty()) {
            status << L"\n\nProtocol error: " << asciiToWide(lastError)
                   << L" (" << systemError << L")";
        }
        const auto statusText = status.str();
        SetTextColor(dc, RGB(148, 163, 184));
        DrawTextW(dc, statusText.c_str(), -1, &textRect,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

        SelectObject(dc, oldFont);
        DeleteObject(titleFont);
        DeleteObject(bodyFont);
        EndPaint(hwnd, &paint);
    }

    TargetOptions m_options;
    HINSTANCE m_instance{nullptr};
    std::atomic<HWND> m_hwnd{nullptr};
    PipeChannel m_channel;
    AdapterOwner m_adapter;
    std::mutex m_errorMutex;
    std::string m_lastError;
    std::uint32_t m_lastSystemError{0};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<int> m_exitCode{0};
    std::jthread m_reader;
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
    if (options.selfTest) {
        return runLocalSelfTest();
    }

    ControlledTarget target(options);
    return target.run(GetModuleHandleW(nullptr), SW_SHOWNORMAL);
}
#else
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
        return runLocalSelfTest();
    }
    printUsage(std::cerr);
    return 3;
}
#endif
