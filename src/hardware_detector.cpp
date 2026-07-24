#include "hydra/hardware_detector.hpp"

#include <iostream>
#include <vector>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <dxgi.h>
#include <xinput.h>
#endif

namespace hydra {

std::vector<DeviceInfo> HardwareDetector::detectDisplays() {
    std::vector<DeviceInfo> result;

#ifdef _WIN32
    DISPLAY_DEVICEW dd;
    dd.cb = sizeof(dd);
    DWORD deviceNum = 0;

    while (EnumDisplayDevicesW(NULL, deviceNum, &dd, 0)) {
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            DeviceInfo info;
            info.id = dd.DeviceName;
            info.name = dd.DeviceString;
            info.devicePath = dd.DeviceID;
            info.type = DeviceType::Display;
            result.push_back(info);
        }
        deviceNum++;
    }
#endif

    return result;
}

std::vector<DeviceInfo> HardwareDetector::detectKeyboards() {
    std::vector<DeviceInfo> result;

#ifdef _WIN32
    UINT numDevices = 0;
    if (GetRawInputDeviceList(NULL, &numDevices, sizeof(RAWINPUTDEVICELIST)) != 0 || numDevices == 0) {
        return result;
    }

    std::vector<RAWINPUTDEVICELIST> rawList(numDevices);
    if (GetRawInputDeviceList(rawList.data(), &numDevices, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) {
        return result;
    }

    int kbdCount = 0;
    for (const auto& dev : rawList) {
        if (dev.dwType == RIM_TYPEKEYBOARD) {
            kbdCount++;
            DeviceInfo info;
            info.id = L"Keyboard_" + std::to_wstring(kbdCount);
            info.name = L"Physical Keyboard #" + std::to_wstring(kbdCount);
            info.type = DeviceType::Keyboard;
            info.nativeHandle = reinterpret_cast<uintptr_t>(dev.hDevice);

            UINT nameSize = 0;
            GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, NULL, &nameSize);
            if (nameSize > 0) {
                std::wstring nameBuf(nameSize, L'\0');
                if (GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, nameBuf.data(), &nameSize) != (UINT)-1) {
                    info.devicePath = nameBuf;
                }
            }

            result.push_back(info);
        }
    }
#endif

    return result;
}

std::vector<DeviceInfo> HardwareDetector::detectMice() {
    std::vector<DeviceInfo> result;

#ifdef _WIN32
    UINT numDevices = 0;
    if (GetRawInputDeviceList(NULL, &numDevices, sizeof(RAWINPUTDEVICELIST)) != 0 || numDevices == 0) {
        return result;
    }

    std::vector<RAWINPUTDEVICELIST> rawList(numDevices);
    if (GetRawInputDeviceList(rawList.data(), &numDevices, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) {
        return result;
    }

    int mouseCount = 0;
    for (const auto& dev : rawList) {
        if (dev.dwType == RIM_TYPEMOUSE) {
            mouseCount++;
            DeviceInfo info;
            info.id = L"Mouse_" + std::to_wstring(mouseCount);
            info.name = L"Physical Mouse / Touchpad #" + std::to_wstring(mouseCount);
            info.type = DeviceType::Mouse;
            info.nativeHandle = reinterpret_cast<uintptr_t>(dev.hDevice);

            UINT nameSize = 0;
            GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, NULL, &nameSize);
            if (nameSize > 0) {
                std::wstring nameBuf(nameSize, L'\0');
                if (GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, nameBuf.data(), &nameSize) != (UINT)-1) {
                    info.devicePath = nameBuf;
                }
            }

            result.push_back(info);
        }
    }
#endif

    return result;
}

std::vector<DeviceInfo> HardwareDetector::detectControllers() {
    std::vector<DeviceInfo> result;

#ifdef _WIN32
    // Check XInput slots (0..3)
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(XINPUT_STATE));
        if (XInputGetState(i, &state) == ERROR_SUCCESS) {
            DeviceInfo info;
            info.id = L"Controller_XInput_" + std::to_wstring(i + 1);
            info.name = L"Xbox / XInput Controller #" + std::to_wstring(i + 1);
            info.type = DeviceType::Controller;
            info.nativeHandle = static_cast<uintptr_t>(i);
            result.push_back(info);
        }
    }
#endif

    return result;
}

void HardwareDetector::printReport() {
    std::wcout << L"===========================================\n";
    std::wcout << L"       HydraSeat Hardware Report           \n";
    std::wcout << L"===========================================\n\n";

    auto displays = detectDisplays();
    std::wcout << L"[Displays Found: " << displays.size() << L"]\n";
    for (const auto& d : displays) {
        std::wcout << L"  - " << d.name << L" (" << d.id << L")\n";
    }

    auto keyboards = detectKeyboards();
    std::wcout << L"\n[Keyboards Found: " << keyboards.size() << L"]\n";
    for (const auto& k : keyboards) {
        std::wcout << L"  - " << k.name << L"\n";
        if (!k.devicePath.empty()) {
            std::wcout << L"    Path: " << k.devicePath << L"\n";
        }
    }

    auto mice = detectMice();
    std::wcout << L"\n[Mice / Touchpads Found: " << mice.size() << L"]\n";
    for (const auto& m : mice) {
        std::wcout << L"  - " << m.name << L"\n";
        if (!m.devicePath.empty()) {
            std::wcout << L"    Path: " << m.devicePath << L"\n";
        }
    }

    auto controllers = detectControllers();
    std::wcout << L"\n[Controllers Found: " << controllers.size() << L"]\n";
    for (const auto& c : controllers) {
        std::wcout << L"  - " << c.name << L"\n";
    }

    std::wcout << L"\n===========================================\n";
}

} // namespace hydra
