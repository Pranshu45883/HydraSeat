#include "hydra/hardware_detector.hpp"
#include "hydra/display_manager.hpp"
#include "hydra/workspace_manager.hpp"
#include "hydra/input_router.hpp"
#include "hydra/runtime_authority.hpp"

#include <iostream>
#include <cassert>

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

void testRuntimeAuthority() {
    hydra::runtime::SessionController controller;

    const auto first = controller.beginSeatActivation(1);
    assert(first.valid());
    assert(first.seatId == 1);

    const hydra::runtime::ProcessIdentity process{4242, 1001};
    assert(controller.publishProcess(first, process));
    assert(!controller.bindTargetWindow(first, {4242, 1002}, 0x100));
    assert(controller.bindTargetWindow(first, process, 0x100));

    auto snapshot = controller.snapshot(1);
    assert(snapshot.has_value());
    assert(snapshot->active);
    assert(snapshot->process == process);
    assert(snapshot->targetHwnd == 0x100);

    // Starting a new generation must never silently forget a live Seat.
    const auto overlapping = controller.beginSeatActivation(1);
    assert(!overlapping.valid());
    snapshot = controller.snapshot(1);
    assert(snapshot.has_value());
    assert(snapshot->generation == first.generation);
    assert(snapshot->process == process);
    assert(snapshot->targetHwnd == 0x100);

    const auto otherSeat = controller.beginSeatActivation(2);
    assert(otherSeat.valid());

    // Machine-wide ownership is exclusive across both Seats.
    assert(!controller.publishProcess(otherSeat, process));
    const hydra::runtime::ProcessIdentity otherProcess{6262, 3003};
    assert(controller.publishProcess(otherSeat, otherProcess));
    assert(!controller.bindTargetWindow(otherSeat, otherProcess, 0x100));
    assert(controller.bindTargetWindow(otherSeat, otherProcess, 0x200));

    assert(controller.endSeatActivation(first));
    snapshot = controller.snapshot(1);
    assert(snapshot.has_value());
    assert(!snapshot->active);
    assert(!snapshot->process.has_value());
    assert(snapshot->targetHwnd == 0);

    // Restart is stop -> verified cleanup -> new generation.
    const auto restarted = controller.beginSeatActivation(1);
    assert(restarted.valid());
    assert(restarted.generation > first.generation);
    assert(!controller.publishProcess(first, process));
    assert(!controller.bindTargetWindow(first, process, 0x300));
    assert(!controller.endSeatActivation(first));

    const hydra::runtime::ProcessIdentity replacement{5252, 2002};
    assert(controller.publishProcess(restarted, replacement));
    assert(controller.bindTargetWindow(restarted, replacement, 0x300));

    const auto seat2Snapshot = controller.snapshot(2);
    assert(seat2Snapshot.has_value());
    assert(seat2Snapshot->active);
    assert(seat2Snapshot->process == otherProcess);
    assert(seat2Snapshot->targetHwnd == 0x200);

    assert(controller.endSeatActivation(restarted));
    assert(controller.endSeatActivation(otherSeat));
    assert(!controller.beginSeatActivation(3).valid());
    std::cout << "[Test] RuntimeAuthority tests passed." << std::endl;
}

int main() {
    std::cout << "Running HydraSeat Engine Tests..." << std::endl;
    testHardwareDetector();
    testWorkspaceManager();
    testRuntimeAuthority();
    std::cout << "All HydraSeat Engine Tests Passed!" << std::endl;
    return 0;
}
