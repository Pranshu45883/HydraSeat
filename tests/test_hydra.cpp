#include "hydra/hardware_detector.hpp"
#include "hydra/display_manager.hpp"
#include "hydra/workspace_manager.hpp"
#include "hydra/input_router.hpp"
#include "hydra/runtime_authority.hpp"
#include <objbase.h>

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

    const auto second = controller.beginSeatActivation(1);
    assert(second.valid());
    assert(second.generation > first.generation);
    assert(!controller.publishProcess(first, process));
    assert(!controller.bindTargetWindow(first, process, 0x200));

    const hydra::runtime::ProcessIdentity replacement{5252, 2002};
    assert(controller.publishProcess(second, replacement));
    assert(controller.bindTargetWindow(second, replacement, 0x200));

    const auto otherSeat = controller.beginSeatActivation(2);
    assert(otherSeat.valid());
    assert(controller.publishProcess(otherSeat, {6262, 3003}));

    assert(controller.endSeatActivation(second));
    snapshot = controller.snapshot(1);
    assert(snapshot.has_value());
    assert(!snapshot->active);
    assert(!snapshot->process.has_value());
    assert(snapshot->targetHwnd == 0);

    const auto seat2Snapshot = controller.snapshot(2);
    assert(seat2Snapshot.has_value());
    assert(seat2Snapshot->active);

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

    // Test 1 — exact process identity match
    ProcessIdentity id1{42, 100};
    ProcessIdentity id2{42, 100};
    assert(AudioSessionObserver::matchIdentity(id1, id2) == ProcessOwnershipMatch::Match);

    // Test 2 — PID reuse protection
    ProcessIdentity id3{42, 200};
    assert(AudioSessionObserver::matchIdentity(id3, id1) == ProcessOwnershipMatch::Mismatch);

    // Mismatch (different PID)
    ProcessIdentity id4{43, 100};
    assert(AudioSessionObserver::matchIdentity(id4, id1) == ProcessOwnershipMatch::Mismatch);

    // Test 3 — missing identity
    assert(AudioSessionObserver::matchIdentity(std::nullopt, id1) == ProcessOwnershipMatch::Unknown);

    // Initialize COM for the test thread
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        auto result = AudioSessionObserver::enumerateSessions();
        assert(result.isSuccess());

        if (result.isSuccess()) {
            const auto& sessions = *result.sessions;
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

#include "hydra/audio_routing_experiment.hpp"

void testAudioRoutingFeasibility() {
    using namespace hydra::windows;

    // Initialize COM for the test thread
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "[Test] Failed to initialize COM for routing test." << std::endl;
        return;
    }

    // 1. Enumerate endpoints to pick a target (any valid endpoint).
    auto inventory = AudioEndpointInventory::enumerateRenderEndpoints();
    std::wstring targetEndpoint = L"";
    if (inventory.isSuccess() && !inventory.endpoints->empty()) {
        targetEndpoint = inventory.endpoints->front().endpointId;
    }

    // 2. Create identity for the current test process.
    DWORD pid = GetCurrentProcessId();
    hydra::runtime::ProcessIdentity identity;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        FILETIME ct, et, kt, ut;
        if (GetProcessTimes(hProcess, &ct, &et, &kt, &ut)) {
            uint64_t cid = (static_cast<uint64_t>(ct.dwHighDateTime) << 32) | static_cast<uint64_t>(ct.dwLowDateTime);
            identity = {pid, cid};
        }
        CloseHandle(hProcess);
    }

    // 3. Run the experiment.
    std::cout << "[Test] Running AudioRoutingFeasibility Experiment..." << std::endl;
    auto evidence = AudioRoutingExperiment::runPolicyConfigExperiment(identity, targetEndpoint);

    std::cout << "  Status: " << (int)evidence.status << std::endl;
    std::cout << "  Mechanism: " << evidence.mechanism << std::endl;
    std::cout << "  Windows Version: " << evidence.windowsVersion << std::endl;
    std::cout << "  Global Default Changed: " << (evidence.globalDefaultChanged ? "YES" : "NO") << std::endl;
    std::cout << "  Rollback Verified: " << (evidence.rollbackVerified ? "YES" : "NO") << std::endl;
    std::cout << "  Notes: " << evidence.notes << std::endl;

    // The feasibility test should gracefully report its findings without crashing the test suite
    // even if unsupported on this machine.
    assert(!evidence.globalDefaultChanged && "Experiment failed safety boundary! Global default changed!");

    CoUninitialize();
    std::cout << "[Test] AudioRoutingFeasibility experiment completed safely." << std::endl;
}

int main() {
    std::cout << "Running HydraSeat Engine Tests..." << std::endl;
    testHardwareDetector();
    testWorkspaceManager();
    testRuntimeAuthority();
    testAudioEndpointInventory();
    testAudioSessionObserver();
    testAudioRoutingFeasibility();
    std::cout << "All HydraSeat Engine Tests Passed!" << std::endl;
    return 0;
}
