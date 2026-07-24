#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace hydra {

enum class DeviceType {
    Display,
    Keyboard,
    Mouse,
    Controller
};

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    std::wstring devicePath;
    DeviceType type;
    uintptr_t nativeHandle{0};
};

class HardwareDetector {
public:
    HardwareDetector() = default;
    ~HardwareDetector() = default;

    // Detect all connected displays (Physical & Virtual)
    std::vector<DeviceInfo> detectDisplays();

    // Detect all physical keyboards separately
    std::vector<DeviceInfo> detectKeyboards();

    // Detect all physical mice / touchpads separately
    std::vector<DeviceInfo> detectMice();

    // Detect all connected gamepads/controllers
    std::vector<DeviceInfo> detectControllers();

    // Print summary of all detected hardware
    void printReport();
};

} // namespace hydra
