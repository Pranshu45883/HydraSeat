#ifdef _WIN32
#include <windows.h>

#include <cwchar>
#include <string>
#include <vector>

namespace {

bool parseUnsigned(const wchar_t* text, DWORD& value) {
    if (!text || *text == L'\0') return false;
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (!end || *end != L'\0') return false;
    value = static_cast<DWORD>(parsed);
    return true;
}

std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::wstring selfPath() {
    std::wstring buffer(32768u, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return buffer;
}

bool spawnDescendant(const std::wstring& readyEvent, DWORD lifetimeMs) {
    const auto executable = selfPath();
    if (executable.empty() || readyEvent.empty()) return false;

    std::wstring commandLine =
        quote(executable) + L" --ready-event " + quote(readyEvent) +
        L" --lifetime-ms " + std::to_wstring(lifetimeMs);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);
    if (!created) return false;

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    std::wstring readyEvent;
    std::wstring descendantReadyEvent;
    DWORD lifetimeMs = 30000;
    bool exitAfterSpawn = false;

    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--ready-event") == 0 && i + 1 < argc) {
            readyEvent = argv[++i];
        } else if (std::wcscmp(argv[i], L"--spawn-child-ready-event") == 0 &&
                   i + 1 < argc) {
            descendantReadyEvent = argv[++i];
        } else if (std::wcscmp(argv[i], L"--lifetime-ms") == 0 && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], lifetimeMs)) return 2;
        } else if (std::wcscmp(argv[i], L"--exit-after-spawn") == 0) {
            exitAfterSpawn = true;
        } else {
            return 3;
        }
    }

    if (readyEvent.empty()) return 4;
    if (!descendantReadyEvent.empty() &&
        !spawnDescendant(descendantReadyEvent, lifetimeMs)) {
        return 7;
    }

    HANDLE eventHandle = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEvent.c_str());
    if (!eventHandle) return 5;

    const BOOL signaled = SetEvent(eventHandle);
    CloseHandle(eventHandle);
    if (!signaled) return 6;
    if (exitAfterSpawn) {
        Sleep(250);
        return 0;
    }

    Sleep(lifetimeMs);
    return 0;
}
#else
int main() { return 0; }
#endif
