#include "hydra/game_launcher.hpp"
#include "hydra/runtime_authority.hpp"
#include "hydra/workspace_manager.hpp"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

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
                                const std::wstring& eventName,
                                const std::wstring& descendantReadyEvent = {},
                                bool exitAfterSpawn = false) {
    hydra::GameProfile profile;
    profile.title = L"HydraSeat controlled process owner child";
    profile.executablePath = helperPath;
    profile.launchArguments =
        L"--ready-event \"" + eventName + L"\" --lifetime-ms 30000";
    if (!descendantReadyEvent.empty()) {
        profile.launchArguments +=
            L" --spawn-child-ready-event \"" + descendantReadyEvent + L"\"";
    }
    if (exitAfterSpawn) profile.launchArguments += L" --exit-after-spawn";
    return profile;
}

bool waitReady(HANDLE eventHandle) {
    return WaitForSingleObject(eventHandle, 5000) == WAIT_OBJECT_0;
}

bool processStillRunning(HANDLE process) {
    return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

DWORD findDirectChild(DWORD parentPid) {
    ScopedHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) {
        snapshot.value = nullptr;
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) return 0;
    do {
        if (entry.th32ParentProcessID == parentPid) return entry.th32ProcessID;
    } while (Process32NextW(snapshot.value, &entry));
    return 0;
}

bool requireExitedOrCleanup(HANDLE process, DWORD timeoutMs, const char* message) {
    if (WaitForSingleObject(process, timeoutMs) == WAIT_OBJECT_0) return true;
    (void)TerminateProcess(process, ERROR_CANCELLED);
    (void)WaitForSingleObject(process, 5000);
    return check(false, message);
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
    const auto event1ChildName = makeEventName(L"seat1-child");
    const auto event2Name = makeEventName(L"seat2");
    ScopedHandle event1{CreateEventW(nullptr, TRUE, FALSE, event1Name.c_str())};
    ScopedHandle event1Child{
        CreateEventW(nullptr, TRUE, FALSE, event1ChildName.c_str())};
    ScopedHandle event2{CreateEventW(nullptr, TRUE, FALSE, event2Name.c_str())};
    if (!check(event1.value && event1Child.value && event2.value,
               "create readiness events")) {
        return 3;
    }

    hydra::GameLauncher unboundLauncher;
    if (!check(!unboundLauncher.launchGameForWorkspace(
                   childProfile(helperPath, event1Name), seat1),
               "launcher without runtime authority fails closed")) {
        return 4;
    }

    if (!check(launcher.launchGameForWorkspace(
                   childProfile(helperPath, event1Name, event1ChildName), seat1),
               "launch Seat 1 process tree")) {
        return 5;
    }
    if (!check(waitReady(event1.value), "Seat 1 root becomes ready")) return 6;
    if (!check(waitReady(event1Child.value), "Seat 1 descendant becomes ready")) return 7;

    const auto seat1Snapshot = controller.snapshot(1);
    if (!check(seat1Snapshot && seat1Snapshot->active && seat1Snapshot->process,
               "Seat 1 runtime owns launched root")) {
        return 8;
    }

    ScopedHandle seat1Process{OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, seat1Snapshot->process->pid)};
    if (!check(seat1Process.value != nullptr, "open Seat 1 root for observation")) return 9;
    if (!check(creationIdentity(seat1Process.value) ==
                   seat1Snapshot->process->creationIdentity,
               "published Seat 1 creation identity matches Windows")) {
        return 10;
    }

    const DWORD descendantPid = findDirectChild(seat1Snapshot->process->pid);
    if (!check(descendantPid != 0, "discover direct Seat 1 descendant")) return 11;
    ScopedHandle seat1Descendant{OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
        FALSE, descendantPid)};
    if (!check(seat1Descendant.value != nullptr, "open Seat 1 descendant")) return 12;
    if (!check(processStillRunning(seat1Descendant.value),
               "Seat 1 descendant is running")) {
        return 13;
    }

    if (!check(launcher.launchGameForWorkspace(
                   childProfile(helperPath, event2Name), seat2),
               "launch Seat 2 child")) {
        return 14;
    }
    if (!check(waitReady(event2.value), "Seat 2 child becomes ready")) return 15;
    const auto seat2Snapshot = controller.snapshot(2);
    if (!check(seat2Snapshot && seat2Snapshot->active && seat2Snapshot->process,
               "Seat 2 runtime owns launched process")) {
        return 16;
    }
    ScopedHandle seat2Process{OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, seat2Snapshot->process->pid)};
    if (!check(seat2Process.value != nullptr, "open Seat 2 process for observation")) return 17;
    if (!check(processStillRunning(seat2Process.value), "Seat 2 process is running")) return 18;

    if (!check(!launcher.launchGameForWorkspace(
                   childProfile(helperPath, event1Name), seat1),
               "duplicate live launch for Seat 1 is rejected")) {
        return 19;
    }

    if (!check(launcher.stopWorkspaceGame(1), "stop Seat 1 process group")) return 20;
    if (!check(WaitForSingleObject(seat1Process.value, 5000) == WAIT_OBJECT_0,
               "Seat 1 root exits after stop")) {
        return 21;
    }
    if (!requireExitedOrCleanup(
            seat1Descendant.value, 5000,
            "Seat 1 descendant must exit with its owned process tree")) {
        return 22;
    }

    const auto stoppedSeat1 = controller.snapshot(1);
    if (!check(stoppedSeat1 && !stoppedSeat1->active && !stoppedSeat1->process,
               "Seat 1 runtime is idle after verified tree stop")) {
        return 23;
    }
    if (!check(processStillRunning(seat2Process.value),
               "stopping Seat 1 tree leaves Seat 2 alive")) {
        return 24;
    }
    const auto liveSeat2 = controller.snapshot(2);
    if (!check(liveSeat2 && liveSeat2->active &&
                   liveSeat2->process == seat2Snapshot->process,
               "stopping Seat 1 preserves Seat 2 ownership")) {
        return 25;
    }

    hydra::GameProfile missing;
    missing.title = L"missing";
    missing.executablePath = L"Z:\\HydraSeat\\definitely-missing.exe";
    if (!check(!launcher.launchGameForWorkspace(missing, seat1),
               "invalid executable launch fails")) {
        return 26;
    }
    const auto rolledBack = controller.snapshot(1);
    if (!check(rolledBack && !rolledBack->active && !rolledBack->process,
               "failed process creation rolls Seat 1 back to idle")) {
        return 27;
    }

    if (!check(launcher.stopWorkspaceGame(2), "stop Seat 2 exact process")) return 28;
    if (!check(WaitForSingleObject(seat2Process.value, 5000) == WAIT_OBJECT_0,
               "Seat 2 process exits after stop")) {
        return 29;
    }
    if (!check(!launcher.stopWorkspaceGame(2), "second Seat 2 stop is rejected")) return 30;
    if (!check(!launcher.stopWorkspaceGame(3), "invalid Seat stop is rejected")) return 31;

    // The Job Object remains the Seat ownership boundary even after the launch
    // root exits naturally while a descendant is still running.
    const auto exitedRootEventName = makeEventName(L"root-exits");
    const auto exitedRootChildEventName = makeEventName(L"root-exits-child");
    ScopedHandle exitedRootEvent{
        CreateEventW(nullptr, TRUE, FALSE, exitedRootEventName.c_str())};
    ScopedHandle exitedRootChildEvent{
        CreateEventW(nullptr, TRUE, FALSE, exitedRootChildEventName.c_str())};
    if (!check(exitedRootEvent.value && exitedRootChildEvent.value,
               "create root-exit readiness events")) {
        return 32;
    }
    if (!check(launcher.launchGameForWorkspace(
                   childProfile(helperPath, exitedRootEventName,
                                exitedRootChildEventName, true),
                   seat1),
               "launch Seat 1 tree whose root exits first")) {
        return 33;
    }
    if (!check(waitReady(exitedRootEvent.value), "root-exit fixture root becomes ready")) return 34;
    const auto exitedRootSnapshot = controller.snapshot(1);
    if (!check(exitedRootSnapshot && exitedRootSnapshot->active &&
                   exitedRootSnapshot->process,
               "Seat 1 remains active for root-exit tree")) {
        return 35;
    }
    ScopedHandle exitedRootProcess{OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, exitedRootSnapshot->process->pid)};
    if (!check(exitedRootProcess.value != nullptr, "open root-exit launch root")) return 36;
    if (!check(waitReady(exitedRootChildEvent.value),
               "root-exit fixture descendant becomes ready")) {
        return 37;
    }
    const DWORD exitedRootChildPid = findDirectChild(exitedRootSnapshot->process->pid);
    if (!check(exitedRootChildPid != 0, "discover root-exit descendant")) return 38;
    ScopedHandle exitedRootChild{OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
        FALSE, exitedRootChildPid)};
    if (!check(exitedRootChild.value != nullptr, "open root-exit descendant")) return 39;
    if (!check(WaitForSingleObject(exitedRootProcess.value, 3000) == WAIT_OBJECT_0,
               "launch root exits before its descendant")) {
        return 40;
    }
    if (!check(processStillRunning(exitedRootChild.value),
               "descendant remains active after launch root exits")) {
        return 41;
    }
    if (!check(launcher.stopWorkspaceGame(1),
               "Seat stop terminates tree after launch root already exited")) {
        return 42;
    }
    if (!requireExitedOrCleanup(
            exitedRootChild.value, 5000,
            "remaining descendant exits when root-exit Seat tree stops")) {
        return 43;
    }
    const auto exitedTreeStopped = controller.snapshot(1);
    if (!check(exitedTreeStopped && !exitedTreeStopped->active &&
                   !exitedTreeStopped->process,
               "root-exit Seat becomes idle only after tree is empty")) {
        return 44;
    }

    hydra::runtime::SessionController destructorController;
    ScopedHandle destructorProcess;
    ScopedHandle destructorDescendant;
    {
        hydra::GameLauncher scopedLauncher(destructorController);
        const auto event3Name = makeEventName(L"destructor");
        const auto event3ChildName = makeEventName(L"destructor-child");
        ScopedHandle event3{CreateEventW(nullptr, TRUE, FALSE, event3Name.c_str())};
        ScopedHandle event3Child{
            CreateEventW(nullptr, TRUE, FALSE, event3ChildName.c_str())};
        if (!check(event3.value != nullptr && event3Child.value != nullptr,
                   "create destructor readiness events")) {
            return 45;
        }
        if (!check(scopedLauncher.launchGameForWorkspace(
                       childProfile(helperPath, event3Name, event3ChildName), seat1),
                   "launch destructor-owned process tree")) {
            return 46;
        }
        if (!check(waitReady(event3.value), "destructor-owned root becomes ready")) return 47;
        if (!check(waitReady(event3Child.value),
                   "destructor-owned descendant becomes ready")) {
            return 48;
        }
        const auto snapshot = destructorController.snapshot(1);
        if (!check(snapshot && snapshot->active && snapshot->process,
                   "destructor test runtime owns process tree")) {
            return 49;
        }
        destructorProcess.value = OpenProcess(SYNCHRONIZE, FALSE, snapshot->process->pid);
        if (!check(destructorProcess.value != nullptr,
                   "open destructor-owned root for observation")) {
            return 50;
        }
        const DWORD destructorChildPid = findDirectChild(snapshot->process->pid);
        if (!check(destructorChildPid != 0,
                   "discover destructor-owned descendant")) {
            return 51;
        }
        destructorDescendant.value = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
            FALSE, destructorChildPid);
        if (!check(destructorDescendant.value != nullptr,
                   "open destructor-owned descendant for observation")) {
            return 52;
        }
    }
    if (!check(WaitForSingleObject(destructorProcess.value, 5000) == WAIT_OBJECT_0,
               "launcher destruction stops retained root")) {
        return 53;
    }
    if (!check(WaitForSingleObject(destructorDescendant.value, 5000) == WAIT_OBJECT_0,
               "launcher destruction stops retained descendant")) {
        return 54;
    }
    const auto destructorSnapshot = destructorController.snapshot(1);
    if (!check(destructorSnapshot && !destructorSnapshot->active &&
                   !destructorSnapshot->process,
               "launcher destruction ends activation after verified tree cleanup")) {
        return 55;
    }

    std::cout << "GameLauncher process-tree ownership test passed\n";
    return 0;
}
#else
int main() { return 0; }
#endif
