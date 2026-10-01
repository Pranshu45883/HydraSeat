#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::installer {

enum class BootstrapOperation : std::uint8_t {
    Install,
    Repair,
    Uninstall,
    Validate
};

enum class BootstrapProcessResult : std::uint8_t {
    Success,
    Cancelled,
    Failed
};

enum class BootstrapPathType : std::uint8_t {
    Missing,
    RegularFile,
    Directory,
    ReparsePoint,
    Other
};

enum class BootstrapPackageStatus : std::uint8_t {
    Valid,
    NotReleaseLayout,
    MissingRequiredPath,
    ReparsePointRejected,
    SignatureRejected,
    UnexpectedLayout
};

enum class BootstrapInstalledState : std::uint8_t {
    NotInstalled,
    Installed,
    Inconsistent
};

struct BootstrapPackageFacts {
    std::wstring architectureDirectoryName;
    BootstrapPathType packageRoot{BootstrapPathType::Missing};
    BootstrapPathType architectureDirectory{BootstrapPathType::Missing};
    BootstrapPathType setupExecutable{BootstrapPathType::Missing};
    BootstrapPathType installerScript{BootstrapPathType::Missing};
    BootstrapPathType signingProvenance{BootstrapPathType::Missing};
    BootstrapPathType signingProvenanceSignature{BootstrapPathType::Missing};
    std::vector<std::wstring> packageRootEntries;
    std::vector<std::wstring> architectureEntries;
};

struct BootstrapPackageAssessment {
    BootstrapPackageStatus status{BootstrapPackageStatus::NotReleaseLayout};
    std::wstring diagnostic;

    bool valid() const noexcept { return status == BootstrapPackageStatus::Valid; }
};

struct BootstrapPackageLayout {
    std::filesystem::path setupExecutable;
    std::filesystem::path architectureDirectory;
    std::filesystem::path packageRoot;
    std::filesystem::path installerScript;
    std::filesystem::path signingProvenance;
    std::filesystem::path signingProvenanceSignature;
};

struct BootstrapPowerShellInvocation {
    std::filesystem::path executable;
    std::wstring parameters;
    bool requestElevation{false};
};

struct BootstrapInstalledInfo {
    BootstrapInstalledState state{BootstrapInstalledState::NotInstalled};
    std::filesystem::path installRoot;
    std::wstring version;
    std::wstring diagnostic;
};

struct BootstrapReleaseIdentity {
    std::wstring version;
    std::uint64_t revision{0};
};

std::wstring_view bootstrapPowerShellMode(BootstrapOperation operation) noexcept;
std::wstring quoteWindowsCommandLineArgument(std::wstring_view argument);
BootstrapProcessResult mapBootstrapProcessResult(std::uint32_t exitCode) noexcept;

std::vector<std::wstring> expectedBootstrapArchitectureFiles();
std::vector<std::wstring> expectedBootstrapPackageRootEntries();
BootstrapPackageAssessment assessBootstrapPackageFacts(
    const BootstrapPackageFacts& facts);

bool makeBootstrapPowerShellInvocation(
    BootstrapOperation operation,
    const std::filesystem::path& powershellExecutable,
    const std::filesystem::path& installerScript,
    const std::optional<std::filesystem::path>& packageRoot,
    const std::optional<BootstrapReleaseIdentity>& expectedRelease,
    bool removeHydraSeatUserData,
    BootstrapPowerShellInvocation& invocation,
    std::wstring* error = nullptr);

#ifdef _WIN32
bool inspectBootstrapPackageLayout(
    const std::filesystem::path& setupExecutable,
    BootstrapPackageLayout& layout,
    BootstrapPackageAssessment& assessment);
std::filesystem::path systemWindowsPowerShellPath(std::wstring* error = nullptr);
BootstrapProcessResult runBootstrapPowerShell(
    const BootstrapPowerShellInvocation& invocation,
    std::uint32_t* processExitCode = nullptr,
    std::uint32_t* systemError = nullptr);
BootstrapInstalledInfo queryBootstrapInstalledInfo();
std::optional<BootstrapReleaseIdentity> readValidatedBootstrapReleaseIdentity(
    const std::filesystem::path& signingProvenance,
    std::wstring* error = nullptr);
bool launchInstalledHydraSeatNormally(
    const BootstrapInstalledInfo& installed,
    std::uint32_t* systemError = nullptr);
#endif

} // namespace hydra::installer
