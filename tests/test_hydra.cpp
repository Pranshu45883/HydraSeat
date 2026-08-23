#include "hydra/hardware_detector.hpp"
#include "hydra/hid_usage.hpp"
#include "hydra/display_manager.hpp"
#include "hydra/workspace_manager.hpp"
#include "hydra/input_router.hpp"

#include <iostream>
#include <cassert>

static_assert(hydra::hid::classifyCollection(0x01, 0x02) == hydra::hid::CollectionKind::Mouse);
static_assert(hydra::hid::classifyCollection(0x0D, 0x05) == hydra::hid::CollectionKind::Touchpad);
static_assert(hydra::hid::classifyCollection(0x01, 0x06) == hydra::hid::CollectionKind::Keyboard);
static_assert(hydra::hid::classifyCollection(0x01, 0x04) == hydra::hid::CollectionKind::Joystick);
static_assert(hydra::hid::classifyCollection(0x01, 0x05) == hydra::hid::CollectionKind::Gamepad);
static_assert(hydra::hid::classifyCollection(0x0C, 0x01) == hydra::hid::CollectionKind::Other);
static_assert(hydra::hid::isMouseLikeCollection(0x01, 0x02));
static_assert(hydra::hid::isMouseLikeCollection(0x0D, 0x05));
static_assert(!hydra::hid::isMouseLikeCollection(0x01, 0x05));

void testHidUsageClassification() {
    std::cout << "[Test] HID usage classification tests passed." << std::endl;
}

void testHardwareDetector() {
    hydra::HardwareDetector detector;
    auto displays = detector.detectDisplays();
    std::cout << "[Test] Displays detected: " << displays.size() << std::endl;

    auto keyboards = detector.detectKeyboards();
    std::cout << "[Test] Keyboards detected: " << keyboards.size() << std::endl;
    for (size_t i = 0; i < keyboards.size(); ++i) {
        std::wcout << L"  KBD [" << i << L"]: handle=0x" << std::hex << keyboards[i].nativeHandle
                   << L", name=" << keyboards[i].name
                   << L", path=" << keyboards[i].devicePath << std::dec << std::endl;
    }

    auto mice = detector.detectMice();
    std::cout << "[Test] Mice detected: " << mice.size() << std::endl;
    for (size_t i = 0; i < mice.size(); ++i) {
        std::wcout << L"  MOU [" << i << L"]: handle=0x" << std::hex << mice[i].nativeHandle
                   << L", name=" << mice[i].name
                   << L", path=" << mice[i].devicePath << std::dec << std::endl;
    }
}

void testWorkspaceManager() {
    hydra::WorkspaceManager mgr;
    uint32_t ws1 = mgr.createWorkspace(L"Player 1");
    uint32_t ws2 = mgr.createWorkspace(L"Player 2");

    assert(ws1 == 1);
    assert(ws2 == 2);
    assert(mgr.getAllWorkspaces().size() == 2);

    bool assigned = mgr.assignDisplay(ws1, L"\\\\.\\DISPLAY1");
    assert(assigned);

    const auto* wsConfig = mgr.getWorkspace(ws1);
    assert(wsConfig != nullptr);
    assert(wsConfig->displayDeviceName == L"\\\\.\\DISPLAY1");

    std::cout << "[Test] WorkspaceManager tests passed." << std::endl;
}

int main() {
    std::cout << "Running HydraSeat Engine Tests..." << std::endl;
    testHidUsageClassification();
    testHardwareDetector();
    testWorkspaceManager();
    std::cout << "All HydraSeat Engine Tests Passed!" << std::endl;
    return 0;
}
