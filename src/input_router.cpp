#include "hydra/input_router.hpp"

#include <chrono>
#include <iostream>

namespace hydra {

InputRouter::InputRouter() = default;

InputRouter::~InputRouter() {
    stop();
}

bool InputRouter::postInputToWindow(uint64_t hwndVal, const RawInputEvent& evt) {
#ifdef _WIN32
    HWND hwnd = reinterpret_cast<HWND>(hwndVal);
    if (!hwnd || !IsWindow(hwnd)) return false;

    if (evt.vkey > 0) {
        UINT msg = (evt.messageType == WM_KEYDOWN || evt.messageType == WM_SYSKEYDOWN) ? WM_KEYDOWN : WM_KEYUP;
        WPARAM wParam = static_cast<WPARAM>(evt.vkey);
        LPARAM lParam = (msg == WM_KEYDOWN) ? 0x00010001 : 0xC0010001;
        PostMessageW(hwnd, msg, wParam, lParam);
        return true;
    } else if (evt.deltaX != 0 || evt.deltaY != 0) {
        WPARAM wParam = 0;
        LPARAM lParam = MAKELPARAM(evt.deltaX, evt.deltaY);
        PostMessageW(hwnd, WM_MOUSEMOVE, wParam, lParam);
        return true;
    }
#else
    (void)hwndVal;
    (void)evt;
#endif

    return false;
}

#ifdef _WIN32
LRESULT CALLBACK InputRouter::WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam) {
    InputRouter* router = reinterpret_cast<InputRouter*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        const auto* create =
            reinterpret_cast<const CREATESTRUCTW*>(lParam);
        router = create
            ? static_cast<InputRouter*>(create->lpCreateParams)
            : nullptr;
        if (router != nullptr) {
            SetWindowLongPtrW(
                hwnd,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(router));
        }
    }

    if (msg == WM_INPUT && router != nullptr) {
        router->handleRawInput(
            reinterpret_cast<HRAWINPUT>(lParam));
    }

    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void InputRouter::handleRawInput(HRAWINPUT hRawInput) {
    UINT dwSize = 0;
    GetRawInputData(hRawInput, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
    if (dwSize == 0) return;

    std::vector<BYTE> lpb(dwSize);
    if (GetRawInputData(hRawInput, RID_INPUT, lpb.data(), &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) {
        return;
    }

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(lpb.data());
    RawInputEvent event{};
    event.deviceHandle = reinterpret_cast<uintptr_t>(raw->header.hDevice);
    event.rawDevType = raw->header.dwType;
    event.timestampMicros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    if (raw->header.hDevice != NULL) {
        UINT nameSize = 0;
        GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICENAME, NULL, &nameSize);
        if (nameSize > 0) {
            std::wstring nameBuf(nameSize, L'\0');
            if (GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICENAME, nameBuf.data(), &nameSize) != (UINT)-1) {
                event.devicePath = nameBuf;
            }
        }
    }

    if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        event.messageType = raw->data.keyboard.Message;
        event.vkey = raw->data.keyboard.VKey;
        event.scanCode = raw->data.keyboard.MakeCode;
        event.keyboardFlags = raw->data.keyboard.Flags;
        if (event.vkey == 0) {
            event.vkey = raw->data.keyboard.MakeCode;
        }
    } else if (raw->header.dwType == RIM_TYPEMOUSE) {
        event.messageType = WM_MOUSEMOVE;
        event.deltaX = raw->data.mouse.lLastX;
        event.deltaY = raw->data.mouse.lLastY;
        event.mouseButtonFlags = raw->data.mouse.usButtonFlags;
        event.mouseButtons = raw->data.mouse.usButtonFlags;
        if ((raw->data.mouse.usButtonFlags &
             (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) != 0) {
            event.wheelDelta =
                static_cast<std::int16_t>(raw->data.mouse.usButtonData);
        }

        // Detect if this mouse event is actually from a touchpad
        // Windows Precision Touchpads route motion through RIM_TYPEMOUSE
        if (!event.devicePath.empty()) {
            std::wstring pathUp = event.devicePath;
            for (auto& c : pathUp) c = ::towupper(c);
            if (pathUp.find(L"ELAN") != std::wstring::npos ||
                pathUp.find(L"SYN") != std::wstring::npos ||
                pathUp.find(L"ITE") != std::wstring::npos ||
                pathUp.find(L"ACPI") != std::wstring::npos ||
                pathUp.find(L"MSFT0001") != std::wstring::npos ||
                pathUp.find(L"PNP0C50") != std::wstring::npos) {
                event.isTouchpad = true;
            }
        }

        // CRITICAL: Windows Precision Touchpads frequently send RIM_TYPEMOUSE
        // events with hDevice == NULL (handle = 0). Physical USB mice ALWAYS
        // have valid non-zero handles. So if handle is NULL and type is MOUSE,
        // this is almost certainly a touchpad event.
        if (raw->header.hDevice == NULL) {
            event.isTouchpad = true;
        }
    } else {
        // Generic HID input (controllers, consumer controls, digitizers, etc.)
        // is not keyboard/mouse routing input. Do not reinterpret every HID
        // report as a touchpad event.
        return;
    }

    // Trigger device specific callback if registered
    auto it = m_deviceCallbacks.find(event.deviceHandle);
    if (it != m_deviceCallbacks.end() && it->second) {
        it->second(event);
    }

    // Trigger global callback
    if (m_globalCallback) {
        m_globalCallback(event);
    }
}
#endif

bool InputRouter::initialize(uint64_t targetHwndVal) {
#ifdef _WIN32
    if (targetHwndVal != 0) {
        m_hwnd = reinterpret_cast<HWND>(targetHwndVal);
    } else {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = InputRouter::WndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"HydraSeatRawInputHost";

        RegisterClassExW(&wc);

        m_hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            L"HydraSeatRawInputHost",
            L"HydraSeat Input Router",
            WS_POPUP,
            0,
            0,
            0,
            0,
            nullptr,
            nullptr,
            GetModuleHandle(nullptr),
            this);

        if (!m_hwnd) {
            return false;
        }
    }
#else
    (void)targetHwndVal;
#endif

    m_running = true;
    return registerRawInputDevices(true);
}

bool InputRouter::registerRawInputDevices(bool backgroundSink) {
#ifdef _WIN32
    if (!m_hwnd) return false;

    DWORD flags = backgroundSink ? (RIDEV_INPUTSINK | RIDEV_DEVNOTIFY) : RIDEV_DEVNOTIFY;

    // Register only the two Raw Input top-level collections HydraSeat
    // actually routes. Generic Desktop 0x05 is a GAME PAD, not a touchpad.
    // Precision Touchpads use Digitizers page 0x0D / usage 0x05 and are
    // system-owned; pointer-compatible touchpad activity already arrives
    // through the normal mouse collection when Windows exposes it that way.
    RAWINPUTDEVICE rid[2];

    // Keyboard
    rid[0].usUsagePage = 0x01; // Generic Desktop
    rid[0].usUsage = 0x06;     // Keyboard
    rid[0].dwFlags = flags;
    rid[0].hwndTarget = m_hwnd;

    // Mouse
    rid[1].usUsagePage = 0x01; // Generic Desktop
    rid[1].usUsage = 0x02;     // Mouse
    rid[1].dwFlags = flags;
    rid[1].hwndTarget = m_hwnd;

    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        return false;
    }
#else
    (void)backgroundSink;
#endif

    return true;
}

void InputRouter::subscribeDevice(uintptr_t deviceHandle, InputCallback callback) {
    m_deviceCallbacks[deviceHandle] = callback;
}

void InputRouter::setGlobalCallback(InputCallback callback) {
    m_globalCallback = callback;
}

void InputRouter::processMessages() {
#ifdef _WIN32
    MSG msg;
    while (PeekMessageW(&msg, m_hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
#endif
}

void InputRouter::stop() {
    m_running = false;
#ifdef _WIN32
    if (m_hwnd) {
        RAWINPUTDEVICE rid[2];
        rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x06; rid[0].dwFlags = RIDEV_REMOVE; rid[0].hwndTarget = NULL;
        rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x02; rid[1].dwFlags = RIDEV_REMOVE; rid[1].hwndTarget = NULL;
        RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));

        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
#endif
}

} // namespace hydra
