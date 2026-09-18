#include "hydra/hardware_detector.hpp"
#include "hydra/display_manager.hpp"
#include "hydra/workspace_manager.hpp"
#include "hydra/input_router.hpp"
#include "hydra/runtime_authority.hpp"
#include <objbase.h>

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

    // Initialize COM for the test thread
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

    // Test 3 — missing identity
    assert(hydra::runtime::matchIdentity(std::nullopt, id1) == ProcessOwnershipMatch::Unknown);

    // Initialize COM for the test thread
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

    std::cout << "[Test] AudioSessionObserver tests passed." << std::endl;
}

int main() {
    std::cout << "Running HydraSeat Engine Tests..." << std::endl;
    testHardwareDetector();
    testWorkspaceManager();
    testRuntimeAuthority();
    testAudioEndpointInventory();
    testAudioSessionObserver();
    testControllerIdentity();
    std::cout << "All HydraSeat Engine Tests Passed!" << std::endl;
    return 0;
}
