#pragma once

#include "hydra/artifact_trust.hpp"
#include "hydra/compatibility_result.hpp"
#include "hydra/profile_schema.hpp"
#include "hydra/setup_package.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::community {

inline constexpr std::uint32_t kCommunityPackageManifestVersion = 1u;
inline constexpr std::size_t kMaximumCommunityPackageEntries = 2048u;
inline constexpr std::size_t kMaximumCommunityPackageHistory = 16u;

enum class EntryKind : std::uint8_t {
    CompatibilityResult = 0,
    TwoPlayerSetup = 1,
    CatalogSnapshot = 2,
};

struct GameSelector {
    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::optional<std::string> gameVersion;

    bool operator==(const GameSelector&) const = default;
};

struct PackageEntry {
    std::string entryId;
    EntryKind kind{EntryKind::CompatibilityResult};
    std::uint32_t contentSchemaVersion{1u};
    std::string expectedSha256;
    GameSelector selector;

    bool operator==(const PackageEntry&) const = default;
};

struct CommunityPackageManifest {
    std::uint32_t schemaVersion{kCommunityPackageManifestVersion};
    std::string packageId;
    std::uint64_t packageRevision{0};
    std::string packageVersion;
    std::string packageSha256;
    std::string sourceId;
    std::string licenseId;
    bool redistributionAllowed{false};
    std::uint32_t minimumProfileSchema{profile::kProfileSchemaVersion};
    std::uint32_t maximumProfileSchema{profile::kProfileSchemaVersion};
    std::vector<PackageEntry> entries;

    bool operator==(const CommunityPackageManifest&) const = default;
};

struct EntryObservation {
    std::string entryId;
    std::string observedSha256;

    bool operator==(const EntryObservation&) const = default;
};

enum class PackageCode : std::uint8_t {
    Success = 0,
    InvalidManifest,
    UnsupportedManifestVersion,
    UnsupportedSchemaRange,
    TooManyEntries,
    DuplicateEntry,
    InvalidEntry,
    MissingEntry,
    UnexpectedEntry,
    EntryHashMismatch,
    PackageTrustRejected,
    StaleRevision,
    NoInstalledPackage,
    NoRollbackVersion,
};

struct PackageDiagnostic {
    PackageCode code{PackageCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == PackageCode::Success; }
};

PackageDiagnostic validateCommunityPackageManifest(const CommunityPackageManifest& manifest);

// Pure in-memory lifecycle used by catalog/cache layers. Artifact bytes are
// verified by exact hashes supplied by a separate read-only loader; the store has
// no path/output/script/executable fields and cannot grant execution capability.
class CommunityPackageStore {
public:
    PackageDiagnostic installOrUpdate(const CommunityPackageManifest& manifest,
                                      std::string_view observedPackageSha256,
                                      std::span<const EntryObservation> entries,
                                      const trust::TrustPolicy& policy);
    PackageDiagnostic rollback();
    void clear() noexcept;

    const std::optional<CommunityPackageManifest>& current() const noexcept { return current_; }
    const std::vector<CommunityPackageManifest>& history() const noexcept { return history_; }

private:
    std::optional<CommunityPackageManifest> current_;
    std::vector<CommunityPackageManifest> history_;
};

std::string_view entryKindName(EntryKind kind) noexcept;
std::string_view packageCodeName(PackageCode code) noexcept;

} // namespace hydra::community
