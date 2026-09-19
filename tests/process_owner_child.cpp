#ifdef _WIN32
#include <windows.h>

#include <cwchar>
#include <string>

namespace {

bool parseUnsigned(const wchar_t* text, DWORD& value) {
    if (!text || *text == L'\0') return false;
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (!end || *end != L'\0') return false;
    value = static_cast<DWORD>(parsed);
    return true;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    std::wstring readyEvent;
    DWORD lifetimeMs = 30000;

    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--ready-event") == 0 && i + 1 < argc) {
            readyEvent = argv[++i];
        } else if (std::wcscmp(argv[i], L"--lifetime-ms") == 0 && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], lifetimeMs)) return 2;
        } else {
            return 3;
        }
    }

    if (readyEvent.empty()) return 4;

    HANDLE eventHandle = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEvent.c_str());
    if (!eventHandle) return 5;

    const BOOL signaled = SetEvent(eventHandle);
    CloseHandle(eventHandle);
    if (!signaled) return 6;

    Sleep(lifetimeMs);
    return 0;
}
#else
int main() { return 0; }
#endif
