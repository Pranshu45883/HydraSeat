#include "hydra/hardware_detector.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void printCategory(
    std::wstring_view title,
    const std::vector<hydra::DeviceInfo>& devices) {
    std::wcout << L"[" << title << L": " << devices.size() << L"]\n";
    for (const auto& device : devices) {
        std::wcout << L"  Name: " << device.name << L"\n"
                   << L"  ID: " << device.id << L"\n"
                   << L"  Path: "
                   << (device.devicePath.empty() ? L"<unavailable>" : device.devicePath)
                   << L"\n"
                   << L"  Native/index: " << device.nativeHandle << L"\n\n";
    }
}

} // namespace

int main() {
    hydra::HardwareDetector detector;

    printCategory(L"Displays", detector.detectDisplays());
    printCategory(L"Keyboards", detector.detectKeyboards());
    printCategory(L"Mice / touchpads", detector.detectMice());
    printCategory(L"Controllers", detector.detectControllers());

    // The current detector API does not expose an authoritative failure object.
    // Empty categories are therefore reported as observations, not treated as
    // proof that hardware is absent or that enumeration succeeded completely.
    return EXIT_SUCCESS;
}
