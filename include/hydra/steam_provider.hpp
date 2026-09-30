#pragma once

#include "hydra/provider_adapter.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace hydra::provider::steam {

inline constexpr std::size_t kMaximumSteamMetadataBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaximumSteamLibraries = 64u;
inline constexpr std::size_t kMaximumSteamManifests = 8192u;
inline constexpr std::size_t kMaximumSteamKeyValuesDepth = 16u;
inline constexpr std::size_t kMaximumSteamKeyValuesNodes = 131072u;
inline constexpr std::size_t kMaximumSteamExecutableScanEntries = 8192u;
inline constexpr std::size_t kMaximumSteamExecutableScanDepth = 4u;

enum class SteamSourceResult : std::uint8_t {
    Success = 0,
    NotInstalled = 1,
    Failure = 2,
};

// Read-only boundary used by the Steam adapter. Test sources provide virtual
// files/process observations; the native source uses registry/filesystem/process
// observation only and exposes no write, launch, authentication, or mutation API.
class SteamMetadataSource {
public:
    virtual ~SteamMetadataSource() = default;

    virtual SteamSourceResult locateInstallation(
        std::wstring& steamRoot,
        std::string& error) noexcept = 0;
    virtual bool readTextFile(const std::wstring& path,
                              std::size_t maximumBytes,
                              std::string& bytes,
                              std::string& error) noexcept = 0;
    virtual bool listManifestFiles(
        std::span<const std::wstring> steamAppsRoots,
        std::vector<std::wstring>& paths,
        std::string& error) noexcept = 0;
    virtual bool listExecutableHints(
        const std::wstring& installRoot,
        std::vector<std::wstring>& paths,
        std::string& error) noexcept = 0;
    virtual bool observeProcesses(
        std::span<const std::wstring> executablePaths,
        std::vector<ProviderProcessEvidence>& processes,
        std::string& error) noexcept = 0;
};

std::shared_ptr<SteamMetadataSource> makeNativeSteamMetadataSource();

class SteamProviderAdapter final : public LauncherProviderAdapter {
public:
    explicit SteamProviderAdapter(std::shared_ptr<SteamMetadataSource> source);

    // Refresh is read-only and transactional. A failed refresh retains no
    // partial app snapshot and exposes an explicit provider failure.
    ProviderDiagnostic refresh() noexcept;

    ProviderDescriptor descriptor() const noexcept override;
    DiscoveryResponse discoverInstalledGames() noexcept override;
    AccountReferenceResponse listAccountReferences() noexcept override;
    LaunchResponse buildLaunchRequest(
        const LaunchSelection& selection) noexcept override;
    ProcessIdentificationResponse identifyProcesses(
        const ProcessIdentificationQuery& query) noexcept override;

private:
    struct AppSnapshot {
        std::string appId;
        std::string gameId;
        std::wstring title;
        std::wstring installRoot;
        std::vector<std::wstring> executableHints;
        std::optional<std::wstring> buildId;
        catalog::GameCatalogCandidate candidate;
    };

    const AppSnapshot* findApp(std::string_view appId) const noexcept;

    std::shared_ptr<SteamMetadataSource> source_;
    ProviderDescriptor descriptor_;
    ProviderResult snapshotResult_{ProviderResult::ProviderAbsent};
    std::string snapshotDiagnostic_;
    std::vector<AppSnapshot> apps_;
};

std::string_view steamSourceResultName(SteamSourceResult result) noexcept;

} // namespace hydra::provider::steam
