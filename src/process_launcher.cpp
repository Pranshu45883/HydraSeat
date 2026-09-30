#include "hydra/process_launcher.hpp"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hydra::process {
namespace {

#ifdef _WIN32

struct ProcessHandles {
    PROCESS_INFORMATION value{};
    ~ProcessHandles() {
        if (value.hThread != nullptr) CloseHandle(value.hThread);
        if (value.hProcess != nullptr) CloseHandle(value.hProcess);
    }
};

std::string win32Error(const char* prefix, DWORD code = GetLastError()) {
    return std::string(prefix) + " (Win32=" + std::to_string(code) + ")";
}

std::wstring quoteArgument(std::wstring_view argument) {
    if (argument.empty()) return L"\"\"";
    const bool needsQuotes = argument.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needsQuotes) return std::wstring(argument);

    std::wstring result;
    result.push_back(L'\"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'\"') {
            result.append(backslashes * 2u + 1u, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2u, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring commandLineFor(const ProcessLaunchSpec& spec) {
    std::wstring command = quoteArgument(spec.executablePath);
    for (const auto& argument : spec.arguments) {
        command.push_back(L' ');
        command += quoteArgument(argument);
    }
    return command;
}

bool keyMatches(std::wstring_view entry, std::wstring_view key) {
    const auto equals = entry.find(L'=');
    if (equals == std::wstring_view::npos || equals != key.size()) return false;
    return _wcsnicmp(entry.data(), key.data(), key.size()) == 0;
}

std::vector<wchar_t> environmentBlockFor(const ProcessLaunchSpec& spec) {
    if (spec.environmentOverrides.empty()) return {};

    std::vector<std::wstring> entries;
    LPWCH raw = GetEnvironmentStringsW();
    if (raw == nullptr) return {};
    for (const wchar_t* cursor = raw; *cursor != L'\0';) {
        std::wstring entry(cursor);
        entries.push_back(entry);
        cursor += entry.size() + 1u;
    }
    FreeEnvironmentStringsW(raw);

    for (const auto& [key, value] : spec.environmentOverrides) {
        if (key.empty() || key.find(L'=') != std::wstring::npos ||
            key.find(L'\0') != std::wstring::npos || value.find(L'\0') != std::wstring::npos) {
            return {};
        }
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
    std::size_t chars = 1u;
    for (const auto& entry : entries) chars += entry.size() + 1u;
    block.reserve(chars);
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

bool matchesArchitecture(HANDLE process, ProcessArchitecture expected,
                         std::string* error) {
    if (expected == ProcessArchitecture::Any) return true;

    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto function = reinterpret_cast<IsWow64Process2Fn>(
        kernel != nullptr ? GetProcAddress(kernel, "IsWow64Process2") : nullptr);
    if (function != nullptr) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (function(process, &processMachine, &nativeMachine) == FALSE) {
            if (error) *error = win32Error("IsWow64Process2 failed");
            return false;
        }
        const bool x86 = processMachine == IMAGE_FILE_MACHINE_I386;
        const bool x64 = processMachine == IMAGE_FILE_MACHINE_AMD64 ||
                         (processMachine == IMAGE_FILE_MACHINE_UNKNOWN &&
                          nativeMachine == IMAGE_FILE_MACHINE_AMD64);
        if ((expected == ProcessArchitecture::X86 && x86) ||
            (expected == ProcessArchitecture::X64 && x64)) {
            return true;
        }
        if (error) *error = "launched process architecture does not match profile policy";
        return false;
    }

    BOOL wow64 = FALSE;
    if (IsWow64Process(process, &wow64) == FALSE) {
        if (error) *error = win32Error("IsWow64Process failed");
        return false;
    }
    const bool x86 = wow64 != FALSE;
    const bool x64 = wow64 == FALSE && sizeof(void*) == 8u;
    if ((expected == ProcessArchitecture::X86 && x86) ||
        (expected == ProcessArchitecture::X64 && x64)) {
        return true;
    }
    if (error) *error = "launched process architecture cannot satisfy profile policy";
    return false;
}

#endif

} // namespace

ProcessLaunchResult ProcessLauncher::launch(const ProcessLaunchSpec& spec,
                                            std::string* error) {
    ProcessLaunchResult result;
#ifdef _WIN32
    if (spec.seatId == 0) {
        if (error) *error = "process launch requires a nonzero Seat identifier";
        return result;
    }
    if (spec.executablePath.empty() ||
        spec.executablePath.find(L'\0') != std::wstring::npos) {
        if (error) *error = "process launch executable path is empty or contains NUL";
        return result;
    }
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(std::filesystem::path(spec.executablePath),
                                          filesystemError) || filesystemError) {
        if (error) *error = "process launch executable does not exist or is not a file";
        return result;
    }
    if (!spec.workingDirectory.empty()) {
        filesystemError.clear();
        if (spec.workingDirectory.find(L'\0') != std::wstring::npos ||
            !std::filesystem::is_directory(std::filesystem::path(spec.workingDirectory),
                                           filesystemError) || filesystemError) {
            if (error) *error = "process launch working directory is invalid";
            return result;
        }
    }
    for (const auto& argument : spec.arguments) {
        if (argument.find(L'\0') != std::wstring::npos) {
            if (error) *error = "process launch argument contains NUL";
            return result;
        }
    }

    std::vector<wchar_t> environment;
    if (!spec.environmentOverrides.empty()) {
        environment = environmentBlockFor(spec);
        if (environment.empty()) {
            if (error) *error = "process launch environment override is invalid";
            return result;
        }
    }

    std::wstring command = commandLineFor(spec);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    ProcessHandles handles;
    DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    if (spec.createNewConsole) flags |= CREATE_NEW_CONSOLE;
    const wchar_t* workingDirectory = spec.workingDirectory.empty()
        ? nullptr : spec.workingDirectory.c_str();
    void* environmentPointer = environment.empty() ? nullptr : environment.data();

    if (CreateProcessW(spec.executablePath.c_str(), mutableCommand.data(),
                       nullptr, nullptr, FALSE, flags, environmentPointer,
                       workingDirectory, &startup, &handles.value) == FALSE) {
        if (error) *error = win32Error("CreateProcessW failed");
        return result;
    }

    if (!matchesArchitecture(handles.value.hProcess, spec.architecture, error)) {
        (void)TerminateProcess(handles.value.hProcess, 0x48594152u); // "HYAR"
        (void)WaitForSingleObject(handles.value.hProcess, 2000u);
        return result;
    }

    const bool allowBreakaway =
        spec.containment == ProcessContainmentPolicy::AllowBreakawayChildren;
    const bool allowRootOnly =
        spec.containment == ProcessContainmentPolicy::AllowRootOnlyFallback;
    const bool forceRootOnly =
        spec.containment == ProcessContainmentPolicy::RootOnly;
    auto group = SeatProcessGroup::adoptLaunchedProcess(
        spec.seatId, reinterpret_cast<std::uintptr_t>(handles.value.hProcess),
        handles.value.dwProcessId, allowBreakaway, allowRootOnly, forceRootOnly,
        error);
    if (!group) {
        (void)TerminateProcess(handles.value.hProcess, 0x4859434Eu); // "HYCN"
        (void)WaitForSingleObject(handles.value.hProcess, 2000u);
        return result;
    }
    handles.value.hProcess = nullptr; // SeatProcessGroup now owns the exact root handle.

    if (ResumeThread(handles.value.hThread) == static_cast<DWORD>(-1)) {
        if (error) *error = win32Error("ResumeThread failed");
        ProcessStopPolicy cleanup;
        cleanup.gracefulTimeoutMs = 0;
        cleanup.forcedTimeoutMs = 2000;
        (void)group->stop(cleanup, nullptr);
        return result;
    }

    result.root = group->rootIdentity();
    result.group = std::move(group);
    return result;
#else
    (void)spec;
    if (error) *error = "process launching is Windows-only";
    return result;
#endif
}

} // namespace hydra::process
