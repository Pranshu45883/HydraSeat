#include "hydra/game_launcher.hpp"
#include "hydra/runtime_authority.hpp"
#include "hydra/workspace_manager.hpp"

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

std::wstring makeEventName(const wchar_t* suffix) {
    return L"Local\\HydraSeatProcessOwnership-" +
           std::to_wstring(GetCurrentProcessId()) + L"-" + suffix;
}

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

std::uint64_t creationIdentity(HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return 0;
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) |
           static_cast<std::uint64_t>(creation.dwLowDateTime);
}

hydra::GameProfile childProfile(const std::wstring& helperPath,
                                const std::wstring& eventName) {
    hydra::GameProfile profile;
    profile.title = L"HydraSeat controlled process owner child";
    profile.executablePath = helperPath;
    profile.launchArguments = L"--ready-event \"" + eventName + L"\" --lifetime-ms 30000";
    return profile;
}

bool waitReady(HANDLE eventHandle) {
    return WaitForSingleObject(eventHandle, 5000) == WAIT_OBJECT_0;
}

bool processStillRunning(HANDLE process) {
    return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!check(argc == 2, "expected controlled child path")) return 2;

    const std::wstring helperPath = argv[1];
    hydra::WorkspaceConfig seat1{};
    seat1.workspaceId = 1;
    hydra::WorkspaceConfig seat2{};
    seat2.workspaceId = 2;

    hydra::runtime::SessionController controller;
    hydra::GameLauncher launcher(controller);

    const auto event1Name = makeEventName(L"seat1");
    const auto event2Name = makeEventName(L"seat2");
    ScopedHandle event1{CreateEventW(nullptr, TRUE, FALSE, event1Name.c_str())};
    ScopedHandle event2{CreateEventW(nullptr, TRUE, FALSE, event2Name.c_str())};
    if (!check(event1.value && event2.value, "create readiness events")) return 3;

    hydra::GameLauncher unboundLauncher;
    if (!check(!unboundLauncher.launchGameForWorkspace(
                   childProfile(helperPath, event1Name), seat1),
               "launcher without runtime authority fails closed")) {
        return 4;
    }

    if (!check(launcher.launchGameForWorkspace(
                   childProfile(helperPath, event1Name), seat1),
               "launch Seat 1 child")) {
        return 5;
    }
    if (!check(waitReady(event1.value), "Seat 1 child becomes ready")) return 6;
    const auto seat1Snapshot = controller.snapshot(1);
    if (!check(seat1Snapshot && seat1Snapshot->active && seat1Snapshot->process,
               "Seat 1 runtime owns launched process")) {
        return 7;
    }

    ScopedHandle seat1Process{OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, seat1Snapshot->process->pid)};
    if (!check(seat1Process.value != nullptr, "open Seat 1 process for observation")) return 8;
    if (!check(creationIdentity(seat1Process.value) ==
                   seat1Snapshot->process->creationIdentity,
               "published Seat 1 creation identity matches Windows")) {
        return 9;
    }

    if (!check(launcher.launchGameForWorkspace(
                   childProfile(helperPath, event2Name), seat2),
               "launch Seat 2 child")) {
        return 10;
    }
    if (!check(waitReady(event2.value), "Seat 2 child becomes ready")) return 11;
    const auto seat2Snapshot = controller.snapshot(2);
    if (!check(seat2Snapshot && seat2Snapshot->active && seat2Snapshot->process,
               "Seat 2 runtime owns launched process")) {
        return 12;
    }
    ScopedHandle seat2Process{OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, seat2Snapshot->process->pid)};
    if (!check(seat2Process.value != nullptr, "open Seat 2 process for observation")) return 13;
    if (!check(processStillRunning(seat2Process.value), "Seat 2 process is running")) return 14;

    if (!check(!launcher.launchGameForWorkspace(
                   childProfile(helperPath, event1Name), seat1),
               "duplicate live launch for Seat 1 is rejected")) {
        return 15;
    }
    if (!check(processStillRunning(seat1Process.value),
               "duplicate launch rejection does not disturb Seat 1")) {
        return 16;
    }

    if (!check(launcher.stopWorkspaceGame(1), "stop Seat 1 exact process")) return 17;
    if (!check(WaitForSingleObject(seat1Process.value, 5000) == WAIT_OBJECT_0,
               "Seat 1 process exits after stop")) {
        return 18;
    }
    const auto stoppedSeat1 = controller.snapshot(1);
    if (!check(stoppedSeat1 && !stoppedSeat1->active && !stoppedSeat1->process,
               "Seat 1 runtime is idle after verified stop")) {
        return 19;
    }
    if (!check(processStillRunning(seat2Process.value),
               "stopping Seat 1 leaves Seat 2 alive")) {
        return 20;
    }
    const auto liveSeat2 = controller.snapshot(2);
    if (!check(liveSeat2 && liveSeat2->active &&
                   liveSeat2->process == seat2Snapshot->process,
               "stopping Seat 1 preserves Seat 2 ownership")) {
        return 21;
    }

    hydra::GameProfile missing;
    missing.title = L"missing";
    missing.executablePath = L"Z:\\HydraSeat\\definitely-missing.exe";
    if (!check(!launcher.launchGameForWorkspace(missing, seat1),
               "invalid executable launch fails")) {
        return 22;
    }
    const auto rolledBack = controller.snapshot(1);
    if (!check(rolledBack && !rolledBack->active && !rolledBack->process,
               "failed process creation rolls Seat 1 back to idle")) {
        return 23;
    }

    if (!check(launcher.stopWorkspaceGame(2), "stop Seat 2 exact process")) return 24;
    if (!check(WaitForSingleObject(seat2Process.value, 5000) == WAIT_OBJECT_0,
               "Seat 2 process exits after stop")) {
        return 25;
    }
    if (!check(!launcher.stopWorkspaceGame(2), "second Seat 2 stop is rejected")) return 26;
    if (!check(!launcher.stopWorkspaceGame(3), "invalid Seat stop is rejected")) return 27;

    hydra::runtime::SessionController destructorController;
    ScopedHandle destructorProcess;
    {
        hydra::GameLauncher scopedLauncher(destructorController);
        const auto event3Name = makeEventName(L"destructor");
        ScopedHandle event3{CreateEventW(nullptr, TRUE, FALSE, event3Name.c_str())};
        if (!check(event3.value != nullptr, "create destructor readiness event")) return 28;
        if (!check(scopedLauncher.launchGameForWorkspace(
                       childProfile(helperPath, event3Name), seat1),
                   "launch destructor-owned child")) {
            return 29;
        }
        if (!check(waitReady(event3.value), "destructor-owned child becomes ready")) return 30;
        const auto snapshot = destructorController.snapshot(1);
        if (!check(snapshot && snapshot->active && snapshot->process,
                   "destructor test runtime owns child")) {
            return 31;
        }
        destructorProcess.value = OpenProcess(SYNCHRONIZE, FALSE, snapshot->process->pid);
        if (!check(destructorProcess.value != nullptr,
                   "open destructor-owned child for observation")) {
            return 32;
        }
    }
    if (!check(WaitForSingleObject(destructorProcess.value, 5000) == WAIT_OBJECT_0,
               "launcher destruction stops retained child")) {
        return 33;
    }
    const auto destructorSnapshot = destructorController.snapshot(1);
    if (!check(destructorSnapshot && !destructorSnapshot->active &&
                   !destructorSnapshot->process,
               "launcher destruction ends activation after verified cleanup")) {
        return 34;
    }

    std::cout << "GameLauncher process ownership test passed\n";
    return 0;
}
#else
int main() { return 0; }
#endif
