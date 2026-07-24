#include "hydra/input_router.hpp"

#include <iostream>

namespace hydra {

static InputRouter* g_routerInstance = nullptr;

InputRouter::InputRouter() {
    g_routerInstance = this;
}

InputRouter::~InputRouter() {
    stop();
    if (g_routerInstance == this) {
        g_routerInstance = nullptr;
    }
}

bool InputRouter::postInputToWindow(uint64_t hwndVal, const RawInputEvent& evt) {
#ifdef _WIN32
    HWND hwnd = reinterpret_cast<HWND>(hwndVal);
    if (!hwnd || !IsWindow(hwnd)) return false;

    if (evt.vkey > 0) {
        UINT msg = (evt.messageType == WM_KEYDOWN) ? WM_KEYDOWN : WM_KEYUP;
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
LRESULT CALLBACK InputRouter::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUT && g_routerInstance) {
        g_routerInstance->handleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
        return 0;
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
    } else if (raw->header.dwType == RIM_TYPEMOUSE) {
        event.messageType = WM_MOUSEMOVE;
        event.deltaX = raw->data.mouse.lLastX;
        event.deltaY = raw->data.mouse.lLastY;
        event.mouseButtons = raw->data.mouse.usButtonFlags;
    } else if (raw->header.dwType == RIM_TYPEHID) {
        event.messageType = WM_MOUSEMOVE;
        event.isTouchpad = true;
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
            WS_EX_TOOLWINDOW, L"HydraSeatRawInputHost", L"HydraSeat Input Router",
            WS_POPUP, 0, 0, 0, 0,
            NULL, NULL, GetModuleHandle(NULL), NULL
        );

        if (!m_hwnd) {
            return false;
        }
    }
#else
    (void)targetHwndVal;
#endif

    m_running = true;
    return registerRawInputDevices(false);
}

bool InputRouter::registerRawInputDevices(bool backgroundSink) {
#ifdef _WIN32
    if (!m_hwnd) return false;

    // Use dwFlags = 0 (or RIDEV_DEVNOTIFY) to monitor Raw Input without suppressing legacy typing
    DWORD flags = backgroundSink ? 0 : 0;

    RAWINPUTDEVICE rid[3];

    // Keyboard (Non-blocking passive monitoring)
    rid[0].usUsagePage = 0x01; // Generic Desktop
    rid[0].usUsage = 0x06;     // Keyboard
    rid[0].dwFlags = flags;
    rid[0].hwndTarget = m_hwnd;

    // Mouse (Non-blocking passive monitoring)
    rid[1].usUsagePage = 0x01; // Generic Desktop
    rid[1].usUsage = 0x02;     // Mouse
    rid[1].dwFlags = flags;
    rid[1].hwndTarget = m_hwnd;

    // Touchpad / Precision Touchpad
    rid[2].usUsagePage = 0x01; // Generic Desktop
    rid[2].usUsage = 0x05;     // Touch Pad
    rid[2].dwFlags = flags;
    rid[2].hwndTarget = m_hwnd;

    if (!RegisterRawInputDevices(rid, 3, sizeof(RAWINPUTDEVICE))) {
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
        // Remove raw input device registrations before closing window
        RAWINPUTDEVICE rid[3];
        rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x06; rid[0].dwFlags = RIDEV_REMOVE; rid[0].hwndTarget = NULL;
        rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x02; rid[1].dwFlags = RIDEV_REMOVE; rid[1].hwndTarget = NULL;
        rid[2].usUsagePage = 0x01; rid[2].usUsage = 0x05; rid[2].dwFlags = RIDEV_REMOVE; rid[2].hwndTarget = NULL;
        RegisterRawInputDevices(rid, 3, sizeof(RAWINPUTDEVICE));

        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
#endif
}

} // namespace hydra
