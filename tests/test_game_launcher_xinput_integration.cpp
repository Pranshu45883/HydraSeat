#include "hydra/game_launcher.hpp"
#include "hydra/runtime_authority.hpp"

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

namespace {

struct ScopedHandle {
    HANDLE value{nullptr};
    ~ScopedHandle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << "\n";
        return false;
    }
    return true;
}

std::wstring makeEventName() {
    return L"Local\\HydraSeatXInputLaunch-" +
           std::to_wstring(GetCurrentProcessId());
}

hydra::GameProfile childProfile(
    const std::wstring& helperPath,
    const std::wstring& readyEvent,
    const std::wstring& pipeEndpoint,
    std::uint32_t seatId,
    std::uint64_t sourceGeneration) {
    hydra::GameProfile profile;
    profile.title = L"HydraSeat XInput launch integration child";
    profile.executablePath = helperPath;
    profile.launchArguments =
        L"--ready-event \"" + readyEvent +
        L"\" --expect-xinput-pipe \"" + pipeEndpoint +
        L"\" --expect-xinput-seat " + std::to_wstring(seatId) +
        L" --expect-xinput-source-generation " +
        std::to_wstring(sourceGeneration) +
        L" --lifetime-ms 30000";
    return profile;
}

hydra::controller::InventorySnapshot authoritativeInventory(
    std::uint64_t sourceGeneration) {
    hydra::controller::InventorySnapshot inventory;
    inventory.authoritative = true;
    inventory.sources.push_back({
        "xinput-slot:0",
        std::nullopt,
        L"Runtime XInput Slot 0",
        hydra::controller::ApiSurface::XInput,
        hydra::controller::IdentityQuality::RuntimeOnly,
        std::uint8_t{0},
        true,
        sourceGeneration});
    return inventory;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!check(argc == 2, "expected controlled child path")) return 2;

    const std::wstring helperPath = argv[1];
    const std::wstring pipeEndpoint =
        L"\\\\.\\pipe\\HydraSeat-XInput-Launch-" +
        std::to_wstring(GetCurrentProcessId());
    const std::uint64_t sourceGeneration = 7;

    hydra::WorkspaceConfig seat1{};
    seat1.workspaceId = 1;
    hydra::runtime::SessionController controller;
    hydra::GameLauncher launcher(controller);

    auto inventory = authoritativeInventory(sourceGeneration);
    const hydra::controller::SeatBinding binding{
        1,
        hydra::controller::ApiSurface::XInput,
        "xinput-slot:0",
        std::nullopt,
        std::uint8_t{0},
        sourceGeneration};

    const auto readyEventName = makeEventName();
    ScopedHandle readyEvent{
        CreateEventW(nullptr, TRUE, FALSE, readyEventName.c_str())};
    if (!check(readyEvent.value != nullptr, "create readiness event")) return 3;

    if (!check(
            launcher.launchGameForWorkspace(
                childProfile(
                    helperPath, readyEventName, pipeEndpoint, 1, sourceGeneration),
                seat1,
                binding,
                inventory,
                pipeEndpoint),
            "launch Seat with controller-derived XInput context")) {
        return 4;
    }

    if (!check(
            WaitForSingleObject(readyEvent.value, 5000) == WAIT_OBJECT_0,
            "child validates Seat-private XInput environment")) {
        return 5;
    }

    const auto snapshot = controller.snapshot(1);
    if (!check(
            snapshot && snapshot->active && snapshot->process &&
                snapshot->controllerBinding &&
                *snapshot->controllerBinding == binding,
            "runtime owns process and controller before child executes")) {
        return 6;
    }

    if (!check(launcher.stopWorkspaceGame(1), "stop XInput launch process tree")) {
        return 7;
    }

    const auto stopped = controller.snapshot(1);
    if (!check(stopped && !stopped->active && !stopped->process &&
                   !stopped->controllerBinding,
               "verified stop clears Seat runtime ownership")) {
        return 8;
    }

    if (!check(
            !launcher.launchGameForWorkspace(
                childProfile(
                    helperPath, readyEventName, pipeEndpoint, 1, sourceGeneration),
                seat1,
                binding,
                inventory,
                L""),
            "empty XInput endpoint fails closed")) {
        return 9;
    }

    auto partialInventory = inventory;
    partialInventory.authoritative = false;
    if (!check(
            !launcher.launchGameForWorkspace(
                childProfile(
                    helperPath, readyEventName, pipeEndpoint, 1, sourceGeneration),
                seat1,
                binding,
                partialInventory,
                pipeEndpoint),
            "non-authoritative controller inventory fails closed")) {
        return 10;
    }

    auto staleInventory = inventory;
    staleInventory.sources[0].sourceGeneration = sourceGeneration + 1;
    if (!check(
            !launcher.launchGameForWorkspace(
                childProfile(
                    helperPath, readyEventName, pipeEndpoint, 1, sourceGeneration),
                seat1,
                binding,
                staleInventory,
                pipeEndpoint),
            "stale controller generation fails closed")) {
        return 11;
    }

    auto wrongSeatBinding = binding;
    wrongSeatBinding.seatId = 2;
    if (!check(
            !launcher.launchGameForWorkspace(
                childProfile(
                    helperPath, readyEventName, pipeEndpoint, 1, sourceGeneration),
                seat1,
                wrongSeatBinding,
                inventory,
                pipeEndpoint),
            "cross-Seat controller binding fails closed")) {
        return 12;
    }

    return 0;
}
#else
int main() { return 0; }
#endif
