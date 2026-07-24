#include <windows.h>
#include <iostream>

int main() {
    std::cout << "Resetting Windows Raw Input device registrations..." << std::endl;

    RAWINPUTDEVICE rid[3];

    // Remove Keyboard Hook
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x06;
    rid[0].dwFlags = RIDEV_REMOVE;
    rid[0].hwndTarget = NULL;

    // Remove Mouse Hook
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x02;
    rid[1].dwFlags = RIDEV_REMOVE;
    rid[1].hwndTarget = NULL;

    // Remove Touchpad Hook
    rid[2].usUsagePage = 0x01;
    rid[2].usUsage = 0x05;
    rid[2].dwFlags = RIDEV_REMOVE;
    rid[2].hwndTarget = NULL;

    if (RegisterRawInputDevices(rid, 3, sizeof(RAWINPUTDEVICE))) {
        std::cout << "[SUCCESS] Windows Raw Input hooks unregistered successfully!" << std::endl;
    } else {
        std::cout << "[FAILED] Error code: " << GetLastError() << std::endl;
    }

    return 0;
}
