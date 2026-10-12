#include "hydra/host_transport.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

#if defined(_WIN32)
std::optional<std::filesystem::path> seatConfigPath(std::string* error) {
    std::array<wchar_t, 32768> localAppData{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        localAppData.data(),
        static_cast<DWORD>(localAppData.size()));
    if (length == 0 || length >= localAppData.size()) {
        if (error) {
            *error =
                "LOCALAPPDATA is unavailable; refusing non-durable host startup";
        }
        return std::nullopt;
    }

    auto path = std::filesystem::path(
        std::wstring(localAppData.data(), length));
    path /= L"HydraSeat";
    path /= L"seat-config.json";
    return path;
}
#endif

} // namespace

int main() {
#if defined(_WIN32)
    // There must be exactly one runtime authority per interactive Windows
    // session. Multiple named-pipe server processes with the same pipe name
    // would otherwise create split-brain host state.
    DWORD sessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
        std::cerr << "hydra_host failed: unable to resolve Windows session\n";
        return 1;
    }

    const std::wstring mutexName =
        L"Local\\HydraSeat.Host.v3." + std::to_wstring(sessionId);
    HANDLE instanceMutex =
        CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (instanceMutex == nullptr) {
        std::cerr << "hydra_host failed: unable to create instance mutex (win32="
                  << GetLastError() << ")\n";
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex);
        return 0;
    }

    struct InstanceMutexGuard {
        HANDLE handle;
        ~InstanceMutexGuard() {
            if (handle != nullptr) {
                ReleaseMutex(handle);
                CloseHandle(handle);
            }
        }
    } instanceGuard{instanceMutex};

    std::string error;
    const auto configPath = seatConfigPath(&error);
    if (!configPath) {
        std::cerr << "hydra_host failed: " << error << '\n';
        return 1;
    }

    hydra::runtime::RuntimeHost host(*configPath);
    if (!host.loadPersistentSeatHardware(&error)) {
        std::cerr
            << "hydra_host refused an invalid persisted Seat configuration: "
            << error << '\n';
        return 1;
    }

    hydra::hostipc::HostPipeServer server(host);
    if (!server.serve(&error)) {
        std::cerr << "hydra_host failed: " << error << '\n';
        return 1;
    }
    return 0;
#else
    std::cerr << "hydra_host is supported only on Windows.\n";
    return 2;
#endif
}
