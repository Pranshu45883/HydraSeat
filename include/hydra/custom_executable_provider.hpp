#pragma once

#include "hydra/provider_adapter.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::provider::custom {

enum class CustomExecutableSourceResult : std::uint8_t {
    Success = 0,
    NotFound = 1,
    InvalidExecutable = 2,
    Failure = 3,
};

struct CustomExecutableDefinition {
    std::wstring title;
    std::wstring executablePath;
    std::vector<std::wstring> arguments;
    std::optional<std::wstring> workingDirectory;
    std::optional<std::wstring> localIconSource;

    bool operator==(const CustomExecutableDefinition&) const = default;
};

struct CustomExecutableObservation {
    std::wstring canonicalExecutablePath;
    std::wstring canonicalWorkingDirectory;
    std::wstring canonicalIconSource;
    std::uint64_t executableSize{0};
    std::uint64_t executableWriteTime{0};
    catalog::ExecutableArchitecture architecture{
        catalog::ExecutableArchitecture::Unknown};

    bool operator==(const CustomExecutableObservation&) const = default;
};

// Read-only virtualizable source. Native inspection validates local filesystem
// identity and PE architecture; process observation never starts or stops work.
class CustomExecutableSource {
public:
    virtual ~CustomExecutableSource() = default;

    virtual CustomExecutableSourceResult inspect(
        const CustomExecutableDefinition& definition,
        CustomExecutableObservation& observation,
        std::string& error) noexcept = 0;
    virtual bool observeProcesses(
        const std::wstring& canonicalExecutablePath,
        std::vector<ProviderProcessEvidence>& processes,
        std::string& error) noexcept = 0;
};

std::shared_ptr<CustomExecutableSource> makeNativeCustomExecutableSource();

class CustomExecutableProviderAdapter final : public LauncherProviderAdapter {
public:
    CustomExecutableProviderAdapter(std::shared_ptr<CustomExecutableSource> source,
                                    CustomExecutableDefinition definition);

    ProviderDiagnostic refresh() noexcept;

    ProviderDescriptor descriptor() const noexcept override;
    DiscoveryResponse discoverInstalledGames() noexcept override;
    AccountReferenceResponse listAccountReferences() noexcept override;
    LaunchResponse buildLaunchRequest(
        const LaunchSelection& selection) noexcept override;
    ProcessIdentificationResponse identifyProcesses(
        const ProcessIdentificationQuery& query) noexcept override;

private:
    std::shared_ptr<CustomExecutableSource> source_;
    CustomExecutableDefinition definition_;
    CustomExecutableObservation observation_;
    ProviderDescriptor descriptor_;
    ProviderResult snapshotResult_{ProviderResult::ProviderAbsent};
    std::string snapshotDiagnostic_;
    catalog::GameCatalogCandidate candidate_;
    std::string gameId_;
    std::string appId_;
};

std::string_view customExecutableSourceResultName(
    CustomExecutableSourceResult result) noexcept;

} // namespace hydra::provider::custom
