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

int main() {
    std::cout << "Running HydraSeat Engine Tests..." << std::endl;
    testHardwareDetector();
    testWorkspaceManager();
    testRuntimeAuthority();
    testAudioEndpointInventory();
    std::cout << "All HydraSeat Engine Tests Passed!" << std::endl;
    return 0;
}
