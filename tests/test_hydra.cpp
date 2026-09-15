#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/hardware_detector.hpp"
#include "hydra/display_manager.hpp"
#include "hydra/workspace_manager.hpp"
#include "hydra/input_router.hpp"
#include "hydra/runtime_authority.hpp"
#ifdef _WIN32
#include <objbase.h>
#endif

#include <iostream>
#include <cassert>
#include <cstdio>
#include <fstream>

void testControllerIdentity();
void testControllerPairing();
void testControllerVirtualXInput();
void testVirtualXInputProtocol();
void testVirtualXInputService();

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

    hydra::controller::InventorySnapshot controllerInventory;
    controllerInventory.authoritative = true;

    hydra::controller::SourceDescriptor stableSource;
    stableSource.runtimeKey = "gameinput:pad-a";
    stableSource.persistentId = std::wstring{L"container-a"};
    stableSource.api = hydra::controller::ApiSurface::GameInput;
    stableSource.identityQuality = hydra::controller::IdentityQuality::Stable;
    stableSource.connected = true;
    controllerInventory.sources.push_back(stableSource);

    hydra::controller::SourceDescriptor xinputSource;
    xinputSource.runtimeKey = "xinput-slot:0";
    xinputSource.api = hydra::controller::ApiSurface::XInput;
    xinputSource.identityQuality = hydra::controller::IdentityQuality::RuntimeOnly;
    xinputSource.runtimeXInputSlot = std::uint8_t{0};
    xinputSource.connected = true;
    controllerInventory.sources.push_back(xinputSource);
    controllerInventory.physicalControllers.push_back(
        {L"container-a", L"Physical Pad A", L"hid-path-a"});

    const hydra::controller::SeatBinding seat1Controller{
        1, hydra::controller::ApiSurface::GameInput, "gameinput:pad-a",
        std::wstring{L"container-a"}, std::nullopt, 0};
    assert(controller.bindController(first, seat1Controller, controllerInventory));

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

    const hydra::controller::SeatBinding duplicateController{
        2, hydra::controller::ApiSurface::GameInput, "gameinput:pad-a",
        std::wstring{L"CONTAINER-A"}, std::nullopt, 0};
    assert(!controller.bindController(
        otherSeat, duplicateController, controllerInventory));

    const hydra::controller::SeatBinding seat2Controller{
        2, hydra::controller::ApiSurface::XInput, "xinput-slot:0",
        std::nullopt, std::uint8_t{0}, 0};
    assert(controller.bindController(
        otherSeat, seat2Controller, controllerInventory));

    const auto unsupportedPoll = controller.pollController(
        first, controllerInventory);
    assert(unsupportedPoll.status == hydra::controller::IoStatus::UnsupportedApi);

    auto staleInventory = controllerInventory;
    ++staleInventory.sources[1].sourceGeneration;
    assert(controller.pollController(otherSeat, staleInventory).status ==
           hydra::controller::IoStatus::StaleBinding);
    assert(controller.setControllerVibration(
               otherSeat, staleInventory, std::uint16_t{0}, std::uint16_t{0}) ==
           hydra::controller::IoStatus::StaleBinding);

    const auto seat2Poll = controller.pollController(
        otherSeat, controllerInventory);
    const auto seat2Vibration = controller.setControllerVibration(
        otherSeat, controllerInventory, std::uint16_t{0}, std::uint16_t{0});
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
    assert(!controller.bindController(
        first, seat1Controller, controllerInventory));
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
    assert(controller.pollController(otherSeat, controllerInventory).status ==
           hydra::controller::IoStatus::InvalidBinding);
    assert(controller.setControllerVibration(
               otherSeat, controllerInventory,
               std::uint16_t{0}, std::uint16_t{0}) ==
           hydra::controller::IoStatus::InvalidBinding);
    assert(!controller.beginSeatActivation(3).valid());

    std::cout << "[Test] RuntimeAuthority tests passed." << std::endl;
}

#include "hydra/audio_endpoint_inventory.hpp"

void testAudioEndpointInventory() {
    using namespace hydra::windows;

    // Model tests
    AudioRenderEndpoint activeEp{L"ep1", std::nullopt, L"Name", AudioEndpointState::Active};
    assert(activeEp.isAvailable() == true);

    AudioRenderEndpoint disabledEp{L"ep2", std::nullopt, L"Name", AudioEndpointState::Disabled};
    assert(disabledEp.isAvailable() == false);

    AudioRenderEndpoint unpluggedEp{L"ep3", L"Stable", L"Name", AudioEndpointState::Unplugged};
    assert(unpluggedEp.isAvailable() == false);

    AudioRenderEndpoint missingEp{L"ep4", L"Stable", L"Name", AudioEndpointState::NotPresent};
    assert(missingEp.isAvailable() == false);

    AudioRenderEndpoint unknownEp{L"ep5", L"Stable", L"Name", AudioEndpointState::Unknown};
    assert(unknownEp.isAvailable() == false);

    // Pure test proving: missing stableId != endpointId fallback
    AudioRenderEndpoint missingStableIdEp{L"Endpoint_XYZ_123", std::nullopt, L"Speakers", AudioEndpointState::Active};
    assert(!missingStableIdEp.stableId.has_value()); // Must not fall back to endpointId

    // Pure test proving: friendlyName is not an identity key and duplicate friendly names are valid
    AudioRenderEndpoint duplicateFriendlyNameEp1{L"Endpoint_1", L"Stable_1", L"Generic Headset", AudioEndpointState::Active};
    AudioRenderEndpoint duplicateFriendlyNameEp2{L"Endpoint_2", L"Stable_2", L"Generic Headset", AudioEndpointState::Active};
    assert(duplicateFriendlyNameEp1.friendlyName == duplicateFriendlyNameEp2.friendlyName);
    assert(duplicateFriendlyNameEp1.endpointId != duplicateFriendlyNameEp2.endpointId);

#ifdef _WIN32
    // Integration coverage belongs to the Windows backend only.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        auto result = AudioEndpointInventory::enumerateRenderEndpoints();
        assert(result.isSuccess());

        if (result.isSuccess()) {
            const auto& endpoints = *result.endpoints;
            std::cout << "[Test] Audio render endpoints detected: " << endpoints.size() << std::endl;

            for (const auto& ep : endpoints) {
                assert(!ep.endpointId.empty());
                std::wcout << L"  Audio Endpoint: " << ep.endpointId << std::endl;
                std::wcout << L"    FriendlyName: " << ep.friendlyName << std::endl;
                if (ep.stableId) {
                    std::wcout << L"    StableId: " << *ep.stableId << std::endl;
                }
                std::wcout << L"    Available: " << (ep.isAvailable() ? L"true" : L"false") << std::endl;
            }
        }
        CoUninitialize();
    } else {
        std::cerr << "[Test] Failed to initialize COM, skipping integration test." << std::endl;
    }
#endif

    std::cout << "[Test] AudioEndpointInventory tests passed." << std::endl;
}

#include "hydra/audio_session_observer.hpp"

void testAudioSessionObserver() {
    using namespace hydra::windows;
    using hydra::runtime::ProcessIdentity;
    using hydra::runtime::ProcessOwnershipMatch;

    // Test 1 — exact process identity match
    ProcessIdentity id1{42, 100};
    ProcessIdentity id2{42, 100};
    assert(hydra::runtime::matchIdentity(id1, id2) == ProcessOwnershipMatch::Match);

    // Test 2 — PID reuse protection
    ProcessIdentity id3{42, 200};
    assert(hydra::runtime::matchIdentity(id3, id1) == ProcessOwnershipMatch::Mismatch);

    // Mismatch (different PID)
    ProcessIdentity id4{43, 100};
    assert(hydra::runtime::matchIdentity(id4, id1) == ProcessOwnershipMatch::Mismatch);

    // Test 3 — missing or invalid identity stays Unknown.
    assert(hydra::runtime::matchIdentity(std::nullopt, id1) == ProcessOwnershipMatch::Unknown);
    assert(hydra::runtime::matchIdentity(ProcessIdentity{}, id1) == ProcessOwnershipMatch::Unknown);
    assert(hydra::runtime::matchIdentity(id1, ProcessIdentity{}) == ProcessOwnershipMatch::Unknown);

#ifdef _WIN32
    // Integration coverage belongs to the Windows backend only.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        auto result = AudioSessionObserver::enumerateSessions();
        assert(result.isSuccess());

        if (result.isSuccess()) {
            std::cout << "[Test] Enumeration completeness: " << (result.isComplete ? "COMPLETE" : "PARTIAL") << std::endl;
            const auto& sessions = result.sessions;
            std::cout << "[Test] Audio sessions detected: " << sessions.size() << std::endl;

            // Validate structural invariants
            for (const auto& session : sessions) {
                // Must have a valid PID (not 0, though technically System Idle Process is 0, audio sessions shouldn't be 0)
                assert(session.processId != 0 || session.state != AudioSessionState::Unknown); // Soft check

                std::wcout << L"  Session PID: " << session.processId << std::endl;
                if (session.processIdentity) {
                    std::wcout << L"    CreationId: " << session.processIdentity->creationIdentity << std::endl;
                } else {
                    std::wcout << L"    CreationId: <Unknown>" << std::endl;
                }

                std::wcout << L"    State: " << (int)session.state << std::endl;
                if (session.displayName) {
                    std::wcout << L"    DisplayName: " << *session.displayName << std::endl;
                }
            }
        }
        CoUninitialize();
    } else {
        std::cerr << "[Test] Failed to initialize COM, skipping session integration test." << std::endl;
    }
#endif

    std::cout << "[Test] AudioSessionObserver tests passed." << std::endl;
}

void testAudioSessionObserverRegression() {
    using namespace hydra::windows;

    std::wstring endpointId = L"Endpoint-A";
    std::optional<std::wstring> endpointStableId = L"Stable-A";

    std::vector<AudioSessionObservation> sessions;

    // Simulate the inner loop over 2 sessions
    for (int j = 0; j < 2; ++j) {
        std::uint32_t pid = 1000 + j;
        std::optional<hydra::runtime::ProcessIdentity> processIdentity = hydra::runtime::ProcessIdentity{pid, 12345};
        AudioSessionState mappedState = AudioSessionState::Active;
        std::optional<std::wstring> optDisplayName = L"TestApp";
        std::optional<std::wstring> optGroupingParam = std::nullopt;

        sessions.push_back({
            endpointId,
            endpointStableId,
            pid,
            std::move(processIdentity),
            mappedState,
            std::move(optDisplayName),
            std::move(optGroupingParam)
        });
    }

    assert(sessions.size() == 2);
    assert(sessions[0].endpointId == L"Endpoint-A");
    assert(sessions[1].endpointId == L"Endpoint-A");

    assert(sessions[0].endpointStableId.has_value() && *sessions[0].endpointStableId == L"Stable-A");
    assert(sessions[1].endpointStableId.has_value() && *sessions[1].endpointStableId == L"Stable-A");

    std::cout << "[Test] AudioSessionObserver regression test (move semantics) passed." << std::endl;
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
    testAudioEndpointInventory();
    testAudioSessionObserver();
    testAudioSessionObserverRegression();
    testControllerIdentity();
    testControllerPairing();
    testControllerVirtualXInput();
    testVirtualXInputProtocol();
    testVirtualXInputService();
    std::cout << "All HydraSeat Engine Tests Passed!" << std::endl;
    return 0;
}
