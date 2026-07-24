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

    // Query device path string from handle if available
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

bool InputRouter::initialize() {
#ifdef _WIN32
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = InputRouter::WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"HydraSeatRawInputHost";

    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        0, L"HydraSeatRawInputHost", L"HydraSeat Input Router",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL
    );

    if (!m_hwnd) {
        return false;
    }
#endif

    m_running = true;
    return registerRawInputDevices(true);
}

bool InputRouter::registerRawInputDevices(bool backgroundSink) {
#ifdef _WIN32
    if (!m_hwnd) return false;

    DWORD flags = backgroundSink ? RIDEV_INPUTSINK : 0;

    RAWINPUTDEVICE rid[3];

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
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
#endif
}

} // namespace hydra
