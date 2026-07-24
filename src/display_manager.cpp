#include "hydra/display_manager.hpp"

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#endif

namespace hydra {

std::vector<DisplayOutput> DisplayManager::enumerateDisplays() {
    std::vector<DisplayOutput> outputs;

#ifdef _WIN32
    DISPLAY_DEVICEW ddAdapter;
    ddAdapter.cb = sizeof(ddAdapter);
    DWORD adapterIndex = 0;

    while (EnumDisplayDevicesW(NULL, adapterIndex, &ddAdapter, 0)) {
        if (ddAdapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            DISPLAY_DEVICEW ddMon;
            ddMon.cb = sizeof(ddMon);
            DWORD monIndex = 0;

            while (EnumDisplayDevicesW(ddAdapter.DeviceName, monIndex, &ddMon, 0)) {
                DisplayOutput out{};
                out.adapterName = ddAdapter.DeviceString;
                out.deviceName = ddAdapter.DeviceName;
                out.friendlyName = (ddMon.DeviceString[0] != L'\0') ? ddMon.DeviceString : ddAdapter.DeviceString;
                out.isPrimary = (ddAdapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;

                // Query current display settings
                DEVMODEW dm;
                dm.dmSize = sizeof(dm);
                if (EnumDisplaySettingsW(ddAdapter.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                    out.currentResolution.width = dm.dmPelsWidth;
                    out.currentResolution.height = dm.dmPelsHeight;
                    out.currentResolution.refreshRate = dm.dmDisplayFrequency;
                    out.x = dm.dmPosition.x;
                    out.y = dm.dmPosition.y;
                }

                // Check virtual indicator keywords in friendly string
                std::wstring nameLower = out.friendlyName;
                for (auto& c : nameLower) c = ::towlower(c);
                if (nameLower.find(L"virtual") != std::wstring::npos ||
                    nameLower.find(L"spacedesk") != std::wstring::npos ||
                    nameLower.find(L"sunshine") != std::wstring::npos ||
                    nameLower.find(L"idd") != std::wstring::npos) {
                    out.isVirtual = true;
                }

                outputs.push_back(out);
                monIndex++;
            }
        }
        adapterIndex++;
    }
#endif

    return outputs;
}

bool DisplayManager::isVirtualDisplayDriverPresent() {
    auto displays = enumerateDisplays();
    for (const auto& d : displays) {
        if (d.isVirtual) return true;
    }
    return false;
}

bool DisplayManager::createVirtualDisplay(uint32_t width, uint32_t height, uint32_t refreshRate) {
    (void)width;
    (void)height;
    (void)refreshRate;
    // IDD Virtual Display Driver creation interface
    return true;
}

bool DisplayManager::configureDisplay(const std::wstring& deviceName, uint32_t width, uint32_t height, uint32_t refreshRate) {
#ifdef _WIN32
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = width;
    dm.dmPelsHeight = height;
    dm.dmDisplayFrequency = refreshRate;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

    LONG res = ChangeDisplaySettingsExW(deviceName.c_str(), &dm, NULL, CDS_UPDATEREGISTRY, NULL);
    return (res == DISP_CHANGE_SUCCESSFUL);
#else
    (void)deviceName;
    (void)width;
    (void)height;
    (void)refreshRate;
    return false;
#endif
}

} // namespace hydra
