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
    // nativeHandle is retained for diagnostics/backward compatibility. For
    // physical Raw Input devices, nativeHandles contains every top-level
    // collection that belongs to the same stable physical identity.
    uintptr_t nativeHandle{0};
    std::vector<uintptr_t> nativeHandles;
    // Composite receivers and secondary HID functions are hidden from the
    // user-facing inventory until Raw Input activity proves that role is
    // actually in use. The canonical host still keeps the candidate identity
    // available for validation once the UI has confirmed it.
    bool requiresActivityConfirmation{false};
    // We could not prove a local hardware transport. Unlike composite HID
    // role confirmation, a persisted profile must not override this guard.
    bool requiresFreshTransportConfirmation{false};
};

class HardwareDetector {
public:
    HardwareDetector() = default;
    ~HardwareDetector() = default;

    // Detect all connected displays (Physical & Virtual)
    std::vector<DeviceInfo> detectDisplays();

    // Detect all physical keyboards separately
    std::vector<DeviceInfo> detectKeyboards();

    // Detect independently routable physical mouse/touchpad endpoints.
    std::vector<DeviceInfo> detectMice();

    // Read-only inventory of Precision Touchpad HID sensors. These are NOT
    // independently assignable mouse devices unless Windows exposes separate
    // attributable Raw Input mouse events (hDevice can be null for touchpads).
    std::vector<DeviceInfo> detectTouchpadSensors();

    // Detect all connected gamepads/controllers
    std::vector<DeviceInfo> detectControllers();

    // Print summary of all detected hardware
    void printReport();
};

} // namespace hydra
