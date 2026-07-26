#include "hydra/hardware_detector.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <dxgi.h>
#include <xinput.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#endif

namespace hydra {

// Extract clean hardware ID key (strips HID sub-collections like &Col01, &Col02)
static std::wstring getHardwareDeviceKey(const std::wstring& devPath) {
    std::wstring pathUpper = devPath;
    for (auto& c : pathUpper) c = ::towupper(c);

    size_t start = pathUpper.find(L"HID#");
    if (start == std::wstring::npos) start = pathUpper.find(L"ACPI#");
    if (start != std::wstring::npos) {
        size_t firstHash = pathUpper.find(L"#", start);
        if (firstHash != std::wstring::npos) {
            size_t secondHash = pathUpper.find(L"#", firstHash + 1);
            if (secondHash != std::wstring::npos) {
                std::wstring key = pathUpper.substr(firstHash + 1, secondHash - firstHash - 1);
                size_t colPos = key.find(L"&COL");
                if (colPos != std::wstring::npos) {
                    key.erase(colPos);
                }
                return key;
            }
        }
    }
    return pathUpper;
}

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

    std::unordered_set<std::wstring> seenBaseIDs;

    // First pass: collect base IDs of all MOUSE/HID devices to detect combo devices
    std::unordered_set<std::wstring> mouseBaseIDs;
    for (const auto& dev : rawList) {
        if (dev.dwType == RIM_TYPEMOUSE || dev.dwType == RIM_TYPEHID) {
            std::wstring devPath;
            UINT nameSize = 0;
            GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, NULL, &nameSize);
            if (nameSize > 0) {
                std::wstring nameBuf(nameSize, L'\0');
                if (GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, nameBuf.data(), &nameSize) != (UINT)-1) {
                    devPath = nameBuf;
                }
            }
            std::wstring baseID = getHardwareDeviceKey(devPath);
            if (!baseID.empty()) {
                mouseBaseIDs.insert(baseID);
            }
        }
    }

    for (const auto& dev : rawList) {
        if (dev.dwType == RIM_TYPEKEYBOARD) {
            std::wstring devPath;
            UINT nameSize = 0;
            GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, NULL, &nameSize);
            if (nameSize > 0) {
                std::wstring nameBuf(nameSize, L'\0');
                if (GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, nameBuf.data(), &nameSize) != (UINT)-1) {
                    devPath = nameBuf;
                }
            }

            std::wstring pathUpper = devPath;
            for (auto& c : pathUpper) c = ::towupper(c);

            // Filter virtual RDP keyboards
            if (pathUpper.find(L"RDP_KBD") != std::wstring::npos || pathUpper.find(L"ROOT\\RDP") != std::wstring::npos) {
                continue;
            }

            // Filter synthetic "Microsoft Keyboard RID" virtual keyboard
            if (pathUpper.find(L"MICROSOFT KEYBOARD") != std::wstring::npos) {
                continue;
            }

            // Deduplicate sub-collections of the same physical USB keyboard
            std::wstring baseID = getHardwareDeviceKey(devPath);
            if (!baseID.empty() && seenBaseIDs.count(baseID) > 0) {
                continue; // Skip duplicate child HID collection
            }
            if (!baseID.empty()) {
                seenBaseIDs.insert(baseID);
            }

            // Filter keyboard sub-collections of USB combo devices that are primarily mice
            // (e.g., USB mouse with media buttons registers a keyboard HID interface)
            if (!baseID.empty() && mouseBaseIDs.count(baseID) > 0) {
                // This device also has mouse sub-collections → it's a mouse with extra keys, not a keyboard
                continue;
            }

            DeviceInfo info;
            info.type = DeviceType::Keyboard;
            info.nativeHandle = reinterpret_cast<uintptr_t>(dev.hDevice);
            info.devicePath = devPath;

            if (pathUpper.find(L"ACPI") != std::wstring::npos || pathUpper.find(L"MSFT0001") != std::wstring::npos || pathUpper.find(L"I8042PRT") != std::wstring::npos) {
                info.name = L"Laptop Internal Keyboard";
            } else if (pathUpper.find(L"HID") != std::wstring::npos || pathUpper.find(L"USB") != std::wstring::npos) {
                info.name = L"USB External Keyboard";
            } else {
                info.name = L"Keyboard";
            }

            info.id = L"Keyboard_unsorted";
            result.push_back(info);
        }
    }

    // Sort: Laptop Internal Keyboard always first, USB External keyboards after
    std::sort(result.begin(), result.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
        bool aIsLaptop = (a.name.find(L"Laptop") != std::wstring::npos);
        bool bIsLaptop = (b.name.find(L"Laptop") != std::wstring::npos);
        if (aIsLaptop != bIsLaptop) return aIsLaptop; // Laptop first
        return false; // Preserve relative order otherwise
    });

    // Re-number after sorting
    int kbdCount = 0;
    for (auto& info : result) {
        kbdCount++;
        if (info.name.find(L"Laptop") != std::wstring::npos) {
            info.name = L"Laptop Internal Keyboard";
        } else if (info.name.find(L"USB") != std::wstring::npos) {
            info.name = L"USB External Keyboard #" + std::to_wstring(kbdCount);
        } else {
            info.name = L"Keyboard #" + std::to_wstring(kbdCount);
        }
        info.id = L"Keyboard_" + std::to_wstring(kbdCount);
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

    std::unordered_set<std::wstring> seenBaseIDs;
    int padCount = 0;

    for (const auto& dev : rawList) {
        if (dev.dwType == RIM_TYPEMOUSE || dev.dwType == RIM_TYPEHID) {
            std::wstring devPath;
            UINT nameSize = 0;
            GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, NULL, &nameSize);
            if (nameSize > 0) {
                std::wstring nameBuf(nameSize, L'\0');
                if (GetRawInputDeviceInfoW(dev.hDevice, RIDI_DEVICENAME, nameBuf.data(), &nameSize) != (UINT)-1) {
                    devPath = nameBuf;
                }
            }

            std::wstring pathUpper = devPath;
            for (auto& c : pathUpper) c = ::towupper(c);

            // Filter virtual RDP mice
            if (pathUpper.find(L"RDP_MOU") != std::wstring::npos || pathUpper.find(L"ROOT\\RDP") != std::wstring::npos) {
                continue;
            }

            // Deduplicate sub-collections of the same physical USB mouse or touchpad controller
            std::wstring baseID = getHardwareDeviceKey(devPath);
            if (!baseID.empty() && seenBaseIDs.count(baseID) > 0) {
                continue;
            }
            if (!baseID.empty()) {
                seenBaseIDs.insert(baseID);
            }

            bool isTouchpad = (pathUpper.find(L"ELAN") != std::wstring::npos ||
                               pathUpper.find(L"SYN") != std::wstring::npos ||
                               pathUpper.find(L"MSFT0001") != std::wstring::npos ||
                               pathUpper.find(L"PNP0C50") != std::wstring::npos ||
                               pathUpper.find(L"ITE5570") != std::wstring::npos ||
                               pathUpper.find(L"TOUCHPAD") != std::wstring::npos);

            // Keep only ONE touchpad tile for the whole system
            if (isTouchpad && padCount > 0) {
                continue; // Skip creating a second touchpad tile
            }
            if (isTouchpad) padCount++;

            DeviceInfo info;
            info.type = DeviceType::Mouse;
            info.nativeHandle = reinterpret_cast<uintptr_t>(dev.hDevice);
            info.devicePath = devPath;

            if (isTouchpad) {
                info.name = L"Laptop Touchpad";
                info.id = L"Touchpad_unsorted";
            } else {
                info.name = L"USB External Mouse";
                info.id = L"Mouse_unsorted";
            }

            result.push_back(info);
        }
    }

    // Sort: USB External Mice first, Laptop Touchpads last
    std::sort(result.begin(), result.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
        bool aIsTouchpad = (a.name.find(L"Touchpad") != std::wstring::npos);
        bool bIsTouchpad = (b.name.find(L"Touchpad") != std::wstring::npos);
        if (aIsTouchpad != bIsTouchpad) return !aIsTouchpad; // USB mice first
        return false;
    });

    // Re-number after sorting
    int mouseCount = 0;
    padCount = 0;
    for (auto& info : result) {
        if (info.name.find(L"Touchpad") != std::wstring::npos) {
            padCount++;
            info.name = L"Laptop Touchpad";
            info.id = L"Touchpad_" + std::to_wstring(padCount);
        } else {
            mouseCount++;
            info.name = L"USB External Mouse #" + std::to_wstring(mouseCount);
            info.id = L"Mouse_" + std::to_wstring(mouseCount);
        }
    }
#endif

    return result;
}

std::vector<DeviceInfo> HardwareDetector::detectControllers() {
    std::vector<DeviceInfo> result;

#ifdef _WIN32
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
