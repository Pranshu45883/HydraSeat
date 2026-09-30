#include "hydra/xinput_abi_probe.hpp"

#if defined(_WIN32)
#include <windows.h>

#define XInputGetState HydraSystemXInputGetStateDeclaration
#define XInputSetState HydraSystemXInputSetStateDeclaration
#define XInputGetCapabilities HydraSystemXInputGetCapabilitiesDeclaration
#include <Xinput.h>
#undef XInputGetCapabilities
#undef XInputSetState
#undef XInputGetState

#include <iostream>
#include <string>
#include <vector>

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using GetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

struct AdapterApi {
    HMODULE module{nullptr};
    GetStateFn getState{nullptr};
    SetStateFn setState{nullptr};
    GetCapabilitiesFn getCapabilities{nullptr};

    ~AdapterApi() {
        if (module != nullptr) FreeLibrary(module);
    }

    AdapterApi(const AdapterApi&) = delete;
    AdapterApi& operator=(const AdapterApi&) = delete;
    AdapterApi() = default;
};

bool loadAdapter(const std::wstring& path, AdapterApi& api) noexcept {
    api.module = LoadLibraryW(path.c_str());
    if (api.module == nullptr) return false;

    api.getState = reinterpret_cast<GetStateFn>(
        GetProcAddress(api.module, "XInputGetState"));
    api.setState = reinterpret_cast<SetStateFn>(
        GetProcAddress(api.module, "XInputSetState"));
    api.getCapabilities = reinterpret_cast<GetCapabilitiesFn>(
        GetProcAddress(api.module, "XInputGetCapabilities"));
    return api.getState != nullptr && api.setState != nullptr &&
           api.getCapabilities != nullptr;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetErrorMode(SEM_FAILCRITICALERRORS |
                 SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);

    std::vector<std::wstring> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0u);
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    const auto options = hydra::controller::abi_probe::parseProbeArgs(args);
    if (!options) return 2;

    AdapterApi api;
    if (!loadAdapter(options->dllPath, api)) return 3;

    if (options->mode == hydra::controller::abi_probe::ProbeMode::Snapshot) {
        XINPUT_CAPABILITIES capabilities{};
        const DWORD capabilityStatus = api.getCapabilities(0, 0, &capabilities);
        std::cout << hydra::controller::abi_probe::formatCapabilitiesLine(
                         capabilityStatus, capabilities.Type, capabilities.SubType)
                  << '\n';
        if (capabilityStatus != ERROR_SUCCESS) {
            std::cout.flush();
            return std::cout.good() ? 4 : 9;
        }

        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            XINPUT_STATE state{};
            const DWORD status = api.getState(slot, &state);
            std::cout << hydra::controller::abi_probe::formatStateLine(
                             slot, status, state.dwPacketNumber,
                             state.Gamepad.wButtons, state.Gamepad.sThumbLX)
                      << '\n';
            if (slot == 0) {
                if (status != ERROR_SUCCESS) {
                    std::cout.flush();
                    return std::cout.good() ? 5 : 9;
                }
            } else if (status != ERROR_DEVICE_NOT_CONNECTED) {
                std::cout.flush();
                return std::cout.good() ? 6 : 9;
            }
        }
    } else {
        XINPUT_VIBRATION vibration{};
        vibration.wLeftMotorSpeed = options->lowFrequencyMotor;
        vibration.wRightMotorSpeed = options->highFrequencyMotor;
        const DWORD status = api.setState(0, &vibration);
        std::cout << hydra::controller::abi_probe::formatVibrationLine(status) << '\n';
        if (status != ERROR_SUCCESS) {
            std::cout.flush();
            return std::cout.good() ? 7 : 9;
        }
    }

    std::cout.flush();
    return std::cout.good() ? 0 : 9;
}
#else
int main() {
    return 2;
}
#endif
