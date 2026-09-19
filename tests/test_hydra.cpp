#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/hardware_detector.hpp"
#include "hydra/display_manager.hpp"
#include "hydra/workspace_manager.hpp"
#include "hydra/input_router.hpp"
#include "hydra/runtime_authority.hpp"

#include <iostream>
#include <cassert>
#include <cstdio>
#include <fstream>

void testControllerIdentity();

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
    assert(mgr.createWorkspace(L"Player 3") == 0);
    assert(mgr.getAllWorkspaces().size() == 2);

    assert(mgr.assignDisplay(ws1, L"\\\\.\\DISPLAY1"));
    assert(mgr.assignController(ws1, L"container-a"));

    const auto* wsConfig = mgr.getWorkspace(ws1);
    assert(wsConfig != nullptr);
    assert(wsConfig->displayDeviceName == L"\\\\.\\DISPLAY1");
    assert(wsConfig->controllerId == std::optional<std::wstring>{L"container-a"});

    const char* profilePath = "workspace_config_test.json";
    assert(mgr.saveToFile(profilePath));
    std::ifstream profile(profilePath);
    const std::string saved((std::istreambuf_iterator<char>(profile)),
                            std::istreambuf_iterator<char>());
    assert(saved.find("\"version\": \"1.1\"") != std::string::npos);
    assert(saved.find("\"controllerId\": \"container-a\"") != std::string::npos);
    std::remove(profilePath);

    hydra::WorkspaceManager reused;
    assert(reused.createWorkspace(L"Seat 1") == 1);
    assert(reused.createWorkspace(L"Seat 2") == 2);
    assert(reused.removeWorkspace(1));
    assert(reused.createWorkspace(L"Seat 1 again") == 1);

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

    const hydra::controller::SeatBinding seat1Controller{
        1, hydra::controller::ApiSurface::GameInput, "gameinput:pad-a",
        std::wstring{L"container-a"}, std::nullopt};
    assert(controller.bindController(first, seat1Controller));

    auto snapshot = controller.snapshot(1);
    assert(snapshot.has_value());
    assert(snapshot->active);
    assert(snapshot->process == process);
    assert(snapshot->targetHwnd == 0x100);
    assert(snapshot->controllerBinding == seat1Controller);

    // Starting a new generation must never silently forget a live Seat.
    const auto overlapping = controller.beginSeatActivation(1);
    assert(!overlapping.valid());
    snapshot = controller.snapshot(1);
    assert(snapshot.has_value());
    assert(snapshot->generation == first.generation);
    assert(snapshot->process == process);
    assert(snapshot->targetHwnd == 0x100);
    assert(snapshot->controllerBinding == seat1Controller);

    const auto otherSeat = controller.beginSeatActivation(2);
    assert(otherSeat.valid());

    // Machine-wide process/window ownership is exclusive across both Seats.
    assert(!controller.publishProcess(otherSeat, process));
    const hydra::runtime::ProcessIdentity otherProcess{6262, 3003};
    assert(controller.publishProcess(otherSeat, otherProcess));
    assert(!controller.bindTargetWindow(otherSeat, otherProcess, 0x100));
    assert(controller.bindTargetWindow(otherSeat, otherProcess, 0x200));

    // Same physical controller cannot be owned by both Seats even if the
    // runtime-facing key differs.
    const hydra::controller::SeatBinding duplicateController{
        2, hydra::controller::ApiSurface::GameInput, "gameinput:pad-a-second-view",
        std::wstring{L"CONTAINER-A"}, std::nullopt};
    assert(!controller.bindController(otherSeat, duplicateController));

    const hydra::controller::SeatBinding seat2Controller{
        2, hydra::controller::ApiSurface::XInput, "xinput-slot:0",
        std::nullopt, std::uint8_t{0}};
    assert(controller.bindController(otherSeat, seat2Controller));

    // GameInput is not silently downgraded to XInput.
    const auto unsupportedPoll = controller.pollController(first);
    assert(unsupportedPoll.status == hydra::controller::IoStatus::UnsupportedApi);

    const auto seat2Poll = controller.pollController(otherSeat);
    const auto seat2Vibration = controller.setControllerVibration(
        otherSeat, std::uint16_t{0}, std::uint16_t{0});
#ifdef _WIN32
    assert(seat2Poll.status == hydra::controller::IoStatus::Ok ||
           seat2Poll.status == hydra::controller::IoStatus::Disconnected);
    assert(seat2Vibration == hydra::controller::IoStatus::Ok ||
           seat2Vibration == hydra::controller::IoStatus::Disconnected);
#else
    assert(seat2Poll.status == hydra::controller::IoStatus::PlatformUnavailable);
    assert(seat2Vibration == hydra::controller::IoStatus::PlatformUnavailable);
#endif

    assert(controller.endSeatActivation(first));
    snapshot = controller.snapshot(1);
    assert(snapshot.has_value());
    assert(!snapshot->active);
    assert(!snapshot->process.has_value());
    assert(snapshot->targetHwnd == 0);
    assert(!snapshot->controllerBinding.has_value());

    // Restart is stop -> verified cleanup -> new generation.
    const auto restarted = controller.beginSeatActivation(1);
    assert(restarted.valid());
    assert(restarted.generation > first.generation);
    assert(!controller.publishProcess(first, process));
    assert(!controller.bindTargetWindow(first, process, 0x300));
    assert(!controller.bindController(first, seat1Controller));
    assert(!controller.endSeatActivation(first));

    const hydra::runtime::ProcessIdentity replacement{5252, 2002};
    assert(controller.publishProcess(restarted, replacement));
    assert(controller.bindTargetWindow(restarted, replacement, 0x300));

    const auto seat2Snapshot = controller.snapshot(2);
    assert(seat2Snapshot.has_value());
    assert(seat2Snapshot->active);
    assert(seat2Snapshot->process == otherProcess);
    assert(seat2Snapshot->targetHwnd == 0x200);
    assert(seat2Snapshot->controllerBinding == seat2Controller);

    assert(controller.endSeatActivation(restarted));
    assert(controller.endSeatActivation(otherSeat));
    assert(controller.pollController(otherSeat).status ==
           hydra::controller::IoStatus::InvalidBinding);
    assert(controller.setControllerVibration(
               otherSeat, std::uint16_t{0}, std::uint16_t{0}) ==
           hydra::controller::IoStatus::InvalidBinding);
    assert(!controller.beginSeatActivation(3).valid());

    std::cout << "[Test] RuntimeAuthority tests passed." << std::endl;
}

int main() {
    bool assertionProbe = false;
    assert((assertionProbe = true));
    if (!assertionProbe) {
        std::cerr << "[FAIL] assertions are disabled in hydra_tests" << std::endl;
        return 2;
    }

    std::cout << "Running HydraSeat Engine Tests..." << std::endl;
    testHardwareDetector();
    testWorkspaceManager();
    testRuntimeAuthority();
    testControllerIdentity();
    std::cout << "All HydraSeat Engine Tests Passed!" << std::endl;
    return 0;
}
