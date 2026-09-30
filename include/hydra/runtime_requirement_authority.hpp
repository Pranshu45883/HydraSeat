#pragma once

#include "hydra/game_runtime_requirement_resolver.hpp"
#include "hydra/provider_adapter.hpp"
#include "hydra/session_metrics.hpp"
#include "hydra/two_seat_launch.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace hydra::requirement {

// Publication deliberately accepts requirements, but never caller-supplied
// capabilities or protected approval. Capabilities are derived from the exact
// local report/result observations below and protected approval remains a
// separate resolver input.
struct RuntimeRequirementAuthorityReview {
    catalog::LocalGameCatalogEntry game;
    provider::ProviderDescriptor provider;
    compat::CompatibilityResult evidence;
    metrics::SessionMetricsReport report;
    launch::Requirements requirements;
};

enum class RuntimeRequirementAuthorityCode : std::uint8_t {
    Success = 0,
    InvalidCatalogGame,
    StaleCatalogGame,
    ProviderMismatch,
    ProviderUnavailable,
    InvalidProviderRevision,
    InvalidCompatibilityEvidence,
    InvalidSessionMetrics,
    EvidenceOriginRejected,
    EvidenceReportMismatch,
    PhysicalEvidenceIneligible,
    InsufficientCapabilityEvidence,
    EvidenceRebindRejected,
    StorePathUnavailable,
    StoreLoadFailed,
    RevisionOverflow,
    InvalidPublishedDocument,
    StoreSaveFailed,
};

struct RuntimeRequirementAuthorityDiagnostic {
    RuntimeRequirementAuthorityCode code{RuntimeRequirementAuthorityCode::Success};
    std::string message;

    bool succeeded() const noexcept {
        return code == RuntimeRequirementAuthorityCode::Success;
    }
};

// Conservative projection only. A capability becomes true only when the current
// provider plus the exact report/result observations positively prove that path.
// This function has no persistence side effect.
launch::Capabilities deriveRuntimeRequirementCapabilities(
    const RuntimeRequirementAuthorityReview& review) noexcept;

// The catalog Game ID is already a bounded, collision-checked stable identity.
// Reusing it avoids absolute executable paths and keeps one stable record ID per
// Game while record revision advances independently.
std::string deterministicRuntimeRequirementRecordId(
    const catalog::LocalGameCatalogEntry& game);

// Missing store means a new empty v1 document. Any malformed/non-missing load
// failure leaves the previous store untouched. Save delegates exclusively to
// GameRuntimeRequirementStore's validate-stage-replace transaction.
RuntimeRequirementAuthorityDiagnostic publishRuntimeRequirementAuthority(
    const RuntimeRequirementAuthorityReview& review,
    const std::filesystem::path& storePath,
    LocalRequirementEvidenceRecord* published = nullptr);

// Production convenience path. It does not broaden path authority; the path is
// resolved only through defaultGameRuntimeRequirementStorePath().
RuntimeRequirementAuthorityDiagnostic publishRuntimeRequirementAuthorityToDefaultStore(
    const RuntimeRequirementAuthorityReview& review,
    LocalRequirementEvidenceRecord* published = nullptr);

std::string_view runtimeRequirementAuthorityCodeName(
    RuntimeRequirementAuthorityCode code) noexcept;

} // namespace hydra::requirement
