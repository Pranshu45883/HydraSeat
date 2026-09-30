#include <iostream>

#if defined(_WIN32)
#include <windows.h>

#define XInputGetState HydraSystemXInputGetStateDeclaration
#define XInputSetState HydraSystemXInputSetStateDeclaration
#define XInputGetCapabilities HydraSystemXInputGetCapabilitiesDeclaration
#include <Xinput.h>
#undef XInputGetCapabilities
#undef XInputSetState
#undef XInputGetState

namespace {
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using GetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
}

int wmain(int argc, wchar_t* argv[]) {
    SetErrorMode(SEM_FAILCRITICALERRORS |
                 SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);

    if (argc < 2) {
        std::cerr << "adapter DLL path missing\n";
        return 2;
    }

    HMODULE module = LoadLibraryW(argv[1]);
    if (module == nullptr) {
        std::cerr << "adapter DLL failed to load\n";
        return 4;
    }

    const auto getState = reinterpret_cast<GetStateFn>(
        GetProcAddress(module, "XInputGetState"));
    const auto setState = reinterpret_cast<SetStateFn>(
        GetProcAddress(module, "XInputSetState"));
    const auto getCapabilities = reinterpret_cast<GetCapabilitiesFn>(
        GetProcAddress(module, "XInputGetCapabilities"));
    if (getState == nullptr || setState == nullptr || getCapabilities == nullptr) {
        std::cerr << "required XInput export missing\n";
        FreeLibrary(module);
        return 5;
    }

    if (getState(0, nullptr) != ERROR_BAD_ARGUMENTS ||
        setState(0, nullptr) != ERROR_BAD_ARGUMENTS ||
        getCapabilities(0, 0, nullptr) != ERROR_BAD_ARGUMENTS) {
        std::cerr << "null pointer contract mismatch\n";
        FreeLibrary(module);
        return 6;
    }

    XINPUT_STATE state{};
    XINPUT_VIBRATION vibration{};
    XINPUT_CAPABILITIES capabilities{};
    if (getState(1, &state) != ERROR_DEVICE_NOT_CONNECTED ||
        setState(1, &vibration) != ERROR_DEVICE_NOT_CONNECTED ||
        getCapabilities(1, 0, &capabilities) != ERROR_DEVICE_NOT_CONNECTED) {
        std::cerr << "nonzero logical slot did not fail closed\n";
        FreeLibrary(module);
        return 7;
    }

    FreeLibrary(module);
    std::cout << "XInput adapter exports and fail-closed ABI verified\n";
    return 0;
}
#else
int main() {
    return 0;
}
#endif
