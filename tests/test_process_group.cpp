#include "hydra/process_launcher.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace hydra::process;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

#ifdef _WIN32

std::filesystem::path executableDirectory() {
    std::wstring buffer(32768u, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path childExecutable() {
    return executableDirectory() / L"hydra_process_tree_child.exe";
}

std::filesystem::path tempCaseDirectory(const wchar_t* name) {
    const auto directory = std::filesystem::temp_directory_path() /
        (std::wstring(L"hydraseat-p4-proc-") + name + L"-" +
         std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    return directory;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

std::vector<std::uint32_t> readPids(const std::filesystem::path& path) {
    std::ifstream stream(path);
    std::vector<std::uint32_t> result;
    std::uint64_t value = 0;
    while (stream >> value) {
        if (value != 0 && value <= 0xffffffffull) {
            result.push_back(static_cast<std::uint32_t>(value));
        }
    }
    return result;
}

std::uint64_t creationTime100ns(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user) == FALSE) return 0;
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return value.QuadPart;
}

struct FixtureIdentity {
    std::uint32_t processId{0};
    std::uint64_t creationTime{0};
};

FixtureIdentity captureFixtureIdentity(std::uint32_t processId) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, processId);
    if (process == nullptr) return {};
    FixtureIdentity identity{processId, creationTime100ns(process)};
    CloseHandle(process);
    return identity;
}

bool processAlive(const FixtureIdentity& identity) {
    if (identity.processId == 0 || identity.creationTime == 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, identity.processId);
    if (process == nullptr) return false;
    const bool same = creationTime100ns(process) == identity.creationTime;
    const bool alive = same && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

void terminateFixtureProcess(const FixtureIdentity& identity) {
    if (identity.processId == 0 || identity.creationTime == 0) return;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                                     PROCESS_TERMINATE,
                                 FALSE, identity.processId);
    if (process == nullptr) return;
    if (creationTime100ns(process) == identity.creationTime &&
        WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
        (void)TerminateProcess(process, 0x50345453u); // "P4TS"
        (void)WaitForSingleObject(process, 2000u);
    }
    CloseHandle(process);
}

ProcessLaunchSpec baseSpec(hydra::SeatId seatId) {
    ProcessLaunchSpec spec;
    spec.seatId = seatId;
    spec.executablePath = childExecutable().wstring();
    spec.workingDirectory = executableDirectory().wstring();
    spec.architecture = sizeof(void*) == 8u
        ? ProcessArchitecture::X64 : ProcessArchitecture::X86;
    return spec;
}

void testParentChildGrandchildAndRootExit() {
    const auto directory = tempCaseDirectory(L"tree");
    const auto pidFile = directory / L"pids.txt";
    auto spec = baseSpec(11);
    spec.arguments = {L"--depth", L"2", L"--sleep-ms", L"800",
                      L"--descendant-sleep-ms", L"4000",
                      L"--pid-file", pidFile.wstring()};

    std::string error;
    auto launched = ProcessLauncher::launch(spec, &error);
    check(launched.group != nullptr, "full Job Object process group launches");
    if (!launched.group) return;
    check(launched.group->capability() == ChildTrackingCapability::FullJobObject,
          "strict launch reports full Job Object capability");
    check(launched.root.valid(), "root process has PID plus creation identity");
    check(launched.group->configureTrustedHandoffExecutables(
              {launched.root.executablePath}, &error),
          "trusted executable evidence binds to the exact launch root");
    const auto rootAuthority = launched.group->handoffSnapshot();
    check(rootAuthority.state == ProcessHandoffState::RootActive &&
              rootAuthority.handoffGeneration == 0u &&
              rootAuthority.authoritativeProcess.sameInstance(launched.root),
          "surviving launcher remains the exact authoritative root");

    const bool trackedThree = waitUntil([&] {
        return launched.group->snapshot().processes.size() >= 3u;
    }, std::chrono::milliseconds(1500));
    check(trackedThree, "parent, child, and grandchild are tracked by completion events");

    const auto initial = launched.group->snapshot();
    std::set<std::uint32_t> ids;
    bool childOfRoot = false;
    bool grandchild = false;
    for (const auto& record : initial.processes) {
        ids.insert(record.identity.processId);
        if (!record.root && record.parentProcessId == launched.root.processId) childOfRoot = true;
    }
    for (const auto& record : initial.processes) {
        if (!record.root && record.parentProcessId != launched.root.processId &&
            ids.contains(record.parentProcessId)) {
            grandchild = true;
        }
    }
    check(ids.size() == initial.processes.size(), "tracked process PIDs are unique while alive");
    check(childOfRoot && grandchild, "process snapshot preserves parent/child/grandchild topology");
    const bool exactParents = std::all_of(
        initial.processes.begin(), initial.processes.end(),
        [&](const ProcessRecord& record) {
            if (record.root) return true;
            if (!record.parentIdentityVerified) return false;
            return std::any_of(initial.processes.begin(), initial.processes.end(),
                               [&](const ProcessRecord& parent) {
                                   return parent.identity.sameInstance(record.parentIdentity);
                               });
        });
    check(exactParents, "every tracked descendant binds to an exact owned parent identity");

    const bool rootExitedFirst = waitUntil([&] {
        const auto snapshot = launched.group->snapshot();
        bool rootExited = false;
        for (const auto& record : snapshot.processes) {
            if (record.root) rootExited = record.exited;
        }
        return rootExited && snapshot.runningCount() >= 1u;
    }, std::chrono::milliseconds(1800));
    check(rootExitedFirst, "root exit does not lose still-running descendants");
    const bool handedOff = waitUntil([&] {
        const auto handoff = launched.group->handoffSnapshot();
        return handoff.state == ProcessHandoffState::DescendantActive &&
               handoff.handoffGeneration >= 1u &&
               !handoff.authoritativeProcess.sameInstance(launched.root);
    }, std::chrono::milliseconds(2000));
    check(handedOff, "launcher exit promotes only the unique exact owned descendant branch");

    ProcessStopPolicy stop;
    stop.gracefulTimeoutMs = 20;
    stop.forcedTimeoutMs = 2500;
    check(launched.group->stop(stop, &error), "owned descendant tree force-cleans after graceful timeout");
    const auto cleaned = launched.group->snapshot();
    check(launched.group->waitForEmpty(0u) && cleaned.runningCount() == 0u,
          "cleanup proves Job active-process count and tracked orphan count are zero");
    check(cleaned.trackingComplete,
          "bounded process tracking remained complete for the normal handoff tree");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void testTwoLevelLauncherLoaderGameHandoff() {
    const auto directory = tempCaseDirectory(L"two-level-handoff");
    const auto pidFile = directory / L"pids.txt";
    auto spec = baseSpec(18);
    spec.arguments = {L"--depth", L"2", L"--sleep-ms", L"5000",
                      L"--exit-after-spawn", L"--pid-file", pidFile.wstring()};

    std::string error;
    auto launched = ProcessLauncher::launch(spec, &error);
    check(launched.group != nullptr, "two-level launcher fixture starts in a strict Job Object");
    if (!launched.group) return;
    check(launched.group->configureTrustedHandoffExecutables(
              {launched.root.executablePath}, &error),
          "two-level handoff uses trusted executable evidence plus exact Job lineage");

    check(waitUntil([&] { return readPids(pidFile).size() >= 3u; },
                    std::chrono::milliseconds(2000)),
          "launcher, loader, and final game all report exact PIDs");
    const bool promotedThroughTwoLevels = waitUntil([&] {
        const auto handoff = launched.group->handoffSnapshot();
        return handoff.state == ProcessHandoffState::DescendantActive &&
               handoff.handoffGeneration >= 1u;
    }, std::chrono::milliseconds(2500));
    check(promotedThroughTwoLevels,
          "launcher to loader to game reaches a trusted exact-lineage descendant");

    const auto pids = readPids(pidFile);
    FixtureIdentity leaf;
    if (pids.size() >= 3u) leaf = captureFixtureIdentity(pids.back());
    const auto handoff = launched.group->handoffSnapshot();
    check(leaf.processId != 0 &&
              handoff.authoritativeProcess.processId == leaf.processId &&
              handoff.authoritativeProcess.creationTime100ns == leaf.creationTime,
          "final surviving game process becomes the exact authoritative owned descendant");
    check(launched.group->ownsExactIdentity(handoff.authoritativeProcess),
          "authoritative handoff identity is present in the exact Seat-owned tree");

    ProcessStopPolicy stop;
    stop.gracefulTimeoutMs = 20;
    stop.forcedTimeoutMs = 2500;
    check(launched.group->stop(stop, &error) && launched.group->waitForEmpty(0u),
          "cleanup after two-level handoff proves orphan=0 in the owned Job");
    check(leaf.processId == 0 || !processAlive(leaf),
          "final exact game instance is gone after handoff cleanup");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void testExactOwnershipIsolationAndPidReuse() {
    auto firstSpec = baseSpec(19);
    auto secondSpec = baseSpec(20);
    firstSpec.arguments = {L"--depth", L"0", L"--sleep-ms", L"5000"};
    secondSpec.arguments = firstSpec.arguments;

    std::string firstError;
    std::string secondError;
    auto first = ProcessLauncher::launch(firstSpec, &firstError);
    auto second = ProcessLauncher::launch(secondSpec, &secondError);
    check(first.group != nullptr && second.group != nullptr,
          "two independent Seats can launch the same executable fixture");
    if (!first.group || !second.group) {
        ProcessStopPolicy cleanup;
        if (first.group) (void)first.group->stop(cleanup, &firstError);
        if (second.group) (void)second.group->stop(cleanup, &secondError);
        return;
    }

    check(first.root.executablePath == second.root.executablePath,
          "cross-Seat rejection is tested with identical executable path/name");
    check(first.group->ownsExactIdentity(first.root) &&
              !first.group->ownsExactIdentity(second.root) &&
              !second.group->ownsExactIdentity(first.root),
          "same executable path never crosses private Seat Job ownership");

    auto reusedIdentity = first.root;
    ++reusedIdentity.creationTime100ns;
    check(!first.group->ownsExactIdentity(reusedIdentity),
          "same PID with a different creation identity is rejected as stale/PID reuse");

    const auto secondProbe = captureFixtureIdentity(second.root.processId);
    ProcessStopPolicy stop;
    stop.gracefulTimeoutMs = 20;
    stop.forcedTimeoutMs = 2000;
    check(first.group->stop(stop, &firstError), "Seat A exact tree stops independently");
    check(processAlive(secondProbe), "Seat A cleanup does not disturb Seat B process ownership");
    check(second.group->stop(stop, &secondError), "Seat B exact tree stops independently");
}

void testUntrustedHelperIsNeverAuthority() {
    const auto directory = tempCaseDirectory(L"untrusted-helper");
    const auto pidFile = directory / L"pids.txt";
    const auto helperPidFile = directory / L"helper-pids.txt";
    auto spec = baseSpec(22);
    spec.arguments = {L"--depth", L"1", L"--sleep-ms", L"5000",
                      L"--exit-after-spawn", L"--spawn-untrusted-helper",
                      L"--pid-file", pidFile.wstring(),
                      L"--helper-pid-file", helperPidFile.wstring()};

    std::string error;
    auto launched = ProcessLauncher::launch(spec, &error);
    check(launched.group != nullptr, "untrusted-helper fixture launches in the owned Job");
    if (!launched.group) return;
    check(launched.group->configureTrustedHandoffExecutables(
              {launched.root.executablePath}, &error),
          "untrusted-helper test binds only the controlled game executable as trusted");

    const bool childrenReported = waitUntil([&] {
        return readPids(pidFile).size() >= 2u && readPids(helperPidFile).size() >= 1u;
    }, std::chrono::milliseconds(2000));
    check(childrenReported, "trusted child and untrusted helper both become live descendants");

    const auto gamePids = readPids(pidFile);
    const auto helperPids = readPids(helperPidFile);
    const std::uint32_t trustedChildPid = gamePids.size() >= 2u ? gamePids[1] : 0u;
    const std::uint32_t helperPid = helperPids.empty() ? 0u : helperPids.front();
    const auto helperIdentity = captureFixtureIdentity(helperPid);

    const bool promotedTrustedChild = waitUntil([&] {
        const auto handoff = launched.group->handoffSnapshot();
        return handoff.state == ProcessHandoffState::DescendantActive &&
               handoff.authoritativeProcess.processId == trustedChildPid;
    }, std::chrono::milliseconds(2500));
    check(promotedTrustedChild,
          "only the trusted exact descendant becomes authority when an untrusted helper is also owned");

    const auto snapshot = launched.group->snapshot();
    check(helperPid != 0u &&
              std::any_of(snapshot.processes.begin(), snapshot.processes.end(),
                          [&](const ProcessRecord& record) {
                              return record.identity.processId == helperPid;
                          }),
          "untrusted helper remains bounded cleanup ownership inside the Seat Job");
    const auto handoff = launched.group->handoffSnapshot();
    check(helperPid == 0u || handoff.authoritativeProcess.processId != helperPid,
          "untrusted helper PID is never promoted to game authority");

    ProcessStopPolicy stop;
    stop.gracefulTimeoutMs = 20;
    stop.forcedTimeoutMs = 2500;
    check(launched.group->stop(stop, &error) && launched.group->waitForEmpty(0u),
          "helper-containing owned tree cleans to verified orphan=0");
    check(!processAlive(helperIdentity),
          "cleanup terminates the owned untrusted helper without treating it as authority");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void testAmbiguousTrustedFrontierFailsClosed() {
    const auto directory = tempCaseDirectory(L"ambiguous-frontier");
    const auto pidFile = directory / L"pids.txt";
    auto spec = baseSpec(23);
    spec.arguments = {L"--depth", L"1", L"--sleep-ms", L"5000",
                      L"--child-count", L"2", L"--exit-after-spawn",
                      L"--pid-file", pidFile.wstring()};

    std::string error;
    auto launched = ProcessLauncher::launch(spec, &error);
    check(launched.group != nullptr, "ambiguous-frontier fixture launches in the owned Job");
    if (!launched.group) return;
    check(launched.group->configureTrustedHandoffExecutables(
              {launched.root.executablePath}, &error),
          "ambiguous-frontier test binds exact trusted executable evidence");
    check(waitUntil([&] { return readPids(pidFile).size() >= 3u; },
                    std::chrono::milliseconds(2000)),
          "launcher reports two simultaneously eligible trusted children");

    const bool rejected = waitUntil([&] {
        return launched.group->handoffSnapshot().state == ProcessHandoffState::Unverifiable;
    }, std::chrono::milliseconds(2500));
    check(rejected, "multiple trusted frontier descendants fail closed as unverifiable");
    const auto handoff = launched.group->handoffSnapshot();
    check(handoff.authoritativeProcess.sameInstance(launched.root),
          "ambiguous handoff never silently replaces the previous exact authority");

    std::vector<FixtureIdentity> descendants;
    const auto pids = readPids(pidFile);
    for (const auto pid : pids) {
        if (pid != launched.root.processId) descendants.push_back(captureFixtureIdentity(pid));
    }
    ProcessStopPolicy stop;
    stop.gracefulTimeoutMs = 20;
    stop.forcedTimeoutMs = 2500;
    check(launched.group->stop(stop, &error) && launched.group->waitForEmpty(0u),
          "ambiguous authority still retains exact cleanup ownership to orphan=0");
    check(std::none_of(descendants.begin(), descendants.end(), processAlive),
          "all ambiguous owned descendants are gone after fail-closed cleanup");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void testStopDuringHandoffPending() {
    const auto directory = tempCaseDirectory(L"handoff-pending-stop");
    const auto helperPidFile = directory / L"helper-pids.txt";
    auto spec = baseSpec(24);
    spec.arguments = {L"--depth", L"0", L"--sleep-ms", L"5000",
                      L"--spawn-untrusted-helper", L"--exit-after-spawn",
                      L"--helper-pid-file", helperPidFile.wstring()};

    std::string error;
    auto launched = ProcessLauncher::launch(spec, &error);
    check(launched.group != nullptr, "handoff-pending stop fixture launches");
    if (!launched.group) return;
    auto futureGamePath = launched.root.executablePath;
    futureGamePath += L".future-game";
    check(launched.group->configureTrustedHandoffExecutables(
              {launched.root.executablePath, futureGamePath}, &error),
          "handoff-pending fixture binds current launcher plus not-yet-created trusted game evidence");

    check(waitUntil([&] { return !readPids(helperPidFile).empty(); },
                    std::chrono::milliseconds(1500)),
          "untrusted owned helper is live while the trusted final process is absent");
    const auto helperPids = readPids(helperPidFile);
    const auto helperIdentity = helperPids.empty()
        ? FixtureIdentity{} : captureFixtureIdentity(helperPids.front());
    check(waitUntil([&] {
              return launched.group->handoffSnapshot().state ==
                     ProcessHandoffState::HandoffPending;
          }, std::chrono::milliseconds(2000)),
          "launcher exit enters explicit bounded handoff-pending instead of guessing the helper");

    ProcessStopPolicy stop;
    stop.gracefulTimeoutMs = 20;
    stop.forcedTimeoutMs = 2500;
    check(launched.group->stop(stop, &error) && launched.group->waitForEmpty(0u),
          "stop during handoff-pending terminates only the exact owned Job and verifies empty");
    check(!processAlive(helperIdentity),
          "handoff-pending cleanup leaves no owned helper orphan");
    check(launched.group->snapshot().runningCount() == 0u,
          "tracked running owned process count is zero after pending-handoff stop");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void testGameExitAfterHandoff() {
    const auto directory = tempCaseDirectory(L"handoff-exit");
    const auto pidFile = directory / L"pids.txt";
    auto spec = baseSpec(21);
    spec.arguments = {L"--depth", L"1", L"--sleep-ms", L"1000",
                      L"--exit-after-spawn", L"--pid-file", pidFile.wstring()};

    std::string error;
    auto launched = ProcessLauncher::launch(spec, &error);
    check(launched.group != nullptr, "handoff-exit fixture launches");
    if (!launched.group) return;
    check(launched.group->configureTrustedHandoffExecutables(
              {launched.root.executablePath}, &error),
          "handoff-exit fixture binds trusted executable evidence");

    ProcessIdentity promoted;
    const bool descendantActive = waitUntil([&] {
        const auto handoff = launched.group->handoffSnapshot();
        if (handoff.state == ProcessHandoffState::DescendantActive &&
            handoff.handoffGeneration >= 1u) {
            promoted = handoff.authoritativeProcess;
            return true;
        }
        return false;
    }, std::chrono::milliseconds(1800));
    check(descendantActive, "legitimate child becomes authoritative after launcher exits");

    const bool treeExited = waitUntil([&] {
        return launched.group->handoffSnapshot().state == ProcessHandoffState::TreeExited;
    }, std::chrono::milliseconds(2500));
    check(treeExited, "final game exit is distinguished from PID reuse and handoff failure");
    const auto finalHandoff = launched.group->handoffSnapshot();
    check(!promoted.valid() || finalHandoff.authoritativeProcess.sameInstance(promoted),
          "untrusted helper descendants never replace the final game authority on exit");
    check(launched.group->waitForEmpty(0u),
          "naturally exited handoff tree is verifiably empty without adopting another process");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void testStrictBreakawayIsRejected() {
    const auto directory = tempCaseDirectory(L"strict-breakaway");
    const auto pidFile = directory / L"pids.txt";
    auto spec = baseSpec(12);
    spec.arguments = {L"--depth", L"1", L"--sleep-ms", L"300",
                      L"--descendant-sleep-ms", L"2500", L"--breakaway-child",
                      L"--pid-file", pidFile.wstring()};

    std::string error;
    auto launched = ProcessLauncher::launch(spec, &error);
    check(launched.group != nullptr, "strict breakaway fixture root launches");
    if (!launched.group) return;
    check(launched.group->waitForEmpty(1800u),
          "strict Job Object root exits without an escaped child");
    const auto pids = readPids(pidFile);
    check(pids.size() == 1u && pids.front() == launched.root.processId,
          "CREATE_BREAKAWAY_FROM_JOB is rejected when capability is strict");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void testAllowedBreakawayIsExplicitAndUnowned() {
    const auto directory = tempCaseDirectory(L"allowed-breakaway");
    const auto pidFile = directory / L"pids.txt";
    auto spec = baseSpec(13);
    spec.containment = ProcessContainmentPolicy::AllowBreakawayChildren;
    spec.arguments = {L"--depth", L"1", L"--sleep-ms", L"300",
                      L"--descendant-sleep-ms", L"3000", L"--breakaway-child",
                      L"--pid-file", pidFile.wstring()};

    std::string error;
    auto launched = ProcessLauncher::launch(spec, &error);
    check(launched.group != nullptr, "breakaway-capable process group launches");
    if (!launched.group) return;
    check(launched.group->capability() == ChildTrackingCapability::JobObjectBreakawayAllowed,
          "weaker breakaway tracking capability is explicit");

    const bool childReported = waitUntil([&] { return readPids(pidFile).size() >= 2u; },
                                         std::chrono::milliseconds(1200));
    check(childReported, "breakaway child fixture reports its independent PID");
    const auto pids = readPids(pidFile);
    std::uint32_t escapedPid = 0;
    for (const auto pid : pids) {
        if (pid != launched.root.processId) escapedPid = pid;
    }
    check(escapedPid != 0, "breakaway child PID differs from owned root");
    check(launched.group->waitForEmpty(1800u),
          "Job Object becomes empty after root exits while breakaway child is external");
    const auto escapedIdentity = captureFixtureIdentity(escapedPid);
    check(processAlive(escapedIdentity),
          "Seat process group does not terminate an explicitly unowned breakaway child");
    check(launched.group->snapshot().processes.size() == 1u,
          "escaped child is not forged into owned process snapshot");

    terminateFixtureProcess(escapedIdentity);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void testExplicitRootOnlyCapability() {
    const auto directory = tempCaseDirectory(L"root-only");
    const auto pidFile = directory / L"pids.txt";
    auto spec = baseSpec(14);
    spec.containment = ProcessContainmentPolicy::RootOnly;
    spec.arguments = {L"--depth", L"1", L"--sleep-ms", L"250",
                      L"--descendant-sleep-ms", L"3000",
                      L"--pid-file", pidFile.wstring()};

    std::string error;
    auto launched = ProcessLauncher::launch(spec, &error);
    check(launched.group != nullptr, "explicit root-only process launches");
    if (!launched.group) return;
    check(launched.group->capability() == ChildTrackingCapability::RootOnly,
          "profiles can explicitly declare root-only ownership capability");
    const bool childReported = waitUntil([&] { return readPids(pidFile).size() >= 2u; },
                                         std::chrono::milliseconds(1200));
    check(childReported, "root-only fixture creates an untracked descendant");
    const auto pids = readPids(pidFile);
    std::uint32_t childPid = 0;
    for (const auto pid : pids) {
        if (pid != launched.root.processId) childPid = pid;
    }
    check(launched.group->waitForEmpty(1800u), "root-only group tracks exact root exit");
    check(launched.group->snapshot().processes.size() == 1u,
          "root-only snapshot never claims descendant ownership");
    const auto childIdentity = captureFixtureIdentity(childPid);
    check(processAlive(childIdentity),
          "root-only cleanup leaves unowned descendant untouched");
    terminateFixtureProcess(childIdentity);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void testGracefulWindowStop() {
    auto spec = baseSpec(15);
    spec.arguments = {L"--depth", L"0", L"--sleep-ms", L"5000", L"--window"};

    std::string error;
    auto launched = ProcessLauncher::launch(spec, &error);
    check(launched.group != nullptr, "windowed owned process launches for graceful stop");
    if (!launched.group) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    ProcessStopPolicy stop;
    stop.gracefulTimeoutMs = 2000;
    stop.forcedTimeoutMs = 1000;
    stop.forceTerminate = false;
    check(launched.group->stop(stop, &error),
          "exact-identity WM_CLOSE performs graceful owned-process stop");
    check(launched.group->snapshot().runningCount() == 0u,
          "graceful stop leaves no owned process running");
}

void testEnvironmentArchitectureAndForcedTermination() {
    auto spec = baseSpec(16);
    spec.environmentOverrides = {{L"HYDRA_PROCESS_TEST", L"present"}};
    spec.arguments = {L"--depth", L"0", L"--sleep-ms", L"5000",
                      L"--require-env", L"HYDRA_PROCESS_TEST", L"present"};

    std::string error;
    auto launched = ProcessLauncher::launch(spec, &error);
    check(launched.group != nullptr, "explicit environment and architecture launch succeeds");
    if (!launched.group) return;
    check(waitUntil([&] { return launched.group->snapshot().runningCount() == 1u; },
                    std::chrono::milliseconds(500)),
          "root process is observable before forced stop");

    ProcessStopPolicy gracefulOnly;
    gracefulOnly.gracefulTimeoutMs = 25;
    gracefulOnly.forcedTimeoutMs = 25;
    gracefulOnly.forceTerminate = false;
    error.clear();
    check(!launched.group->stop(gracefulOnly, &error) &&
              launched.group->snapshot().runningCount() == 1u,
          "graceful timeout without force permission preserves the owned process");

    ProcessStopPolicy stop;
    stop.gracefulTimeoutMs = 25;
    stop.forcedTimeoutMs = 2000;
    stop.forcedExitCode = 0x13572468u;
    check(launched.group->stop(stop, &error),
          "graceful timeout escalates only the owned process group when force is allowed");
    const auto stopped = launched.group->snapshot();
    check(stopped.runningCount() == 0u, "forced policy leaves no owned process running");
    bool sawForcedCode = false;
    for (const auto& record : stopped.processes) {
        if (record.root && record.exited && record.exitCode == stop.forcedExitCode) {
            sawForcedCode = true;
        }
    }
    check(sawForcedCode, "forced termination exit code is retained in deterministic snapshot");

    auto mismatch = baseSpec(17);
    mismatch.architecture = sizeof(void*) == 8u
        ? ProcessArchitecture::X86 : ProcessArchitecture::X64;
    mismatch.arguments = {L"--sleep-ms", L"5000"};
    error.clear();
    auto rejected = ProcessLauncher::launch(mismatch, &error);
    check(rejected.group == nullptr && !error.empty(),
          "architecture mismatch is rejected before target resume");
}

#endif

void testPidReuseIdentityGuard() {
    ProcessIdentity oldProcess;
    oldProcess.processId = 4242;
    oldProcess.creationTime100ns = 100;
    oldProcess.executablePath = L"old.exe";
    ProcessIdentity reusedPid = oldProcess;
    reusedPid.creationTime100ns = 200;
    reusedPid.executablePath = L"new.exe";
    check(!oldProcess.sameInstance(reusedPid),
          "same PID with a different creation time is never the same process instance");
    check(oldProcess.sameInstance(oldProcess),
          "exact PID plus creation identity matches itself");
}

} // namespace

int main() {
    testPidReuseIdentityGuard();
#ifdef _WIN32
    testParentChildGrandchildAndRootExit();
    testTwoLevelLauncherLoaderGameHandoff();
    testExactOwnershipIsolationAndPidReuse();
    testUntrustedHelperIsNeverAuthority();
    testAmbiguousTrustedFrontierFailsClosed();
    testStopDuringHandoffPending();
    testGameExitAfterHandoff();
    testStrictBreakawayIsRejected();
    testAllowedBreakawayIsExplicitAndUnowned();
    testExplicitRootOnlyCapability();
    testGracefulWindowStop();
    testEnvironmentArchitectureAndForcedTermination();
#else
    std::cout << "Process group integration tests are Windows-only.\n";
#endif

    if (failures != 0) {
        std::cerr << failures << " process group test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Process group tests passed.\n";
    return EXIT_SUCCESS;
}
