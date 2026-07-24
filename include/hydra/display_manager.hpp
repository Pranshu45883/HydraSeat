#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace hydra {

struct DisplayResolution {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t refreshRate{0};
};

struct DisplayOutput {
    std::wstring adapterName;
    std::wstring deviceName;
    std::wstring friendlyName;
    bool isPrimary{false};
    bool isVirtual{false};
    int32_t x{0};
    int32_t y{0};
    DisplayResolution currentResolution;
    std::vector<DisplayResolution> supportedResolutions;
};

class DisplayManager {
public:
    DisplayManager() = default;
    ~DisplayManager() = default;

    // Enumerate physical and virtual displays via DXGI and Windows Display Config API
    std::vector<DisplayOutput> enumerateDisplays();

    // Check if Virtual Display Driver (IDD / SpaceDesk / Sunshine) is installed
    bool isVirtualDisplayDriverPresent();

    // Create a virtual display output if IDD driver is available
    bool createVirtualDisplay(uint32_t width = 1920, uint32_t height = 1080, uint32_t refreshRate = 60);

    // Apply resolution & positioning settings to a target display
    bool configureDisplay(const std::wstring& deviceName, uint32_t width, uint32_t height, uint32_t refreshRate);
};

} // namespace hydra
