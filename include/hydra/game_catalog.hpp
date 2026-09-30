#pragma once

#include "hydra/profile_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hydra::catalog {

inline constexpr std::size_t kMaximumCatalogCandidates = 8192u;
inline constexpr std::size_t kNoCandidateIndex = static_cast<std::size_t>(-1);

enum class ExecutableArchitecture : std::uint8_t {
    Unknown = 0,
    X86 = 1,
    X64 = 2,
    Arm64 = 3,
};

enum class CatalogStaleness : std::uint8_t {
    Unknown = 0,
    Current = 1,
    Stale = 2,
};

enum class CatalogBuildResult : std::uint8_t {
    Success = 0,
    TooManyCandidates = 1,
    InvalidCandidate = 2,
    IdentityConflict = 3,
    IdentityCollision = 4,
    SchemaValidationError = 5,
};

// Provider adapters and the manual Add Game path both emit this same bounded
// candidate shape. The catalog core performs no filesystem, registry, network,
// process, launcher, or provider I/O; it only validates and reconciles supplied
// local metadata. providerId/providerAppId are logical identities, while title
// and icon are presentation metadata and never define identity by themselves.
struct GameCatalogCandidate {
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::wstring title;
    std::wstring installRoot;
    std::vector<std::wstring> executableCandidates;
    std::optional<std::wstring> localVersion;
    std::optional<std::string> executableSha256;
    std::optional<profile::CompatibilityReference> compatibility;
    profile::GameOrigin origin{profile::GameOrigin::Discovered};
    std::optional<std::wstring> localIconSource;
    ExecutableArchitecture architecture{ExecutableArchitecture::Unknown};
    CatalogStaleness staleness{CatalogStaleness::Unknown};

    bool operator==(const GameCatalogCandidate&) const = default;
};

// Catalog-only metadata stays outside persisted GameRecord until a schema packet
// explicitly chooses to persist it. Missing icon or unknown architecture never
// blocks a valid game record.
struct LocalGameCatalogEntry {
    profile::GameRecord game;
    std::optional<std::wstring> localIconSource;
    ExecutableArchitecture architecture{ExecutableArchitecture::Unknown};
    CatalogStaleness staleness{CatalogStaleness::Unknown};
    std::uint32_t mergedCandidateCount{0};

    bool operator==(const LocalGameCatalogEntry&) const = default;
};

struct LocalGameCatalog {
    std::vector<LocalGameCatalogEntry> entries;

    bool operator==(const LocalGameCatalog&) const = default;
};

struct CatalogBuildDiagnostic {
    CatalogBuildResult result{CatalogBuildResult::Success};
    std::size_t candidateIndex{kNoCandidateIndex};
    std::string message;

    bool succeeded() const noexcept { return result == CatalogBuildResult::Success; }
};

// Transactional pure catalog build. On any malformed candidate, ambiguous strong
// identity, incompatible hash metadata, generated-ID collision, or final schema
// failure, output is left unchanged. Candidate ordering does not affect a
// successful catalog result.
CatalogBuildDiagnostic buildLocalGameCatalog(
    std::span<const GameCatalogCandidate> candidates,
    LocalGameCatalog& output);

std::string_view catalogBuildResultName(CatalogBuildResult result) noexcept;
std::string_view executableArchitectureName(ExecutableArchitecture architecture) noexcept;
std::string_view catalogStalenessName(CatalogStaleness staleness) noexcept;

} // namespace hydra::catalog
