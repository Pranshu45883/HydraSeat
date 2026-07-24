#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hydra {

struct RawInputEvent {
    uintptr_t deviceHandle{0};
    std::wstring devicePath;
    uint32_t messageType{0}; // WM_KEYDOWN, WM_KEYUP, WM_MOUSEMOVE, etc.
    uint32_t vkey{0};
    int32_t deltaX{0};
    int32_t deltaY{0};
    uint32_t mouseButtons{0};
    bool isTouchpad{false};
};

using InputCallback = std::function<void(const RawInputEvent&)>;

class InputRouter {
public:
    InputRouter();
    ~InputRouter();

    // Initialize Win32 Raw Input hook window
    bool initialize();

    // Register Raw Input sink for keyboards, mice, and touchpads
    bool registerRawInputDevices(bool backgroundSink = true);

    // Bind a callback to receive routed input events from a specific device handle
    void subscribeDevice(uintptr_t deviceHandle, InputCallback callback);

    // Bind a global callback for all raw input events
    void setGlobalCallback(InputCallback callback);

    // Process Win32 window message loop for Raw Input
    void processMessages();

    // Stop raw input hook
    void stop();

    // Check if input router is running
    bool isRunning() const { return m_running; }

private:
#ifdef _WIN32
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void handleRawInput(HRAWINPUT hRawInput);
    HWND m_hwnd{nullptr};
#endif

    bool m_running{false};
    InputCallback m_globalCallback{nullptr};
    std::unordered_map<uintptr_t, InputCallback> m_deviceCallbacks;
};

} // namespace hydra
