#include "ui/host_bootstrap.hpp"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hydra::ui {
namespace {

void setError(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

std::optional<std::filesystem::path> currentExecutableDirectory(
    std::string* error) {
    std::vector<wchar_t> buffer(1024, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            setError(error, "GetModuleFileNameW failed while locating hydra_host.exe");
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(
                std::wstring(buffer.data(), length)).parent_path();
        }
        if (buffer.size() >= 32768) {
            setError(error, "current executable path exceeds Windows path bounds");
            return std::nullopt;
        }
        buffer.resize(buffer.size() * 2, L'\0');
    }
}

bool siblingHostExists(const std::filesystem::path& hostPath) {
    const DWORD attrs = GetFileAttributesW(hostPath.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool launchSiblingHost(std::string* error) {
    static std::mutex launchMutex;
    std::lock_guard lock(launchMutex);

    // A different UI-side caller may have launched the host while this caller
    // was waiting for the mutex. Avoid spawning a redundant process if the pipe
    // has already become reachable.
    const auto pipeName = hydra::hostipc::currentHostPipeName();
    if (!pipeName.empty() && WaitNamedPipeW(pipeName.c_str(), 250)) {
        return true;
    }

    const auto directory = currentExecutableDirectory(error);
    if (!directory) return false;

    const auto hostPath = *directory / L"hydra_host.exe";
    if (!siblingHostExists(hostPath)) {
        setError(
            error,
            "hydra_host.exe is missing beside HydraSeat.exe; installation is incomplete");
        return false;
    }

    std::wstring commandLine = L"\"" + hostPath.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    const BOOL created = CreateProcessW(
        hostPath.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        directory->c_str(),
        &startup,
        &process);
    if (!created) {
        setError(
            error,
            "failed to start sibling hydra_host.exe (win32=" +
                std::to_string(GetLastError()) + ")");
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace

bool connectCanonicalHost(
    hydra::hostipc::HostPipeClient& client,
    hydra::hostipc::ClientRole role,
    std::string* error) {
    if (client.connected()) return true;

    std::string initialError;
    if (client.connect(role, 150, &initialError)) {
        return true;
    }

    std::string launchError;
    if (!launchSiblingHost(&launchError)) {
        setError(
            error,
            launchError.empty()
                ? initialError
                : initialError + "; " + launchError);
        return false;
    }

    std::string retryError;
    if (client.connect(
            role,
            hydra::hostipc::kDefaultHostPipeTimeoutMs,
            &retryError)) {
        return true;
    }

    setError(
        error,
        retryError.empty()
            ? "hydra_host.exe started but canonical host IPC did not become ready"
            : retryError);
    return false;
}

} // namespace hydra::ui
