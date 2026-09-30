#include "hydra/raw_input_probe_trace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include "hydra/hardware_identity.hpp"
#include "hydra/raw_input_utils.hpp"

#include <hidusage.h>
#include <windows.h>
#endif

namespace {

using namespace hydra::rawprobe;

enum class Mode {
    SelfTest,
    RegistrationSelfTest,
    Observe,
    ProcessTeardownSelfTest,
    RegistrationChildNoCleanup
};

struct Options {
    Mode mode{Mode::SelfTest};
    std::filesystem::path outputPath;
    std::uint32_t durationSeconds{10};
};

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  hydra_gate_c_raw_input_probe --self-test\n"
        << "  hydra_gate_c_raw_input_probe --registration-self-test [--output <trace.json>]\n"
        << "  hydra_gate_c_raw_input_probe --process-teardown-self-test\n"
        << "  hydra_gate_c_raw_input_probe --observe --output <trace.json> [--duration <1-30>]\n";
}

std::optional<std::uint32_t> parsePositiveU32(std::string_view text) {
    std::uint64_t value = 0;
    if (text.empty()) return std::nullopt;
    for (const char character : text) {
        if (character < '0' || character > '9') return std::nullopt;
        const auto digit = static_cast<unsigned>(character - '0');
        if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10u) {
            return std::nullopt;
        }
        value = value * 10u + digit;
        if (value > (std::numeric_limits<std::uint32_t>::max)()) return std::nullopt;
    }
    if (value == 0) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

std::optional<Options> parseOptions(int argc, char** argv) {
    Options options;
    bool modeSeen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto selectMode = [&](Mode mode) {
            if (modeSeen) return false;
            options.mode = mode;
            modeSeen = true;
            return true;
        };
        if (argument == "--self-test") {
            if (!selectMode(Mode::SelfTest)) return std::nullopt;
        } else if (argument == "--registration-self-test") {
            if (!selectMode(Mode::RegistrationSelfTest)) return std::nullopt;
        } else if (argument == "--observe") {
            if (!selectMode(Mode::Observe)) return std::nullopt;
        } else if (argument == "--process-teardown-self-test") {
            if (!selectMode(Mode::ProcessTeardownSelfTest)) return std::nullopt;
        } else if (argument == "--registration-child-no-cleanup") {
            if (!selectMode(Mode::RegistrationChildNoCleanup)) return std::nullopt;
        } else if (argument == "--output") {
            if (++index >= argc || options.outputPath.empty() == false) return std::nullopt;
            options.outputPath = argv[index];
        } else if (argument == "--duration") {
            if (++index >= argc) return std::nullopt;
            const auto parsed = parsePositiveU32(argv[index]);
            if (!parsed || *parsed > kMaxObserveSeconds) return std::nullopt;
            options.durationSeconds = *parsed;
        } else {
            return std::nullopt;
        }
    }
    if (!modeSeen) return std::nullopt;
    if (options.mode == Mode::Observe && options.outputPath.empty()) return std::nullopt;
    return options;
}

[[maybe_unused]] bool writeTrace(const std::filesystem::path& path,
                                 const RawInputProbeTrace& trace) {
    if (path.empty()) return true;
    const std::string serialized = serializeRawInputProbeTrace(trace);
    if (serialized.empty()) return false;
    if (!parseRawInputProbeTrace(serialized)) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    output.put('\n');
    output.flush();
    return static_cast<bool>(output);
}

bool runPortableSelfTest() {
    RawInputProbeTrace trace;
    trace.platform = "windows";
    trace.sourceKind = RawProbeSourceKind::SyntheticParserFixture;
    trace.architectureBits = sizeof(void*) == 8 ? 64 : 32;
    trace.rawInputHeaderBytes = sizeof(void*) == 8 ? 24 : 16;
    trace.rawInputBytes = sizeof(void*) == 8 ? 48 : 40;
    trace.rawInputBufferAlignmentBytes = sizeof(void*) == 8 ? 8 : 4;
    trace.observations.push_back({"self_test", RawProbeResultKind::Success,
                                  "No Windows API or physical-input claim."});
    const std::string serialized = serializeRawInputProbeTrace(trace);
    const auto parsed = parseRawInputProbeTrace(serialized);
    return !serialized.empty() && parsed &&
           parsed.trace->sourceKind == RawProbeSourceKind::SyntheticParserFixture;
}

#ifdef _WIN32

constexpr wchar_t kWindowClassName[] = L"HydraSeatRawInputBehaviorProbeV1";
constexpr std::uint32_t kApiError = (std::numeric_limits<std::uint32_t>::max)();

std::uint64_t runtimeValue(const void* value) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
}

std::uint64_t monotonicTimestampMicros() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > 32768) return {};
    const int characters = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                              value.data(), characters,
                                              nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            characters, result.data(), required,
                            nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

class RawInputProbe final {
public:
    RawInputProbe() {
        trace_.sourceKind = RawProbeSourceKind::ObservedWindowsApi;
        trace_.architectureBits = static_cast<std::uint16_t>(sizeof(void*) * 8u);
        trace_.processId = GetCurrentProcessId();
        trace_.threadId = GetCurrentThreadId();
        trace_.rawInputHeaderBytes = sizeof(RAWINPUTHEADER);
        trace_.rawInputBytes = sizeof(RAWINPUT);
        BOOL wow64 = FALSE;
        const bool wow64Process = sizeof(void*) == 4 &&
            IsWow64Process(GetCurrentProcess(), &wow64) != FALSE && wow64 != FALSE;
        trace_.rawInputBufferAlignmentBytes = wow64Process
            ? 8u : static_cast<std::uint32_t>(sizeof(void*));
        trace_.registrationEvents.reserve(kMaxTraceEvents);
        trace_.messageEvents.reserve(kMaxTraceEvents);
        trace_.dataEvents.reserve(kMaxTraceEvents);
        trace_.bufferEvents.reserve(kMaxTraceEvents);
        trace_.observations.reserve(kMaxTraceEvents);
        trace_.observations.push_back({
            "runtime_handle_policy", RawProbeResultKind::Success,
            "HWND/HANDLE/HRAWINPUT/hDevice values are runtime-only diagnostics, not stable identity."});
        trace_.observations.push_back({
            "dangerous_flags", RawProbeResultKind::NotObserved,
            "RIDEV_NOLEGACY/RIDEV_CAPTUREMOUSE/RIDEV_APPKEYS: NotTestedInP3Raw01"});
    }

    ~RawInputProbe() {
        cleanup();
        destroyWindows();
    }

    RawInputProbe(const RawInputProbe&) = delete;
    RawInputProbe& operator=(const RawInputProbe&) = delete;

    bool createWindows(bool showObservationWindow) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &RawInputProbe::windowProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kWindowClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (RegisterClassExW(&windowClass) == 0) {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                addObservation("register_window_class", RawProbeResultKind::ApiFailure,
                               error);
                return false;
            }
        }

        windowA_ = createWindow(L"HydraSeat Raw Input Probe - Window A", 80, 80);
        windowB_ = createWindow(L"HydraSeat Raw Input Probe - Window B", 440, 80);
        if (windowA_ == nullptr || windowB_ == nullptr) {
            addObservation("create_controlled_windows", RawProbeResultKind::ApiFailure,
                           GetLastError());
            destroyWindows();
            return false;
        }
        if (showObservationWindow) {
            ShowWindow(windowA_, SW_SHOWNORMAL);
            UpdateWindow(windowA_);
            ShowWindow(windowB_, SW_SHOWNORMAL);
            UpdateWindow(windowB_);
        }
        return true;
    }

    bool runRegistrationLifecycle() {
        if (!createWindows(false)) return false;
        bool ok = true;
        ok = performRegistration(RawProbeOperation::RegisterKeyboard,
                                 HID_USAGE_GENERIC_KEYBOARD, 0, windowA_) && ok;
        ok = requireStored(HID_USAGE_GENERIC_KEYBOARD, 0, windowA_) && ok;
        ok = performRegistration(RawProbeOperation::RegisterMouse,
                                 HID_USAGE_GENERIC_MOUSE, 0, windowA_) && ok;
        ok = requireStored(HID_USAGE_GENERIC_MOUSE, 0, windowA_) && ok;

        ok = performRegistration(RawProbeOperation::ReplaceKeyboardTarget,
                                 HID_USAGE_GENERIC_KEYBOARD, 0, windowB_) && ok;
        ok = requireStored(HID_USAGE_GENERIC_KEYBOARD, 0, windowB_) && ok;
        ok = performRegistration(RawProbeOperation::ReplaceMouseTarget,
                                 HID_USAGE_GENERIC_MOUSE, 0, windowB_) && ok;
        ok = requireStored(HID_USAGE_GENERIC_MOUSE, 0, windowB_) && ok;

        // Establish the independent keyboard-A / mouse-B state, then mutate
        // and remove only the keyboard registration.
        ok = performRegistration(RawProbeOperation::ReplaceKeyboardTarget,
                                 HID_USAGE_GENERIC_KEYBOARD, 0, windowA_) && ok;
        ok = requireStored(HID_USAGE_GENERIC_KEYBOARD, 0, windowA_) && ok;
        ok = requireStored(HID_USAGE_GENERIC_MOUSE, 0, windowB_) && ok;
        ok = performRegistration(RawProbeOperation::ReplaceKeyboardTarget,
                                 HID_USAGE_GENERIC_KEYBOARD, 0, windowB_) && ok;
        ok = requireStored(HID_USAGE_GENERIC_MOUSE, 0, windowB_) && ok;
        ok = performRegistration(RawProbeOperation::RemoveKeyboard,
                                 HID_USAGE_GENERIC_KEYBOARD, RIDEV_REMOVE,
                                 nullptr) && ok;
        ok = requireAbsent(HID_USAGE_GENERIC_KEYBOARD) && ok;
        ok = requireStored(HID_USAGE_GENERIC_MOUSE, 0, windowB_) && ok;

        ok = performRegistration(RawProbeOperation::RegisterInputSink,
                                 HID_USAGE_GENERIC_KEYBOARD, RIDEV_INPUTSINK,
                                 windowA_) && ok;
        ok = requireStored(HID_USAGE_GENERIC_KEYBOARD, RIDEV_INPUTSINK,
                           windowA_) && ok;
        ok = performRegistration(RawProbeOperation::RegisterDeviceNotify,
                                 HID_USAGE_GENERIC_MOUSE, RIDEV_DEVNOTIFY,
                                 windowA_) && ok;
        // Native Windows x64 and WOW64/x86 observation shows that
        // RegisterRawInputDevices accepts RIDEV_DEVNOTIFY, while
        // GetRegisteredRawInputDevices does not echo that notification bit.
        // The request and stored snapshots remain separately recorded so
        // P3-RAW-02 can preserve the distinction without guessing.
        ok = requireStored(HID_USAGE_GENERIC_MOUSE, 0, windowA_) && ok;
        ok = performRegistration(
                 RawProbeOperation::RegisterBackgroundDeviceNotify,
                 HID_USAGE_GENERIC_KEYBOARD,
                 RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, windowB_) && ok;
        ok = requireStored(HID_USAGE_GENERIC_KEYBOARD, RIDEV_INPUTSINK,
                           windowB_) && ok;

        ok = performRegistration(RawProbeOperation::RemoveMouse,
                                 HID_USAGE_GENERIC_MOUSE, RIDEV_REMOVE,
                                 nullptr) && ok;
        ok = requireAbsent(HID_USAGE_GENERIC_MOUSE) && ok;
        ok = performRegistration(RawProbeOperation::RemoveKeyboard,
                                 HID_USAGE_GENERIC_KEYBOARD, RIDEV_REMOVE,
                                 nullptr) && ok;
        ok = requireAbsent(HID_USAGE_GENERIC_KEYBOARD) && ok;

        ok = performRegistration(RawProbeOperation::RegisterKeyboard,
                                 HID_USAGE_GENERIC_KEYBOARD, 0, windowB_) && ok;
        RawRegistrationEvent destroyed;
        destroyed.context = nextContext();
        destroyed.operation = RawProbeOperation::DestroyTargetWindow;
        destroyed.request = descriptor(HID_USAGE_GENERIC_KEYBOARD, 0, windowB_);
        destroyed.before = snapshotRegistrations();
        const auto destroyCallContext = nextContext();
        SetLastError(ERROR_SUCCESS);
        const BOOL destroyedResult = DestroyWindow(windowB_);
        const DWORD destroyError = GetLastError();
        destroyed.call = boolApiResult(destroyCallContext, destroyedResult,
                                       destroyError);
        windowB_ = nullptr;
        destroyed.after = snapshotRegistrations();
        appendRegistrationEvent(std::move(destroyed));
        ok = destroyedResult != FALSE && ok;

        // The exact post-destruction snapshot is evidence, not an assumed
        // assertion. A fresh valid registration must still replace it.
        ok = performRegistration(RawProbeOperation::ReplaceDestroyedTarget,
                                 HID_USAGE_GENERIC_KEYBOARD, 0, windowA_) && ok;
        ok = requireStored(HID_USAGE_GENERIC_KEYBOARD, 0, windowA_) && ok;
        ok = performRegistration(RawProbeOperation::RemoveKeyboard,
                                 HID_USAGE_GENERIC_KEYBOARD, RIDEV_REMOVE,
                                 nullptr) && ok;
        ok = requireAbsent(HID_USAGE_GENERIC_KEYBOARD) && ok;

        const bool firstCleanup = cleanup();
        const bool secondCleanup = cleanup();
        if (!firstCleanup || !secondCleanup) {
            addObservation("cleanup_idempotence", RawProbeResultKind::ApiFailure,
                           ERROR_INVALID_STATE);
            ok = false;
        } else {
            trace_.observations.push_back({"cleanup_idempotence",
                                           RawProbeResultKind::Success,
                                           "Repeated local cleanup is a no-op."});
        }
        return ok && !trace_.traceOverflowed;
    }

    bool runObserve(std::uint32_t durationSeconds) {
        if (durationSeconds == 0 || durationSeconds > kMaxObserveSeconds ||
            !createWindows(true)) {
            return false;
        }
        bool ok = true;
        ok = performRegistration(RawProbeOperation::RegisterBackgroundDeviceNotify,
                                 HID_USAGE_GENERIC_KEYBOARD,
                                 RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
                                 windowA_) && ok;
        ok = performRegistration(RawProbeOperation::RegisterBackgroundDeviceNotify,
                                 HID_USAGE_GENERIC_MOUSE,
                                 RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
                                 windowA_) && ok;
        if (!ok) return false;

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(durationSeconds);
        auto nextBufferProbe = std::chrono::steady_clock::now();
        std::uint32_t bufferCalls = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    return false;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextBufferProbe && bufferCalls < 32) {
                observeRawInputBuffer();
                ++bufferCalls;
                nextBufferProbe = now + std::chrono::milliseconds(250);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        observeRawInputBuffer();
        return cleanup() && !trace_.traceOverflowed;
    }

    bool registerForTeardownChild() {
        if (!createWindows(false)) return false;
        const auto baseline = snapshotRegistrations();
        if (!baseline.api.success || findRegistration(baseline, HID_USAGE_GENERIC_KEYBOARD) ||
            findRegistration(baseline, HID_USAGE_GENERIC_MOUSE)) {
            return false;
        }
        return registerOne(HID_USAGE_GENERIC_KEYBOARD,
                           RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, windowA_).success &&
               registerOne(HID_USAGE_GENERIC_MOUSE,
                           RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, windowA_).success;
    }

    RawRegistrationSnapshot currentRegistrations() {
        return snapshotRegistrations();
    }

    RawInputProbeTrace finishTrace() {
        cleanup();
        resolveDeviceIdentities();
        return trace_;
    }

private:
    HWND createWindow(const wchar_t* title, int x, int y) {
        return CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClassName, title,
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               x, y, 320, 120, nullptr, nullptr,
                               GetModuleHandleW(nullptr), this);
    }

    void destroyWindows() noexcept {
        if (windowB_ != nullptr) {
            DestroyWindow(windowB_);
            windowB_ = nullptr;
        }
        if (windowA_ != nullptr) {
            DestroyWindow(windowA_);
            windowA_ = nullptr;
        }
    }

    static LRESULT CALLBACK windowProcedure(HWND window, UINT message,
                                             WPARAM wParam, LPARAM lParam) {
        RawInputProbe* probe = reinterpret_cast<RawInputProbe*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            probe = static_cast<RawInputProbe*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(probe));
        }
        if (probe != nullptr && message == WM_INPUT) {
            probe->observeRawInputMessage(window, wParam,
                                          reinterpret_cast<HRAWINPUT>(lParam));
            return DefWindowProcW(window, message, wParam, lParam);
        }
        if (probe != nullptr && message == WM_INPUT_DEVICE_CHANGE) {
            probe->observeDeviceChangeMessage(window, wParam,
                                              reinterpret_cast<HANDLE>(lParam));
            return 0;
        }
        if (message == WM_CLOSE) {
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    RawEventContext nextContext() noexcept {
        return {nextSequence_++, monotonicTimestampMicros(), GetCurrentThreadId()};
    }

    static RawRegistrationDescriptor descriptor(USHORT usage, DWORD flags,
                                                HWND window) noexcept {
        return {HID_USAGE_PAGE_GENERIC, usage, flags, runtimeValue(window)};
    }

    static RawApiResult boolApiResult(RawEventContext context, BOOL apiResult,
                                      DWORD error) noexcept {
        RawApiResult result;
        result.context = context;
        result.success = apiResult != FALSE;
        result.kind = result.success ? RawProbeResultKind::Success
                                     : RawProbeResultKind::ApiFailure;
        result.resultValue = result.success ? 1 : 0;
        result.systemError = error;
        return result;
    }

    RawApiResult registerOne(USHORT usage, DWORD flags, HWND window) {
        const auto request = descriptor(usage, flags, window);
        const RawRegistrationContractInput contract{
            request, sizeof(RAWINPUTDEVICE), sizeof(RAWINPUTDEVICE), true};
        if (validateRawRegistrationContract(contract) != RawProbeResultKind::Success) {
            RawApiResult invalid;
            invalid.kind = RawProbeResultKind::InvalidContract;
            return invalid;
        }
        RAWINPUTDEVICE native{};
        native.usUsagePage = request.usagePage;
        native.usUsage = request.usage;
        native.dwFlags = request.flags;
        native.hwndTarget = window;
        const auto context = nextContext();
        SetLastError(ERROR_SUCCESS);
        const BOOL apiResult = RegisterRawInputDevices(
            &native, 1, sizeof(RAWINPUTDEVICE));
        const DWORD error = GetLastError();
        auto result = boolApiResult(context, apiResult, error);
        if (result.success) {
            bool* owned = usage == HID_USAGE_GENERIC_KEYBOARD
                              ? &ownsKeyboardRegistration_
                              : &ownsMouseRegistration_;
            *owned = (flags & RIDEV_REMOVE) == 0;
        }
        return result;
    }

    RawRegistrationSnapshot snapshotRegistrations() {
        RawRegistrationSnapshot snapshot;
        UINT count = 0;
        snapshot.sizeQuery.context = nextContext();
        snapshot.api.context = snapshot.sizeQuery.context;
        SetLastError(ERROR_SUCCESS);
        const UINT queryResult = GetRegisteredRawInputDevices(
            nullptr, &count, sizeof(RAWINPUTDEVICE));
        const DWORD queryError = GetLastError();
        snapshot.reportedDeviceCount = count;
        snapshot.sizeQuery.resultValue = queryResult;
        snapshot.sizeQuery.systemError = queryError;
        snapshot.sizeQuery.pcbSizeBefore = 0;
        snapshot.sizeQuery.pcbSizeAfter = count;
        snapshot.sizeQuery.cbSizeHeader = sizeof(RAWINPUTDEVICE);
        snapshot.sizeQuery.reportedSize = count;
        if (queryResult == static_cast<UINT>(-1)) {
            snapshot.sizeQuery.kind = RawProbeResultKind::ApiFailure;
            snapshot.api.kind = RawProbeResultKind::ApiFailure;
            snapshot.api.systemError = queryError;
            return snapshot;
        }
        snapshot.sizeQuery.kind = RawProbeResultKind::Success;
        snapshot.sizeQuery.success = true;
        if (count > kMaxRawRegistrations) {
            snapshot.api.kind = RawProbeResultKind::BoundsExceeded;
            snapshot.api.reportedSize = count;
            return snapshot;
        }
        if (count == 0) {
            snapshot.api.kind = RawProbeResultKind::Success;
            snapshot.api.success = true;
            snapshot.api.resultValue = queryResult;
            return snapshot;
        }
        std::array<RAWINPUTDEVICE, kMaxRawRegistrations> registrations{};
        UINT capacity = count;
        snapshot.read.context = nextContext();
        snapshot.api.context = snapshot.read.context;
        SetLastError(ERROR_SUCCESS);
        const UINT returned = GetRegisteredRawInputDevices(
            registrations.data(), &capacity, sizeof(RAWINPUTDEVICE));
        const DWORD error = GetLastError();
        snapshot.read.resultValue = returned;
        snapshot.read.systemError = error;
        snapshot.read.pcbSizeBefore = count;
        snapshot.read.pcbSizeAfter = capacity;
        snapshot.read.cbSizeHeader = sizeof(RAWINPUTDEVICE);
        snapshot.read.reportedSize = count;
        snapshot.read.returnedSize = returned == static_cast<UINT>(-1) ? 0 : returned;
        if (returned == static_cast<UINT>(-1)) {
            snapshot.read.kind = capacity > count
                ? (capacity > kMaxRawRegistrations
                       ? RawProbeResultKind::BoundsExceeded
                       : RawProbeResultKind::SizeMismatch)
                : RawProbeResultKind::ApiFailure;
            snapshot.api.kind = snapshot.read.kind;
            snapshot.api.systemError = error;
            return snapshot;
        }
        if (returned > count || returned > kMaxRawRegistrations) {
            snapshot.read.kind = RawProbeResultKind::BoundsExceeded;
            snapshot.api.kind = RawProbeResultKind::BoundsExceeded;
            return snapshot;
        }
        snapshot.read.kind = RawProbeResultKind::Success;
        snapshot.read.success = true;
        snapshot.api.kind = RawProbeResultKind::Success;
        snapshot.api.success = true;
        snapshot.api.resultValue = returned;
        snapshot.api.reportedSize = count;
        snapshot.api.returnedSize = returned;
        for (UINT index = 0; index < returned; ++index) {
            const auto& value = registrations[index];
            snapshot.registrations.push_back({
                value.usUsagePage, value.usUsage, value.dwFlags,
                runtimeValue(value.hwndTarget)});
        }
        std::sort(snapshot.registrations.begin(), snapshot.registrations.end(),
                  [](const auto& left, const auto& right) {
                      if (left.usagePage != right.usagePage) return left.usagePage < right.usagePage;
                      if (left.usage != right.usage) return left.usage < right.usage;
                      if (left.flags != right.flags) return left.flags < right.flags;
                      return left.targetWindowRuntimeValue < right.targetWindowRuntimeValue;
                  });
        return snapshot;
    }

    bool performRegistration(RawProbeOperation operation, USHORT usage,
                             DWORD flags, HWND window) {
        RawRegistrationEvent event;
        event.context = nextContext();
        event.operation = operation;
        event.request = descriptor(usage, flags, window);
        event.before = snapshotRegistrations();
        event.call = registerOne(usage, flags, window);
        event.after = snapshotRegistrations();
        const bool success = event.before.api.success && event.call.success &&
                             event.after.api.success;
        appendRegistrationEvent(std::move(event));
        return success;
    }

    void appendRegistrationEvent(RawRegistrationEvent event) {
        if (totalEventCount() >= kMaxTraceEvents) {
            trace_.traceOverflowed = true;
            return;
        }
        trace_.registrationEvents.push_back(std::move(event));
    }

    const RawRegistrationDescriptor* findRegistration(
        const RawRegistrationSnapshot& snapshot, USHORT usage) const noexcept {
        for (const auto& registration : snapshot.registrations) {
            if (registration.usagePage == HID_USAGE_PAGE_GENERIC &&
                registration.usage == usage) {
                return &registration;
            }
        }
        return nullptr;
    }

    bool requireStored(USHORT usage, DWORD flags, HWND window) {
        if (trace_.registrationEvents.empty()) return false;
        const auto& snapshot = trace_.registrationEvents.back().after;
        const auto* stored = findRegistration(snapshot, usage);
        const bool ok = snapshot.api.success && stored != nullptr &&
            stored->flags == flags &&
            stored->targetWindowRuntimeValue == runtimeValue(window);
        if (!ok) {
            addObservation("registration_state_mismatch",
                           RawProbeResultKind::SizeMismatch, ERROR_INVALID_DATA);
        }
        return ok;
    }

    bool requireAbsent(USHORT usage) {
        if (trace_.registrationEvents.empty()) return false;
        const auto& snapshot = trace_.registrationEvents.back().after;
        const bool ok = snapshot.api.success && findRegistration(snapshot, usage) == nullptr;
        if (!ok) {
            addObservation("registration_removal_mismatch",
                           RawProbeResultKind::SizeMismatch, ERROR_INVALID_DATA);
        }
        return ok;
    }

    bool cleanup() noexcept {
        if (cleanupComplete_) return cleanupResult_;
        cleanupComplete_ = true;
        cleanupResult_ = true;
        const auto removeUsage = [&](USHORT usage, bool& owned) noexcept {
            if (!owned) return;
            try {
                const bool removed = performRegistration(
                    RawProbeOperation::Cleanup, usage, RIDEV_REMOVE, nullptr);
                bool absent = false;
                if (!trace_.registrationEvents.empty()) {
                    absent = findRegistration(
                        trace_.registrationEvents.back().after, usage) == nullptr;
                }
                if (!removed || !absent) {
                    cleanupResult_ = false;
                }
            } catch (...) {
                cleanupResult_ = false;
            }
        };
        removeUsage(HID_USAGE_GENERIC_KEYBOARD, ownsKeyboardRegistration_);
        removeUsage(HID_USAGE_GENERIC_MOUSE, ownsMouseRegistration_);
        return cleanupResult_;
    }

    RawDataQueryEvent queryRawInput(HRAWINPUT handle, UINT command) {
        RawDataQueryEvent event;
        event.context = nextContext();
        event.uiCommand = command;
        UINT required = 0;
        event.query.context = nextContext();
        SetLastError(ERROR_SUCCESS);
        const UINT query = GetRawInputData(handle, command, nullptr, &required,
                                           sizeof(RAWINPUTHEADER));
        const DWORD queryError = GetLastError();
        event.query.resultValue = query;
        event.query.systemError = queryError;
        event.query.pcbSizeAfter = required;
        event.query.cbSizeHeader = sizeof(RAWINPUTHEADER);
        event.query.reportedSize = required;
        event.query.success = query != static_cast<UINT>(-1);
        event.query.kind = event.query.success ? RawProbeResultKind::Success
                                               : RawProbeResultKind::ApiFailure;
        if (!event.query.success || required == 0) return event;
        const UINT maximum = command == RID_HEADER
            ? static_cast<UINT>(sizeof(RAWINPUTHEADER))
            : static_cast<UINT>(kMaxRawPacketBytes);
        if (required > maximum) {
            event.read.kind = RawProbeResultKind::Oversized;
            return event;
        }
        UINT available = required;
        event.read.context = nextContext();
        SetLastError(ERROR_SUCCESS);
        const UINT read = GetRawInputData(handle, command, scratch_.data(),
                                          &available, sizeof(RAWINPUTHEADER));
        const DWORD readError = GetLastError();
        event.read.resultValue = read;
        event.read.systemError = readError;
        event.read.pcbSizeBefore = required;
        event.read.pcbSizeAfter = available;
        event.read.cbSizeHeader = sizeof(RAWINPUTHEADER);
        event.read.returnedSize = read == static_cast<UINT>(-1) ? 0 : read;
        event.read.success = read != static_cast<UINT>(-1);
        event.read.kind = event.read.success ? RawProbeResultKind::Success
                                             : RawProbeResultKind::ApiFailure;
        if (!event.read.success || read < sizeof(RAWINPUTHEADER)) {
            if (event.read.success) event.read.kind = RawProbeResultKind::Truncated;
            return event;
        }
        RAWINPUTHEADER header{};
        std::memcpy(&header, scratch_.data(), sizeof(header));
        event.header.available = true;
        event.header.dwType = header.dwType;
        event.header.dwSize = header.dwSize;
        event.header.deviceRuntimeValue = runtimeValue(header.hDevice);
        event.header.wParamRuntimeValue = static_cast<std::uint64_t>(header.wParam);
        event.totalPayloadBytes = read;
        if (command == RID_INPUT) {
            RawDataContractInput contract;
            contract.headerBytes = sizeof(RAWINPUTHEADER);
            contract.queryReturnValue = query;
            contract.querySizeAfter = required;
            contract.readReturnValue = read;
            contract.readSizeAfter = available;
            contract.suppliedBufferBytes = required;
            contract.rawDwType = header.dwType;
            contract.rawDwSize = header.dwSize;
            event.read.kind = validateRawDataContract(contract);
            event.read.success = event.read.kind == RawProbeResultKind::Success;
        }
        return event;
    }

    void observeRawInputMessage(HWND window, WPARAM wParam, HRAWINPUT handle) {
        if (totalEventCount() >= kMaxTraceEvents) {
            trace_.traceOverflowed = true;
            return;
        }
        RawMessageEvent event;
        event.context = nextContext();
        event.messageKind = RawMessageKind::Input;
        event.messageId = WM_INPUT;
        event.messageTimeMilliseconds = static_cast<std::uint32_t>(GetMessageTime());
        event.windowRuntimeValue = runtimeValue(window);
        event.wParamRuntimeValue = static_cast<std::uint64_t>(wParam);
        event.lParamRuntimeValue = runtimeValue(handle);
        const UINT code = GET_RAWINPUT_CODE_WPARAM(wParam);
        event.inputCode = code == RIM_INPUT ? RawInputCodeKind::Foreground :
                          code == RIM_INPUTSINK ? RawInputCodeKind::Background :
                          RawInputCodeKind::Unknown;
        event.headerQuery = queryRawInput(handle, RID_HEADER);
        event.inputQueryAndRead = queryRawInput(handle, RID_INPUT);
        trace_.messageEvents.push_back(std::move(event));
    }

    void observeDeviceChangeMessage(HWND window, WPARAM wParam, HANDLE device) {
        if (totalEventCount() >= kMaxTraceEvents) {
            trace_.traceOverflowed = true;
            return;
        }
        RawMessageEvent event;
        event.context = nextContext();
        event.messageKind = RawMessageKind::DeviceChange;
        event.messageId = WM_INPUT_DEVICE_CHANGE;
        event.messageTimeMilliseconds = static_cast<std::uint32_t>(GetMessageTime());
        event.windowRuntimeValue = runtimeValue(window);
        event.wParamRuntimeValue = static_cast<std::uint64_t>(wParam);
        event.lParamRuntimeValue = runtimeValue(device);
        event.deviceChange = wParam == GIDC_ARRIVAL ? RawDeviceChangeKind::Arrival :
                             wParam == GIDC_REMOVAL ? RawDeviceChangeKind::Removal :
                             RawDeviceChangeKind::Unknown;
        trace_.deviceChangeObserved = true;
        trace_.messageEvents.push_back(std::move(event));
    }

    void observeRawInputBuffer() {
        if (totalEventCount() >= kMaxTraceEvents) {
            trace_.traceOverflowed = true;
            return;
        }
        RawBufferQueryEvent event;
        event.context = nextContext();
        event.requestedBufferBytes = static_cast<std::uint32_t>(scratch_.size());
        UINT bytes = static_cast<UINT>(scratch_.size());
        scratch_.fill(std::byte{0});
        event.call.context = nextContext();
        SetLastError(ERROR_SUCCESS);
        const UINT count = GetRawInputBuffer(
            reinterpret_cast<PRAWINPUT>(scratch_.data()), &bytes,
            sizeof(RAWINPUTHEADER));
        const DWORD error = GetLastError();
        event.call.resultValue = count;
        event.call.systemError = error;
        event.call.pcbSizeBefore = static_cast<std::uint32_t>(scratch_.size());
        event.call.pcbSizeAfter = bytes;
        event.call.cbSizeHeader = sizeof(RAWINPUTHEADER);
        event.call.reportedSize = bytes;
        event.call.returnedSize = count == static_cast<UINT>(-1) ? 0 : count;
        if (count == static_cast<UINT>(-1)) {
            event.call.kind = RawProbeResultKind::ApiFailure;
        } else {
            event.returnedRawInputCount = count;
            const auto parsed = parseRawInputBufferLayout(
                std::span<const std::byte>(scratch_), count,
                trace_.architectureBits, sizeof(RAWINPUTHEADER),
                trace_.rawInputBufferAlignmentBytes);
            event.call.kind = parsed.kind;
            event.call.success = static_cast<bool>(parsed);
            event.blocks = parsed.blocks;
        }
        trace_.bufferEvents.push_back(std::move(event));
    }

    void resolveDeviceIdentities() {
        const auto resolve = [this](RawHeaderObservation& header) {
            if (!header.available || header.deviceRuntimeValue == 0 ||
                !header.devicePath.empty()) return;
            const HANDLE device = reinterpret_cast<HANDLE>(
                static_cast<std::uintptr_t>(header.deviceRuntimeValue));
            const auto path = hydra::win32::rawInputDeviceName(device);
            if (!path) return;
            header.devicePath = utf8(*path);
            const std::wstring category = header.dwType == RIM_TYPEKEYBOARD
                ? L"Keyboard" : header.dwType == RIM_TYPEMOUSE
                ? L"Mouse" : L"RawInput";
            header.stableDeviceId = utf8(
                hydra::win32::makeStableRawInputDeviceId(category, *path));
            if ((header.dwType == RIM_TYPEKEYBOARD ||
                 header.dwType == RIM_TYPEMOUSE) &&
                !hydra::hardware::isObviousRemoteOrSyntheticInputPath(*path)) {
                trace_.physicalInputObserved = true;
            }
        };
        for (auto& message : trace_.messageEvents) {
            resolve(message.headerQuery.header);
            resolve(message.inputQueryAndRead.header);
        }
        for (auto& data : trace_.dataEvents) resolve(data.header);
        for (auto& buffer : trace_.bufferEvents) {
            for (auto& block : buffer.blocks) resolve(block.header);
        }
    }

    void addObservation(std::string name, RawProbeResultKind result,
                        DWORD error) {
        if (trace_.observations.size() >= kMaxTraceEvents) {
            trace_.traceOverflowed = true;
            return;
        }
        trace_.observations.push_back({std::move(name), result,
                                       "system_error=" + std::to_string(error)});
    }

    std::size_t totalEventCount() const noexcept {
        return trace_.registrationEvents.size() + trace_.messageEvents.size() +
               trace_.dataEvents.size() + trace_.bufferEvents.size() +
               trace_.observations.size();
    }

    RawInputProbeTrace trace_;
    HWND windowA_{nullptr};
    HWND windowB_{nullptr};
    bool cleanupComplete_{false};
    bool cleanupResult_{true};
    bool ownsKeyboardRegistration_{false};
    bool ownsMouseRegistration_{false};
    std::uint64_t nextSequence_{1};
    // Microsoft documents an 8-byte alignment requirement for x86 processes
    // running under WOW64. Eight bytes is also valid for native x86/x64.
    alignas(8) std::array<std::byte, kMaxRawPacketBytes> scratch_{};
};

std::filesystem::path currentExecutablePath() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    return std::filesystem::path(std::wstring(path.data(), length));
}

bool runChildAndWait(const std::filesystem::path& executable) {
    std::wstring command = L"\"" + executable.wstring() +
                           L"\" --registration-child-no-cleanup";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        CloseHandle(job);
        return false;
    }
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
                        &startup, &process)) {
        CloseHandle(job);
        return false;
    }
    if (!AssignProcessToJobObject(job, process.hProcess) ||
        ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.hProcess, 2);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 20000);
    DWORD exitCode = (std::numeric_limits<DWORD>::max)();
    BOOL queried = FALSE;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 2);
        WaitForSingleObject(process.hProcess, 5000);
    } else if (wait == WAIT_OBJECT_0) {
        queried = GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hProcess);
    CloseHandle(job);
    return wait == WAIT_OBJECT_0 && queried && exitCode == 0;
}

bool runProcessTeardownSelfTest() {
    RawInputProbe parent;
    const auto before = parent.currentRegistrations();
    if (!before.api.success) return false;
    const auto executable = currentExecutablePath();
    if (executable.empty() || !runChildAndWait(executable) ||
        !runChildAndWait(executable)) {
        return false;
    }
    const auto after = parent.currentRegistrations();
    return after.api.success && before.registrations == after.registrations;
}

#endif

} // namespace

int main(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    if (!options) {
        printUsage();
        return 2;
    }
    if (options->mode == Mode::SelfTest) {
        if (!runPortableSelfTest()) return 1;
        std::cout << "Raw Input trace self-test passed\n";
        return 0;
    }
#ifdef _WIN32
    if (options->mode == Mode::ProcessTeardownSelfTest) {
        if (!runProcessTeardownSelfTest()) return 1;
        std::cout << "Raw Input process teardown self-test passed\n";
        return 0;
    }
    RawInputProbe probe;
    if (options->mode == Mode::RegistrationChildNoCleanup) {
        if (!probe.registerForTeardownChild()) return 1;
        ExitProcess(0);
    }
    const bool success = options->mode == Mode::Observe
        ? probe.runObserve(options->durationSeconds)
        : probe.runRegistrationLifecycle();
    const auto trace = probe.finishTrace();
    if (!writeTrace(options->outputPath, trace)) {
        std::cerr << "Failed to write bounded Raw Input trace\n";
        return 1;
    }
    if (!success) return 1;
    std::cout << "Raw Input probe completed: physical_input_observed="
              << (trace.physicalInputObserved ? "true" : "false")
              << " device_change_observed="
              << (trace.deviceChangeObserved ? "true" : "false") << '\n';
    return 0;
#else
    std::cerr << "Windows Raw Input integration is unavailable on this platform\n";
    return 3;
#endif
}
