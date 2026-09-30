#pragma once

#include "hydra/game_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::provider {

inline constexpr std::size_t kMaximumProviderDiagnosticBytes = 1024u;
inline constexpr std::size_t kMaximumProviderAccounts = 64u;
inline constexpr std::size_t kMaximumLaunchArguments = 128u;
inline constexpr std::size_t kMaximumLaunchArgumentCodeUnits = 4096u;
inline constexpr std::size_t kMaximumLaunchUriCodeUnits = 8192u;
inline constexpr std::size_t kMaximumProcessEvidence = 256u;

enum class ProviderAvailability : std::uint8_t {
    Available = 0,
    Offline = 1,
    Absent = 2,
};

struct ProviderCapabilities {
    bool installedGameDiscovery{false};
    bool accountReferences{false};
    bool launchRequests{false};
    bool offlineLaunch{false};
    bool processIdentification{false};

    bool operator==(const ProviderCapabilities&) const = default;
};

struct ProviderDescriptor {
    std::string providerId;
    ProviderAvailability availability{ProviderAvailability::Absent};
    std::uint64_t metadataRevision{0};
    ProviderCapabilities capabilities;

    bool operator==(const ProviderDescriptor&) const = default;
};

enum class ProviderResult : std::uint8_t {
    Success = 0,
    ProviderAbsent = 1,
    ProviderOffline = 2,
    UnsupportedOperation = 3,
    InvalidDescriptor = 4,
    InvalidRequest = 5,
    InvalidMetadata = 6,
    StaleMetadata = 7,
    ProviderFailure = 8,
};

struct ProviderDiagnostic {
    ProviderResult result{ProviderResult::Success};
    std::string message;

    bool succeeded() const noexcept { return result == ProviderResult::Success; }
};

struct DiscoveryResponse {
    ProviderResult result{ProviderResult::Success};
    std::uint64_t metadataRevision{0};
    std::vector<catalog::GameCatalogCandidate> candidates;
    std::string diagnostic;
};

struct AccountReferenceResponse {
    ProviderResult result{ProviderResult::Success};
    std::uint64_t metadataRevision{0};
    std::vector<profile::ProviderAccountReference> accounts;
    std::string diagnostic;
};

enum class LaunchTargetKind : std::uint8_t {
    Executable = 0,
    ProviderUri = 1,
};

struct LaunchSelection {
    std::string providerId;
    std::string gameId;
    std::optional<std::string> providerAppId;
    std::optional<std::string> accountRef;
    std::uint64_t expectedMetadataRevision{0};
    std::vector<std::wstring> instanceArguments;

    bool operator==(const LaunchSelection&) const = default;
};

// A launch request is structured data, never a command line or shell string.
// The process-owning layer is responsible for quoting each argument for the
// selected Windows API without concatenating untrusted provider text.
struct ProviderLaunchRequest {
    std::string providerId;
    std::string gameId;
    std::optional<std::string> providerAppId;
    std::optional<std::string> accountRef;
    std::uint64_t metadataRevision{0};
    LaunchTargetKind targetKind{LaunchTargetKind::Executable};
    std::wstring target;
    std::vector<std::wstring> arguments;
    std::optional<std::wstring> workingDirectory;
    std::string launchCorrelationId;

    bool operator==(const ProviderLaunchRequest&) const = default;
};

struct LaunchResponse {
    ProviderResult result{ProviderResult::Success};
    ProviderLaunchRequest request;
    std::string diagnostic;
};

struct ProcessIdentificationQuery {
    std::string providerId;
    std::string gameId;
    std::optional<std::string> providerAppId;
    std::string launchCorrelationId;
    std::uint64_t expectedMetadataRevision{0};

    bool operator==(const ProcessIdentificationQuery&) const = default;
};

struct ProviderProcessEvidence {
    std::uint32_t processId{0};
    std::uint64_t creationTime100ns{0};
    std::wstring executablePath;
    bool providerRelationshipVerified{false};

    bool operator==(const ProviderProcessEvidence&) const = default;
};

struct ProcessIdentificationResponse {
    ProviderResult result{ProviderResult::Success};
    std::uint64_t metadataRevision{0};
    std::vector<ProviderProcessEvidence> processes;
    std::string diagnostic;
};

// Provider implementations own their supported read-only metadata access and
// normal launcher integration. This boundary has no credential, mutation,
// arbitrary script, shell-command, or bypass operation.
class LauncherProviderAdapter {
public:
    virtual ~LauncherProviderAdapter() = default;

    virtual ProviderDescriptor descriptor() const noexcept = 0;
    virtual DiscoveryResponse discoverInstalledGames() noexcept = 0;
    virtual AccountReferenceResponse listAccountReferences() noexcept = 0;
    virtual LaunchResponse buildLaunchRequest(
        const LaunchSelection& selection) noexcept = 0;
    virtual ProcessIdentificationResponse identifyProcesses(
        const ProcessIdentificationQuery& query) noexcept = 0;
};

// All boundary functions validate complete adapter output before replacing the
// caller's previous value. Provider absence/offline/capability failures are
// explicit and never fall back to another provider or executable silently.
ProviderDiagnostic discoverInstalledGames(
    LauncherProviderAdapter& adapter,
    std::vector<catalog::GameCatalogCandidate>& output);
ProviderDiagnostic listAccountReferences(
    LauncherProviderAdapter& adapter,
    std::vector<profile::ProviderAccountReference>& output);
ProviderDiagnostic buildLaunchRequest(
    LauncherProviderAdapter& adapter,
    const LaunchSelection& selection,
    ProviderLaunchRequest& output);
ProviderDiagnostic identifyProcesses(
    LauncherProviderAdapter& adapter,
    const ProcessIdentificationQuery& query,
    std::vector<ProviderProcessEvidence>& output);

std::string_view providerResultName(ProviderResult result) noexcept;
std::string_view providerAvailabilityName(ProviderAvailability availability) noexcept;
std::string_view launchTargetKindName(LaunchTargetKind kind) noexcept;

} // namespace hydra::provider
