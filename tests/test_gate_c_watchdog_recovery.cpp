#include "hydra/crash_journal.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::wstring quote(std::wstring_view value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(ch);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::filesystem::path modulePath() {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    check(length != 0 && length < buffer.size(), "test module path resolves");
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::filesystem::path sibling(std::wstring_view name) {
    auto path = modulePath().parent_path();
    path /= std::wstring(name);
    return path;
}

class Process {
public:
    Process() = default;
    Process(HANDLE handle, DWORD pid) : m_handle(handle), m_pid(pid) {}
    ~Process() { close(); }
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)),
          m_pid(std::exchange(other.m_pid, 0)) {}
    Process& operator=(Process&& other) noexcept {
        if (this == &other) return *this;
        close();
        m_handle = std::exchange(other.m_handle, nullptr);
        m_pid = std::exchange(other.m_pid, 0);
        return *this;
    }

    bool valid() const noexcept { return m_handle != nullptr; }
    DWORD pid() const noexcept { return m_pid; }
    HANDLE handle() const noexcept { return m_handle; }

    bool wait(DWORD timeoutMs, DWORD* exitCode = nullptr) const {
        if (!valid() || WaitForSingleObject(m_handle, timeoutMs) != WAIT_OBJECT_0) {
            return false;
        }
        DWORD code = 0;
        if (GetExitCodeProcess(m_handle, &code) == FALSE) return false;
        if (exitCode != nullptr) *exitCode = code;
        return true;
    }

    bool terminate(DWORD exitCode) const {
        if (!valid()) return false;
        return TerminateProcess(m_handle, exitCode) != FALSE;
    }

private:
    void close() noexcept {
        if (m_handle != nullptr) CloseHandle(m_handle);
        m_handle = nullptr;
        m_pid = 0;
    }

    HANDLE m_handle{nullptr};
    DWORD m_pid{0};
};

Process launch(const std::filesystem::path& executable,
               std::wstring arguments) {
    std::wstring command = quote(executable.wstring());
    if (!arguments.empty()) {
        command.push_back(L' ');
        command += arguments;
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &startup, &process);
    if (created == FALSE) return {};
    CloseHandle(process.hThread);
    return Process(process.hProcess, process.dwProcessId);
}

Process launchCaptured(const std::filesystem::path& executable,
                       std::wstring arguments,
                       const std::filesystem::path& outputPath) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    const HANDLE output = CreateFileW(
        outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return {};
    const HANDLE input = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        CloseHandle(output);
        return {};
    }

    std::wstring command = quote(executable.wstring());
    if (!arguments.empty()) {
        command.push_back(L' ');
        command += arguments;
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = output;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &startup, &process);
    CloseHandle(input);
    CloseHandle(output);
    if (created == FALSE) return {};
    CloseHandle(process.hThread);
    return Process(process.hProcess, process.dwProcessId);
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

std::filesystem::path uniqueRecoveryDir(std::string_view label) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("hydraseat-p3-rec-" + std::string(label) + "-" +
         std::to_string(nonce));
}

std::wstring recoveryArgs(std::string_view scenario,
                          const std::filesystem::path& directory) {
    std::wstring scenarioWide(scenario.begin(), scenario.end());
    return L"--recovery-self-test " + scenarioWide +
        L" --recovery-dir " + quote(directory.wstring());
}

void cleanupDirectory(const std::filesystem::path& directory) {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void runScenario(std::string_view scenario,
                 const std::filesystem::path& directory) {
    cleanupDirectory(directory);
    const auto host = sibling(L"hydra_gate_c_host.exe");
    auto process = launch(host, recoveryArgs(scenario, directory));
    check(process.valid(), "recovery scenario host launches");
    DWORD exitCode = 0;
    check(process.wait(30'000, &exitCode) && exitCode == 0,
          std::string("recovery scenario passes: ") + std::string(scenario));
}

std::vector<DWORD> directChildren(DWORD parentPid) {
    std::vector<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            const std::wstring_view image(entry.szExeFile);
            if (entry.th32ParentProcessID == parentPid &&
                (image == L"hydra_watchdog.exe" ||
                 image == L"hydra_gate_c_target.exe")) {
                result.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return result;
}

std::vector<HANDLE> openChildren(const std::vector<DWORD>& pids) {
    std::vector<HANDLE> result;
    result.reserve(pids.size());
    for (DWORD pid : pids) {
        HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                    FALSE, pid);
        if (handle != nullptr) result.push_back(handle);
    }
    return result;
}

void closeHandles(std::vector<HANDLE>& handles) {
    for (HANDLE handle : handles) CloseHandle(handle);
    handles.clear();
}

void testCleanAndRepeatedReset() {
    const auto directory = uniqueRecoveryDir("clean");
    runScenario("clean", directory);
    // A second complete activation on the same clean journal proves generation
    // advancement and repeated stop/reset semantics are safe and idempotent.
    const auto host = sibling(L"hydra_gate_c_host.exe");
    auto second = launch(host, recoveryArgs("clean", directory));
    check(second.valid(), "second clean recovery cycle launches");
    DWORD exitCode = 0;
    check(second.wait(30'000, &exitCode) && exitCode == 0,
          "second clean recovery cycle passes");
    hydra::recovery::NativeCrashJournalStorage storage(directory);
    hydra::recovery::CrashJournalStore store(storage);
    const auto current = store.loadCurrent();
    check(current.has_value() &&
              current->phase == hydra::recovery::CrashJournalPhase::Clean &&
              current->runtimeGeneration == 2,
          "repeated clean cycle advances runtime generation exactly once");
    cleanupDirectory(directory);
}

void testFaultScenarios() {
    for (std::string_view scenario : {
             "lease-stall", "target-killed", "watchdog-killed",
             "pipe-disconnect", "adapter-failure", "shim-abnormal-exit", "console-logoff",
             "console-shutdown"}) {
        const auto directory = uniqueRecoveryDir(scenario);
        runScenario(scenario, directory);
        cleanupDirectory(directory);
    }
}

void testUiDeathDoesNotOwnRecovery() {
    const auto directory = uniqueRecoveryDir("ui");
    cleanupDirectory(directory);
    const auto hostPath = sibling(L"hydra_gate_c_host.exe");
    auto host = launch(hostPath, recoveryArgs("ui-killed", directory));
    check(host.valid(), "UI-independence recovery host launches");
    auto surrogate = launch(modulePath(), L"--ui-surrogate");
    check(surrogate.valid(), "UI surrogate launches independently");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    check(surrogate.terminate(0x55494b4cu), "UI surrogate can be killed");
    check(surrogate.wait(5'000), "UI surrogate exits");
    DWORD hostExit = 0;
    check(host.wait(30'000, &hostExit) && hostExit == 0,
          "killing non-authoritative UI does not interrupt host recovery lease");
    cleanupDirectory(directory);
}

void testStaleJournalBlocksStartup() {
    const auto directory = uniqueRecoveryDir("stale");
    runScenario("stale-journal", directory);
    hydra::recovery::NativeCrashJournalStorage storage(directory);
    hydra::recovery::CrashJournalStore store(storage);
    check(store.loadSafeMode().has_value(),
          "stale journal self-test leaves durable safe-mode evidence");
    auto blocked = launch(sibling(L"hydra_gate_c_host.exe"),
                          recoveryArgs("clean", directory));
    check(blocked.valid(), "blocked startup process launches for preflight");
    DWORD exitCode = 0;
    check(blocked.wait(10'000, &exitCode) && exitCode != 0,
          "active safe-mode marker blocks a new Gate C activation");
    cleanupDirectory(directory);
}

void testEmergencyResetStopsLiveGateCHost() {
    const auto directory = uniqueRecoveryDir("live-reset");
    cleanupDirectory(directory);
    auto host = launch(sibling(L"hydra_gate_c_host.exe"),
                       recoveryArgs("wait-host-kill", directory));
    check(host.valid(), "live-reset Gate C host launches");

    hydra::recovery::NativeCrashJournalStorage storage(directory);
    hydra::recovery::CrashJournalStore store(storage);
    std::vector<DWORD> children;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(15);
    bool active = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto journal = store.loadCurrent();
        active = journal.has_value() &&
            journal->phase == hydra::recovery::CrashJournalPhase::Active;
        children = directChildren(host.pid());
        if (active && children.size() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(active && children.size() >= 2,
          "live-reset host reaches active state with watchdog and targets");
    auto childHandles = openChildren(children);
    check(childHandles.size() == children.size(),
          "live-reset observes every exact guarded child handle");

    std::wstring resetArguments = L"all --confirm --recovery-dir \"";
    resetArguments += directory.wstring();
    resetArguments += L"\" --json";
    const auto resetOutput = directory / L"live-reset.jsonl";
    auto reset = launchCaptured(sibling(L"hydra_reset.exe"), resetArguments,
                                resetOutput);
    check(reset.valid(), "independent hydra_reset launches while Gate C host is active");
    DWORD resetExit = 0;
    const bool resetWaited = reset.wait(20'000, &resetExit);
    if (!resetWaited || resetExit != 0) {
        std::cerr << "[RESET] " << readText(resetOutput) << '\n';
    }
    check(resetWaited && resetExit == 0,
          std::string("hydra_reset recovers an active Gate C session without UI participation; waited=") +
              (resetWaited ? "true" : "false") +
              " exit=" + std::to_string(resetExit));
    check(host.wait(5'000), "live Gate C owner exits during emergency reset");
    for (HANDLE child : childHandles) {
        check(WaitForSingleObject(child, 10'000) == WAIT_OBJECT_0,
              "live emergency reset leaves no watchdog/target orphan");
    }
    closeHandles(childHandles);

    const auto current = store.loadCurrent();
    check(current.has_value() &&
              current->phase == hydra::recovery::CrashJournalPhase::Clean &&
              current->finalResult ==
                  hydra::recovery::CrashJournalFinalResult::Clean,
          "live emergency reset persists verified clean journal evidence");
    check(!store.loadSafeMode().has_value(),
          "live emergency reset leaves no safe-mode marker after verified cleanup");
    check(!std::filesystem::exists(directory / L"reset-runtime.bin"),
          "live emergency reset removes its exact runtime registration");
    cleanupDirectory(directory);
}

void testHostDeathLeavesNoOrphanAndEntersSafeMode() {
    const auto directory = uniqueRecoveryDir("hostkill");
    cleanupDirectory(directory);
    auto host = launch(sibling(L"hydra_gate_c_host.exe"),
                       recoveryArgs("wait-host-kill", directory));
    check(host.valid(), "host-kill recovery fixture launches");

    hydra::recovery::NativeCrashJournalStorage storage(directory);
    hydra::recovery::CrashJournalStore store(storage);
    std::vector<DWORD> children;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(15);
    bool active = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto journal = store.loadCurrent();
        active = journal.has_value() &&
            journal->phase == hydra::recovery::CrashJournalPhase::Active;
        children = directChildren(host.pid());
        if (active && children.size() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(active && children.size() >= 2,
          "guarded host reaches active state with watchdog and two targets");
    auto childHandles = openChildren(children);
    check(childHandles.size() == children.size(),
          "every guarded child has an observable exact process handle");

    check(host.terminate(0x484b494cu), "test kills Gate C host abruptly");
    check(host.wait(5'000), "killed Gate C host exits");
    for (HANDLE child : childHandles) {
        check(WaitForSingleObject(child, 10'000) == WAIT_OBJECT_0,
              "watchdog leaves no guarded child/helper orphan after host death");
    }
    closeHandles(childHandles);

    const auto assessment = store.assessStartupAndEnterSafeMode();
    check(assessment.state ==
              hydra::recovery::StartupRecoveryState::RecoverableIncomplete &&
              assessment.safeMode.has_value() &&
              assessment.safeMode->reason ==
                  hydra::recovery::SafeModeReason::IncompleteSession,
          "host death remains durable incomplete evidence and enters safe mode");
    auto blocked = launch(sibling(L"hydra_gate_c_host.exe"),
                          recoveryArgs("clean", directory));
    check(blocked.valid(), "post-crash blocked startup launches");
    DWORD blockedExit = 0;
    check(blocked.wait(10'000, &blockedExit) && blockedExit != 0,
          "post-crash safe mode blocks automatic reactivation");

    std::wstring resetArguments = L"all --confirm --recovery-dir \"";
    resetArguments += directory.wstring();
    resetArguments += L"\" --json";
    auto reset = launch(sibling(L"hydra_reset.exe"), resetArguments);
    check(reset.valid(), "independent hydra_reset launches after Gate C host death");
    DWORD resetExit = 0;
    check(reset.wait(20'000, &resetExit) && resetExit == 0,
          "hydra_reset verifies and closes the stale Gate C recovery state");
    const auto resetCurrent = store.loadCurrent();
    check(resetCurrent.has_value() &&
              resetCurrent->phase == hydra::recovery::CrashJournalPhase::Clean &&
              resetCurrent->finalResult ==
                  hydra::recovery::CrashJournalFinalResult::Clean,
          "hydra_reset replaces incomplete Gate C evidence with verified clean evidence");
    check(!store.loadSafeMode().has_value(),
          "hydra_reset clears the correlated post-crash safe-mode marker");
    check(!std::filesystem::exists(directory / L"reset-runtime.bin"),
          "hydra_reset removes the exact runtime reset registration after success");

    auto resumed = launch(sibling(L"hydra_gate_c_host.exe"),
                          recoveryArgs("clean", directory));
    check(resumed.valid(), "Gate C relaunches after verified emergency reset");
    DWORD resumedExit = 0;
    check(resumed.wait(30'000, &resumedExit) && resumedExit == 0,
          "Gate C clean activation succeeds after emergency reset");
    const auto resumedCurrent = store.loadCurrent();
    check(resumedCurrent.has_value() &&
              resumedCurrent->phase == hydra::recovery::CrashJournalPhase::Clean &&
              resumedCurrent->runtimeGeneration == 2,
          "post-reset Gate C activation advances runtime generation exactly once");
    cleanupDirectory(directory);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--ui-surrogate") {
        Sleep(10'000);
        return EXIT_SUCCESS;
    }

    check(std::filesystem::exists(sibling(L"hydra_gate_c_host.exe")),
          "Gate C recovery host is staged beside process test");
    check(std::filesystem::exists(sibling(L"hydra_watchdog.exe")),
          "watchdog is staged beside Gate C recovery host");
    check(std::filesystem::exists(sibling(L"hydra_gate_c_target.exe")),
          "controlled target is staged beside recovery host");
    check(std::filesystem::exists(sibling(L"hydra_gate_c_api_probe.exe")),
          "API probe is staged beside recovery host");
    check(std::filesystem::exists(sibling(L"hydra_gate_c_shim.dll")),
          "controlled shim is staged beside recovery host");

    testCleanAndRepeatedReset();
    testFaultScenarios();
    testUiDeathDoesNotOwnRecovery();
    testStaleJournalBlocksStartup();
    testEmergencyResetStopsLiveGateCHost();
    testHostDeathLeavesNoOrphanAndEntersSafeMode();

    std::cout << "Gate C watchdog recovery process tests passed.\n";
    return EXIT_SUCCESS;
}
