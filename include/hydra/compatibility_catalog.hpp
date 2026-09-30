#pragma once

#include "hydra/community_package.hpp"
#include "hydra/community_setup.hpp"
#include "hydra/compatibility_aggregation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::community {

inline constexpr std::size_t kMaximumCatalogResults = 100000u;
inline constexpr std::size_t kMaximumCatalogSetups = 4096u;

struct LocalCatalogSnapshot {
    CommunityPackageManifest manifest;
    std::vector<compat::CompatibilityResult> results;
    std::vector<CommunitySetupEntry> setups;

    bool operator==(const LocalCatalogSnapshot&) const = default;
};

struct GameEvidenceView {
    GameSelector selector;
    std::vector<compat::CohortStatistics> cohorts;
    std::vector<CommunitySetupEntry> setups;
    std::uint64_t totalResultSamples{0};
    std::uint64_t currentResultSamples{0};
    std::uint64_t staleResultSamples{0};

    bool operator==(const GameEvidenceView&) const = default;
};

enum class CatalogCode : std::uint8_t {
    Success = 0,
    InvalidPackage,
    TooManyPayloads,
    MissingManifestEntry,
    DuplicatePayload,
    KindMismatch,
    SelectorMismatch,
    InvalidResult,
    InvalidSetup,
    AggregationFailed,
};

struct CatalogDiagnostic {
    CatalogCode code{CatalogCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == CatalogCode::Success; }
};

// Validates that every decoded payload is declared by the already trusted package
// manifest, then canonicalizes local result/setup ordering. It performs no network,
// filesystem, image, URI, script, or executable work.
CatalogDiagnostic buildLocalCatalogSnapshot(
    const CommunityPackageManifest& manifest,
    std::span<const compat::CompatibilityResult> results,
    std::span<const CommunitySetupEntry> setups,
    LocalCatalogSnapshot& output);

CatalogDiagnostic queryGameEvidence(
    const LocalCatalogSnapshot& snapshot,
    const GameSelector& selector,
    const compat::AggregationPolicy& policy,
    GameEvidenceView& output);

std::string_view catalogCodeName(CatalogCode code) noexcept;

} // namespace hydra::community
