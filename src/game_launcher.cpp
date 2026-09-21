#include "hydra/game_launcher.hpp"

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hydra {
namespace {

#ifdef _WIN32
HANDLE toNativeHandle(std::uintptr_t value) noexcept {
    return reinterpret_cast<HANDLE>(value);
}

std::uintptr_t fromNativeHandle(HANDLE handle) noexcept {
    return reinterpret_cast<std::uintptr_t>(handle);
}

runtime::ProcessIdentity readProcessIdentity(HANDLE process, DWORD pid) noexcept {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return {};

    const std::uint64_t creationIdentity =
        (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) |
        static_cast<std::uint64_t>(creation.dwLowDateTime);
    return {static_cast<std::uint32_t>(pid), creationIdentity};
}

std::wstring commandLineFor(const GameProfile& game) {
    std::wstring commandLine = L"\"" + game.executablePath + L"\"";
    if (!game.launchArguments.empty()) {
        commandLine += L" ";
        commandLine += game.launchArguments;
    }
    return commandLine;
}

HANDLE createStrictSeatJob() noexcept {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

bool queryActiveProcessCount(HANDLE job, DWORD& activeProcesses) noexcept {
    if (!job || job == INVALID_HANDLE_VALUE) return false;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(
            job,
            JobObjectBasicAccountingInformation,
            &accounting,
            sizeof(accounting),
            nullptr)) {
        return false;
    }
    activeProcesses = accounting.ActiveProcesses;
    return true;
}

bool waitForJobEmpty(HANDLE job, DWORD timeoutMs) noexcept {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD activeProcesses = 0;
        if (!queryActiveProcessCount(job, activeProcesses)) return false;
        if (activeProcesses == 0) return true;

        if (GetTickCount64() - start >= timeoutMs) return false;
        Sleep(5);
    }
}

void terminateCreatedProcess(HANDLE process) noexcept {
    if (!process || process == INVALID_HANDLE_VALUE) return;
    if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
        (void)TerminateProcess(process, ERROR_CANCELLED);
        (void)WaitForSingleObject(process, 5000);
    }
}
#endif

} // namespace

GameLauncher::~GameLauncher() {
#ifdef _WIN32
    for (std::uint32_t seatId = 1; seatId <= 2; ++seatId) {
        const auto index = seatIndex(seatId);
        if (!index || !seatProcesses_[*index]) continue;
        if (stopWorkspaceGame(seatId)) continue;

        // Destruction cannot report cleanup failure. Closing a strict Job Object
        // still kills its assigned tree, but we deliberately do not mark the
        // runtime Idle because safe-state verification did not complete.
        HANDLE job = toNativeHandle(seatProcesses_[*index]->jobHandle);
        HANDLE process = toNativeHandle(seatProcesses_[*index]->processHandle);
        if (job && job != INVALID_HANDLE_VALUE) {
            CloseHandle(job);
        }
        if (process && process != INVALID_HANDLE_VALUE) {
            CloseHandle(process);
        }
        seatProcesses_[*index].reset();
    }
#endif
}

std::optional<std::size_t> GameLauncher::seatIndex(std::uint32_t workspaceId) noexcept {
    if (workspaceId == 1) return std::size_t{0};
    if (workspaceId == 2) return std::size_t{1};
    return std::nullopt;
}

bool GameLauncher::launchGameForWorkspace(const GameProfile& game,
                                          const WorkspaceConfig& workspace) {
#ifdef _WIN32
    const auto index = seatIndex(workspace.workspaceId);
    if (!controller_ || !index || game.executablePath.empty() || seatProcesses_[*index]) {
        return false;
    }

    const auto token = controller_->beginSeatActivation(workspace.workspaceId);
    if (!token.valid()) return false;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = commandLineFor(game);
    const wchar_t* workingDirectory =
        game.workingDirectory.empty() ? nullptr : game.workingDirectory.c_str();

    const BOOL created = CreateProcessW(
        game.executablePath.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_SUSPENDED,
        nullptr,
        workingDirectory,
        &startup,
        &processInfo);

    if (!created) {
        controller_->endSeatActivation(token);
        return false;
    }

    HANDLE job = createStrictSeatJob();
    if (!job) {
        terminateCreatedProcess(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        controller_->endSeatActivation(token);
        return false;
    }

    if (!AssignProcessToJobObject(job, processInfo.hProcess)) {
        terminateCreatedProcess(processInfo.hProcess);
        CloseHandle(job);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        controller_->endSeatActivation(token);
        return false;
    }

    seatProcesses_[*index] = SeatProcess{
        fromNativeHandle(processInfo.hProcess),
        fromNativeHandle(job),
        token,
        {}};

    const auto rollback = [&]() {
        if (processInfo.hThread) {
            CloseHandle(processInfo.hThread);
            processInfo.hThread = nullptr;
        }
        stopWorkspaceGame(workspace.workspaceId);
    };

    const auto identity =
        readProcessIdentity(processInfo.hProcess, processInfo.dwProcessId);
    if (!identity.valid()) {
        rollback();
        return false;
    }

    seatProcesses_[*index]->identity = identity;
    if (!controller_->publishProcess(token, identity)) {
        rollback();
        return false;
    }

    if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
        rollback();
        return false;
    }

    CloseHandle(processInfo.hThread);
    processInfo.hThread = nullptr;

    std::wcout << L"[GameLauncher] Started " << game.title
               << L" for Seat #" << workspace.workspaceId
               << L" (PID: " << processInfo.dwProcessId << L")\n";
    return true;
#else
    (void)game;
    (void)workspace;
    return false;
#endif
}

bool GameLauncher::stopWorkspaceGame(std::uint32_t workspaceId) {
#ifdef _WIN32
    const auto index = seatIndex(workspaceId);
    if (!controller_ || !index || !seatProcesses_[*index]) return false;

    auto& session = *seatProcesses_[*index];
    HANDLE process = toNativeHandle(session.processHandle);
    HANDLE job = toNativeHandle(session.jobHandle);
    if (!process || process == INVALID_HANDLE_VALUE ||
        !job || job == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD activeProcesses = 0;
    if (!queryActiveProcessCount(job, activeProcesses)) return false;
    if (activeProcesses != 0 && !TerminateJobObject(job, ERROR_CANCELLED)) {
        return false;
    }

    if (!waitForJobEmpty(job, 5000)) return false;
    if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) return false;

    if (!controller_->endSeatActivation(session.token)) {
        return false;
    }

    CloseHandle(process);
    CloseHandle(job);
    seatProcesses_[*index].reset();
    return true;
#else
    (void)workspaceId;
    return false;
#endif
}

} // namespace hydra
