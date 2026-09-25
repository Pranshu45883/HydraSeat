#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

#ifdef _WIN32

std::wstring quoteArgument(std::wstring_view argument) {
    if (argument.empty()) return L"\"\"";
    if (argument.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'\"') {
            result.append(slashes * 2u + 1u, L'\\');
            result.push_back(L'\"');
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(ch);
        }
    }
    result.append(slashes * 2u, L'\\');
    result.push_back(L'\"');
    return result;
}

std::filesystem::path selfPath() {
    std::wstring buffer(32768u, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

void appendPid(const std::filesystem::path& path, DWORD pid) {
    if (path.empty()) return;
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const std::string line = std::to_string(pid) + "\n";
    DWORD written = 0;
    (void)WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

LRESULT CALLBACK testWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND createTestWindow() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = testWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"HydraSeatProcessTreeFixture";
    (void)RegisterClassW(&windowClass);
    return CreateWindowExW(0, windowClass.lpszClassName, L"HydraSeat process fixture",
                           WS_OVERLAPPED, 0, 0, 32, 32, nullptr, nullptr,
                           instance, nullptr);
}

bool launchDescendant(int depth, int sleepMs, const std::filesystem::path& pidFile,
                      bool breakaway, bool exitAfterSpawn) {
    const auto executable = selfPath();
    std::wstring command = quoteArgument(executable.wstring()) +
        L" --depth " + std::to_wstring(depth) +
        L" --sleep-ms " + std::to_wstring(sleepMs);
    if (!pidFile.empty()) command += L" --pid-file " + quoteArgument(pidFile.wstring());
    if (exitAfterSpawn) command += L" --exit-after-spawn";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    DWORD flags = CREATE_NO_WINDOW;
    if (breakaway) flags |= CREATE_BREAKAWAY_FROM_JOB;
    const BOOL created = CreateProcessW(executable.c_str(), mutableCommand.data(),
                                        nullptr, nullptr, FALSE, flags, nullptr,
                                        executable.parent_path().c_str(),
                                        &startup, &process);
    if (created == FALSE) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool launchUntrustedHelper(int sleepMs, const std::filesystem::path& pidFile) {
    std::wstring systemDirectory(32768u, L'\0');
    const UINT length = GetSystemDirectoryW(systemDirectory.data(),
                                            static_cast<UINT>(systemDirectory.size()));
    if (length == 0 || length >= systemDirectory.size()) return false;
    systemDirectory.resize(length);
    const auto executable = std::filesystem::path(systemDirectory) / L"ping.exe";
    const int count = std::max(2, sleepMs / 1000 + 1);
    std::wstring command = quoteArgument(executable.wstring()) +
        L" -n " + std::to_wstring(count) + L" -w 1000 127.0.0.1";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(executable.c_str(), mutableCommand.data(),
                                        nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, executable.parent_path().c_str(),
                                        &startup, &process);
    if (created == FALSE) return false;
    appendPid(pidFile, process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

#endif

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    int depth = 0;
    int sleepMs = 1000;
    int descendantSleepMs = -1;
    int childCount = 1;
    bool breakawayChild = false;
    bool exitAfterSpawn = false;
    bool spawnUntrustedHelper = false;
    bool createWindow = false;
    std::filesystem::path pidFile;
    std::filesystem::path helperPidFile;
    std::string requiredEnvironment;
    std::string requiredValue;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--depth" && index + 1 < argc) {
            depth = std::atoi(argv[++index]);
        } else if (argument == "--sleep-ms" && index + 1 < argc) {
            sleepMs = std::atoi(argv[++index]);
        } else if (argument == "--descendant-sleep-ms" && index + 1 < argc) {
            descendantSleepMs = std::atoi(argv[++index]);
        } else if (argument == "--child-count" && index + 1 < argc) {
            childCount = std::atoi(argv[++index]);
            if (childCount < 1 || childCount > 8) return 2;
        } else if (argument == "--breakaway-child") {
            breakawayChild = true;
        } else if (argument == "--exit-after-spawn") {
            exitAfterSpawn = true;
        } else if (argument == "--spawn-untrusted-helper") {
            spawnUntrustedHelper = true;
        } else if (argument == "--window") {
            createWindow = true;
        } else if (argument == "--pid-file" && index + 1 < argc) {
            pidFile = std::filesystem::path(argv[++index]);
        } else if (argument == "--helper-pid-file" && index + 1 < argc) {
            helperPidFile = std::filesystem::path(argv[++index]);
        } else if (argument == "--require-env" && index + 2 < argc) {
            requiredEnvironment = argv[++index];
            requiredValue = argv[++index];
        } else {
            return 2;
        }
    }

    if (!requiredEnvironment.empty()) {
        const DWORD required = GetEnvironmentVariableA(requiredEnvironment.c_str(), nullptr, 0);
        if (required == 0) return 3;
        std::string value(required, '\0');
        const DWORD copied = GetEnvironmentVariableA(requiredEnvironment.c_str(), value.data(), required);
        if (copied == 0 || copied >= required) return 3;
        value.resize(copied);
        if (value != requiredValue) return 4;
    }

    appendPid(pidFile, GetCurrentProcessId());
    bool spawnedAny = false;
    if (spawnUntrustedHelper) {
        if (!launchUntrustedHelper(std::max(sleepMs, 2000), helperPidFile)) return 8;
        spawnedAny = true;
    }
    if (depth > 0) {
        const int childSleep = descendantSleepMs >= 0 ? descendantSleepMs : sleepMs;
        for (int childIndex = 0; childIndex < childCount; ++childIndex) {
            if (!launchDescendant(depth - 1, childSleep, pidFile, breakawayChild,
                                  exitAfterSpawn)) {
                if (breakawayChild) {
                    // Strict Job Objects must reject explicit breakaway. Any other
                    // failure is a fixture failure, not evidence for the policy.
                    if (GetLastError() != ERROR_ACCESS_DENIED) return 7;
                    break;
                }
                return 5;
            }
            spawnedAny = true;
        }
    }
    if (exitAfterSpawn && spawnedAny) return 0;

    HWND window = nullptr;
    if (createWindow) {
        window = createTestWindow();
        if (window == nullptr) return 6;
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::max(sleepMs, 0));
    while (sleepMs > 0 && std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            if (message.message == WM_QUIT) return 0;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (window != nullptr && IsWindow(window) != FALSE) DestroyWindow(window);
    return 0;
#else
    (void)argc;
    (void)argv;
    return 0;
#endif
}
