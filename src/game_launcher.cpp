#include "hydra/game_launcher.hpp"

#include <array>
#include <iostream>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <cwchar>
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

bool containsNul(const std::wstring& value) noexcept {
    return value.find(L'\0') != std::wstring::npos;
}

bool keyMatches(std::wstring_view entry, std::wstring_view key) noexcept {
    const auto equals = entry.find(L'=');
    if (equals == std::wstring_view::npos || equals != key.size()) return false;
    return _wcsnicmp(entry.data(), key.data(), key.size()) == 0;
}

std::optional<std::vector<wchar_t>> xinputEnvironmentBlock(
    const controller::VirtualXInputMapping& mapping,
    const std::wstring& pipeEndpoint) {
    if (!mapping.valid() || mapping.source.sourceGeneration == 0 ||
        pipeEndpoint.empty() || containsNul(pipeEndpoint)) {
        return std::nullopt;
    }

    const std::array<std::pair<std::wstring, std::wstring>, 4> overrides{{
        {L"HYDRA_XINPUT_PIPE", pipeEndpoint},
        {L"HYDRA_XINPUT_SEAT_ID", std::to_wstring(mapping.seatId)},
        {L"HYDRA_XINPUT_ACTIVATION_GENERATION", std::to_wstring(mapping.activationGeneration)},
        {L"HYDRA_XINPUT_SOURCE_GENERATION", std::to_wstring(mapping.source.sourceGeneration)},
    }};

    LPWCH rawEnvironment = GetEnvironmentStringsW();
    if (!rawEnvironment) return std::nullopt;

    std::vector<std::wstring> entries;
    for (const wchar_t* cursor = rawEnvironment; *cursor != L'\0';) {
        std::wstring entry(cursor);
        entries.push_back(entry);
        cursor += entry.size() + 1u;
    }
    FreeEnvironmentStringsW(rawEnvironment);

    for (const auto& [key, value] : overrides) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const std::wstring& entry) {
                                         return keyMatches(entry, key);
                                     }),
                      entries.end());
        entries.push_back(key + L"=" + value);
    }

    std::sort(entries.begin(), entries.end(), [](const std::wstring& left,
                                                  const std::wstring& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });

    std::vector<wchar_t> block;
    std::size_t characterCount = 1u;
    for (const auto& entry : entries) characterCount += entry.size() + 1u;
    block.reserve(characterCount);
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
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
    return launchGameForWorkspaceImpl(game, workspace, nullptr, nullptr, nullptr);
}

bool GameLauncher::launchGameForWorkspace(
    const GameProfile& game,
    const WorkspaceConfig& workspace,
    const controller::SeatBinding& controllerBinding,
    const controller::InventorySnapshot& inventory,
    std::wstring xinputPipeEndpoint) {
    return launchGameForWorkspaceImpl(
        game, workspace, &controllerBinding, &inventory, &xinputPipeEndpoint);
}

bool GameLauncher::launchGameForWorkspaceImpl(
    const GameProfile& game,
    const WorkspaceConfig& workspace,
    const controller::SeatBinding* controllerBinding,
    const controller::InventorySnapshot* inventory,
    const std::wstring* xinputPipeEndpoint) {
#ifdef _WIN32
    const auto index = seatIndex(workspace.workspaceId);
    if (!controller_ || !index || game.executablePath.empty() || seatProcesses_[*index]) {
        return false;
    }

    const bool wantsXInput =
        controllerBinding != nullptr || inventory != nullptr || xinputPipeEndpoint != nullptr;
    if (wantsXInput &&
        (!controllerBinding || !inventory || !xinputPipeEndpoint ||
         controllerBinding->seatId != workspace.workspaceId ||
         controllerBinding->api != controller::ApiSurface::XInput ||
         controllerBinding->sourceGeneration == 0 ||
         xinputPipeEndpoint->empty() || containsNul(*xinputPipeEndpoint))) {
        return false;
    }

    const auto token = controller_->beginSeatActivation(workspace.workspaceId);
    if (!token.valid()) return false;

    bool activationOwned = true;
    const auto endActivation = [&]() noexcept {
        if (activationOwned) {
            (void)controller_->endSeatActivation(token);
            activationOwned = false;
        }
    };

    std::optional<std::vector<wchar_t>> environment;
    DWORD creationFlags = CREATE_SUSPENDED;
    if (wantsXInput) {
        if (!controller_->bindController(token, *controllerBinding, *inventory)) {
            endActivation();
            return false;
        }
        const auto mapping = controller_->virtualXInputMapping(token);
        if (!mapping) {
            endActivation();
            return false;
        }
        environment = xinputEnvironmentBlock(*mapping, *xinputPipeEndpoint);
        if (!environment) {
            endActivation();
            return false;
        }
        creationFlags |= CREATE_UNICODE_ENVIRONMENT;
    }

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
        creationFlags,
        environment ? environment->data() : nullptr,
        workingDirectory,
        &startup,
        &processInfo);

    if (!created) {
        endActivation();
        return false;
    }

    HANDLE job = createStrictSeatJob();
    if (!job) {
        terminateCreatedProcess(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        endActivation();
        return false;
    }

    if (!AssignProcessToJobObject(job, processInfo.hProcess)) {
        terminateCreatedProcess(processInfo.hProcess);
        CloseHandle(job);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        endActivation();
        return false;
    }

    seatProcesses_[*index] = SeatProcess{
        fromNativeHandle(processInfo.hProcess),
        fromNativeHandle(job),
        token,
        {}};
    activationOwned = false;

    const auto rollback = [&]() {
        if (processInfo.hThread) {
            CloseHandle(processInfo.hThread);
            processInfo.hThread = nullptr;
        }
        (void)stopWorkspaceGame(workspace.workspaceId);
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
    (void)controllerBinding;
    (void)inventory;
    (void)xinputPipeEndpoint;
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
