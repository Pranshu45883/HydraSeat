#pragma once

#include "hydra/compatibility_result.hpp"
#include "hydra/game_catalog.hpp"
#include "hydra/provider_launch_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hydra::materialization {
class ITrustedMaterializationDecisionSource;
}

namespace hydra::requirement {

inline constexpr std::uint32_t kLegacyRequirementEvidenceStoreSchemaVersion = 1u;
inline constexpr std::uint32_t kRequirementEvidenceStoreSchemaVersion = 2u;
inline constexpr std::size_t kMaximumRequirementEvidenceStoreBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaximumRequirementEvidenceRecords = profile::kMaximumGames;
inline constexpr std::size_t kMaximumRequirementResolveIssues = profile::kMaximumGames;

// Persisted authority is deliberately narrower than a launch plan. It records the
// exact local game/provider/evidence revision that justified one requirements and
// capabilities decision. Process IDs, paths, account references, community scores,
// and launch commands are not representable here.
struct LocalRequirementEvidenceRecord {
    std::string recordId;
    std::uint64_t revision{0};

    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::optional<std::string> gameVersionUtf8;
    std::optional<std::string> executableSha256;
    std::optional<profile::CompatibilityReference> catalogCompatibility;

    std::uint64_t providerMetadataRevision{0};
    std::string evidenceResultId;
    std::string evidenceProvenanceId;
    std::uint64_t evidenceProvenanceRevision{0};
    std::uint8_t validatedSeatCount{0};

    launch::Requirements requirements;
    launch::Capabilities capabilities;

    bool operator==(const LocalRequirementEvidenceRecord&) const = default;
};

struct RequirementEvidenceDocument {
    std::uint32_t schemaVersion{kRequirementEvidenceStoreSchemaVersion};
    std::vector<LocalRequirementEvidenceRecord> records;

    bool operator==(const RequirementEvidenceDocument&) const = default;
};

enum class RequirementStoreCode : std::uint8_t {
    Success = 0,
    Missing,
    TooLarge,
    ParseError,
    UnsupportedSchema,
    UnknownField,
    InvalidRecord,
    DuplicateRecord,
    DuplicateGameAuthority,
    ReadFailed,
    WriteFailed,
    RemoveFailed,
};

struct RequirementStoreDiagnostic {
    RequirementStoreCode code{RequirementStoreCode::Success};
    std::string message;

    bool succeeded() const noexcept {
        return code == RequirementStoreCode::Success || code == RequirementStoreCode::Missing;
    }
    bool found() const noexcept { return code != RequirementStoreCode::Missing; }
};

RequirementStoreDiagnostic validateRequirementEvidenceDocument(
    const RequirementEvidenceDocument& document);
RequirementStoreDiagnostic encodeRequirementEvidenceDocumentJson(
    const RequirementEvidenceDocument& document,
    std::string& output);
RequirementStoreDiagnostic decodeRequirementEvidenceDocumentJson(
    std::string_view json,
    RequirementEvidenceDocument& output);

// Fixed-purpose local store. Save is validate-then-stage-then-replace, and load
// never replaces caller state with malformed or partial data.
class GameRuntimeRequirementStore final {
public:
    explicit GameRuntimeRequirementStore(std::filesystem::path path)
        : path_(std::move(path)) {}

    RequirementStoreDiagnostic load(RequirementEvidenceDocument& output) const;
    RequirementStoreDiagnostic save(const RequirementEvidenceDocument& document) const;
    RequirementStoreDiagnostic remove() const;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path temporaryPath() const;

    std::filesystem::path path_;
};

std::optional<std::filesystem::path> defaultGameRuntimeRequirementStorePath(
    std::string* error = nullptr);

// Imported community evidence is never accepted as runtime authority. Production
// defaults to PhysicalOnly. ControlledOrPhysical exists for controlled integration
// environments but still rejects Synthetic and ImportedCommunity results.
enum class LocalEvidenceTrust : std::uint8_t {
    PhysicalOnly = 0,
    ControlledOrPhysical = 1,
};

struct RequirementResolveContext {
    // Deterministic wall-clock input. Resolver itself never reads the clock.
    std::string referenceMonth; // YYYY-MM
    std::uint32_t staleAfterMonths{6u};

    // Exact local environment identity for the evidence being consumed.
    std::string hydraSeatVersion;
    std::string hydraSeatBuild;
    std::string windowsBuildClass;
    std::string architecture;
    LocalEvidenceTrust trust{LocalEvidenceTrust::PhysicalOnly};
};

// Approval is not persisted in the evidence store. A protected experiment must
// supply a caller-owned approval pinned to the exact game/provider/store/evidence
// revisions that are being resolved now.
struct ProtectedRuntimeApproval {
    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::uint64_t providerMetadataRevision{0};
    std::string requirementRecordId;
    std::uint64_t requirementRevision{0};
    std::string evidenceResultId;
    std::uint64_t evidenceProvenanceRevision{0};

    bool operator==(const ProtectedRuntimeApproval&) const = default;
};

enum class RequirementResolveCode : std::uint8_t {
    MissingStoredEvidence = 0,
    StaleCatalogGame,
    GameIdentityMismatch,
    MissingProvider,
    DuplicateProvider,
    ProviderUnavailable,
    StaleProviderRevision,
    MissingLocalEvidence,
    InvalidLocalEvidence,
    CommunityEvidenceRejected,
    UntrustedLocalEvidenceOrigin,
    EvidenceEnvironmentMismatch,
    StaleLocalEvidence,
    FutureLocalEvidence,
    InsufficientCapabilityEvidence,
    ProtectedApprovalRequired,
    DuplicateProtectedApproval,
};

struct RequirementResolveIssue {
    RequirementResolveCode code{RequirementResolveCode::MissingStoredEvidence};
    std::string gameId;
    std::string message;

    bool operator==(const RequirementResolveIssue&) const = default;
};

inline constexpr std::uint32_t kTrustedRequirementSnapshotSchemaVersion = 1u;

// Host-side authority keeps the exact provider/evidence binding that produced the
// launcher-facing GameRuntimeRequirement. The launcher receives only requirements;
// the production runtime consumes authorities as a second, stricter gate.
struct TrustedGameRuntimeAuthority {
    plan::GameRuntimeRequirement requirement;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::uint64_t providerMetadataRevision{0};
    std::optional<std::string> gameVersionUtf8;
    std::optional<std::string> executableSha256;
    std::vector<std::wstring> executableCandidates;
    std::string evidenceResultId;
    std::string evidenceProvenanceId;
    std::uint64_t evidenceProvenanceRevision{0};
    std::string evidenceTimestampBucket;
    compat::ResultOrigin evidenceOrigin{compat::ResultOrigin::Synthetic};

    bool operator==(const TrustedGameRuntimeAuthority&) const = default;
};

struct TrustedRequirementSnapshot {
    std::uint32_t schemaVersion{kTrustedRequirementSnapshotSchemaVersion};
    std::string referenceMonth;
    std::uint32_t staleAfterMonths{0};
    LocalEvidenceTrust trust{LocalEvidenceTrust::PhysicalOnly};
    std::vector<TrustedGameRuntimeAuthority> authorities;
    std::vector<plan::GameRuntimeRequirement> requirements;
    std::vector<RequirementResolveIssue> blockedGames;
};

enum class RequirementSnapshotCode : std::uint8_t {
    Success = 0,
    InvalidContext,
    InvalidStore,
    InvalidCatalog,
    TooManyInputs,
    DuplicateLocalEvidenceId,
    InvalidApproval,
    DuplicateApproval,
    InputUnavailable,
    InternalFailure,
};

struct RequirementSnapshotDiagnostic {
    RequirementSnapshotCode code{RequirementSnapshotCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == RequirementSnapshotCode::Success; }
};

enum class TrustedPlanRequirementCode : std::uint8_t {
    Success = 0,
    InvalidSnapshot,
    MissingAuthority,
    DuplicateAuthority,
    GameIdentityMismatch,
    ProviderIdentityMismatch,
    ProviderRevisionMismatch,
    RequirementMismatch,
    UntrustedEvidenceOrigin,
    StaleEvidence,
    ProtectedApprovalRequired,
    ValidationSeatScopeExceeded,
};

struct TrustedPlanRequirementDiagnostic {
    TrustedPlanRequirementCode code{TrustedPlanRequirementCode::Success};
    std::string gameId;
    std::string message;

    bool succeeded() const noexcept { return code == TrustedPlanRequirementCode::Success; }
};

// Builds authority only from the exact current local snapshots supplied by the
// caller. Community aggregation/popularity is intentionally absent from this API.
// A blocked game receives no GameRuntimeRequirement entry, so existing P6 planning
// fails closed with MissingRequirement if the user selects it.
RequirementSnapshotDiagnostic resolveTrustedGameRuntimeRequirements(
    const RequirementEvidenceDocument& stored,
    const catalog::LocalGameCatalog& catalog,
    std::span<const plan::ProviderAdapterBinding> providers,
    std::span<const compat::CompatibilityResult> localEvidence,
    std::span<const ProtectedRuntimeApproval> protectedApprovals,
    const RequirementResolveContext& context,
    TrustedRequirementSnapshot& output);

namespace detail {

inline bool parseTrustedMonth(std::string_view value, std::int64_t& output) noexcept {
    if (value.size() != 7u || value[4] != '-') return false;
    for (std::size_t index = 0u; index < value.size(); ++index) {
        if (index == 4u) continue;
        if (value[index] < '0' || value[index] > '9') return false;
    }
    const auto year = static_cast<std::int64_t>(value[0] - '0') * 1000 +
                      static_cast<std::int64_t>(value[1] - '0') * 100 +
                      static_cast<std::int64_t>(value[2] - '0') * 10 +
                      static_cast<std::int64_t>(value[3] - '0');
    const auto month = static_cast<std::int64_t>(value[5] - '0') * 10 +
                       static_cast<std::int64_t>(value[6] - '0');
    if (year < 2000 || year > 9999 || month < 1 || month > 12) return false;
    output = year * 12 + (month - 1);
    return true;
}

inline TrustedPlanRequirementDiagnostic trustedPlanFailure(
    TrustedPlanRequirementCode code, std::string gameId, std::string message) {
    return {code, std::move(gameId), std::move(message)};
}

} // namespace detail

// Runtime-side validation is intentionally header-only so the host registry can
// enforce the gate without linking the resolver implementation itself. A trusted
// source must re-resolve immediately before install; the runtime then compares the
// immutable plan against that exact snapshot before any registry/resource mutation.
inline TrustedPlanRequirementDiagnostic validateProviderAwareLaunchPlanAgainstTrustedRequirements(
    const plan::ProviderAwareLaunchPlan& providerPlan,
    const TrustedRequirementSnapshot& snapshot,
    bool requirePhysicalEvidence = true) {
    if (snapshot.schemaVersion != kTrustedRequirementSnapshotSchemaVersion ||
        snapshot.authorities.size() > profile::kMaximumGames ||
        snapshot.requirements.size() != snapshot.authorities.size() ||
        snapshot.staleAfterMonths > 120u ||
        (snapshot.trust != LocalEvidenceTrust::PhysicalOnly &&
         snapshot.trust != LocalEvidenceTrust::ControlledOrPhysical)) {
        return detail::trustedPlanFailure(
            TrustedPlanRequirementCode::InvalidSnapshot, {},
            "trusted runtime requirement snapshot is malformed or internally inconsistent");
    }
    std::int64_t referenceMonth = 0;
    if (!detail::parseTrustedMonth(snapshot.referenceMonth, referenceMonth)) {
        return detail::trustedPlanFailure(
            TrustedPlanRequirementCode::InvalidSnapshot, {},
            "trusted runtime requirement snapshot has an invalid reference month");
    }

    for (std::size_t index = 0u; index < snapshot.authorities.size(); ++index) {
        const auto& authority = snapshot.authorities[index];
        if (authority.requirement.gameId.empty() || authority.requirement.revision == 0u ||
            authority.requirement.validatedSeatCount < 1u ||
            authority.requirement.validatedSeatCount > 2u ||
            authority.providerId.empty() || authority.providerMetadataRevision == 0u ||
            authority.executableCandidates.empty() ||
            authority.executableCandidates.size() > profile::kMaximumExecutableCandidates ||
            authority.evidenceResultId.empty() || authority.evidenceProvenanceId.empty() ||
            authority.evidenceProvenanceRevision == 0u ||
            authority.evidenceTimestampBucket.size() < 7u) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::InvalidSnapshot, authority.requirement.gameId,
                "trusted runtime authority has an invalid identity/revision field");
        }
        for (std::size_t other = index + 1u; other < snapshot.authorities.size(); ++other) {
            if (snapshot.authorities[other].requirement.gameId == authority.requirement.gameId) {
                return detail::trustedPlanFailure(
                    TrustedPlanRequirementCode::DuplicateAuthority, authority.requirement.gameId,
                    "trusted snapshot contains duplicate authority for one Game");
            }
        }
        const auto matchingRequirement = std::count(
            snapshot.requirements.begin(), snapshot.requirements.end(), authority.requirement);
        if (matchingRequirement != 1) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::InvalidSnapshot, authority.requirement.gameId,
                "launcher requirement vector diverges from runtime authority records");
        }
        if (authority.evidenceOrigin == compat::ResultOrigin::ImportedCommunity ||
            authority.evidenceOrigin == compat::ResultOrigin::Synthetic ||
            (requirePhysicalEvidence && authority.evidenceOrigin != compat::ResultOrigin::Physical) ||
            (snapshot.trust == LocalEvidenceTrust::PhysicalOnly &&
             authority.evidenceOrigin != compat::ResultOrigin::Physical)) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::UntrustedEvidenceOrigin,
                authority.requirement.gameId,
                "runtime authority is not backed by acceptable local physical evidence");
        }
        std::int64_t observedMonth = 0;
        if (!detail::parseTrustedMonth(
                std::string_view(authority.evidenceTimestampBucket).substr(0u, 7u),
                observedMonth) || observedMonth > referenceMonth ||
            static_cast<std::uint64_t>(referenceMonth - observedMonth) >
                snapshot.staleAfterMonths) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::StaleEvidence, authority.requirement.gameId,
                "runtime authority evidence is stale, future-dated, or malformed");
        }
        if (authority.requirement.requirements.highRisk &&
            !authority.requirement.highRiskApproved) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::ProtectedApprovalRequired,
                authority.requirement.gameId,
                "Protected / Experimental runtime authority lacks exact current approval");
        }
    }

    for (const auto& seat : providerPlan.seats) {
        const TrustedGameRuntimeAuthority* authority = nullptr;
        for (const auto& candidate : snapshot.authorities) {
            if (candidate.requirement.gameId != seat.gameId) continue;
            if (authority != nullptr) {
                return detail::trustedPlanFailure(
                    TrustedPlanRequirementCode::DuplicateAuthority, seat.gameId,
                    "multiple trusted authorities match the planned Game");
            }
            authority = &candidate;
        }
        if (authority == nullptr) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::MissingAuthority, seat.gameId,
                "provider plan Game is absent from the freshly resolved trusted snapshot");
        }
        if (seat.launchRequest.providerId != authority->providerId ||
            seat.launchRequest.providerAppId != authority->providerAppId ||
            seat.launchRequest.gameId != seat.gameId) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::ProviderIdentityMismatch, seat.gameId,
                "provider plan identity does not match trusted local Game authority");
        }
        if (seat.launchRequest.targetKind == provider::LaunchTargetKind::Executable &&
            std::find(authority->executableCandidates.begin(), authority->executableCandidates.end(),
                      seat.launchRequest.target) == authority->executableCandidates.end()) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::GameIdentityMismatch, seat.gameId,
                "provider plan executable target is outside the exact current Game identity");
        }
        if (seat.launchRequest.metadataRevision != authority->providerMetadataRevision) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::ProviderRevisionMismatch, seat.gameId,
                "provider metadata revision changed after trusted requirement resolution");
        }
        const auto& trusted = authority->requirement;
        const auto selectedSeatCount = static_cast<std::uint8_t>(std::count_if(
            providerPlan.seats.begin(), providerPlan.seats.end(),
            [&](const plan::SeatProviderLaunchPlan& candidate) {
                return candidate.gameId == seat.gameId;
            }));
        if (selectedSeatCount > trusted.validatedSeatCount) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::ValidationSeatScopeExceeded, seat.gameId,
                "provider plan uses the Game on more Seats than the trusted physical validation scope");
        }
        if (seat.requirementRevision != trusted.revision ||
            seat.requirements != trusted.requirements ||
            seat.capabilities != trusted.capabilities ||
            seat.compatibility != trusted.compatibility) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::RequirementMismatch, seat.gameId,
                "provider plan requirement/capability evidence differs from trusted authority");
        }
        if (seat.requirements.highRisk && !trusted.highRiskApproved) {
            return detail::trustedPlanFailure(
                TrustedPlanRequirementCode::ProtectedApprovalRequired, seat.gameId,
                "provider plan cannot activate a Protected path without trusted approval");
        }
    }
    return {};
}

// Production sources should capture provider/catalog/evidence inputs read-only and
// freshly for each call. The registry invokes this interface outside its mutex and
// performs its own exact plan comparison under the current host profile/session.
class ITrustedRequirementSource {
public:
    virtual ~ITrustedRequirementSource() = default;
    virtual RequirementSnapshotDiagnostic resolveCurrent(
        TrustedRequirementSnapshot& output) = 0;

    // Production requirement authority is also the composition root for optional
    // locally trusted compatibility materialization. Test/custom sources default
    // to no materialization so plans without a decision retain the old path.
    virtual std::shared_ptr<materialization::ITrustedMaterializationDecisionSource>
    trustedMaterializationDecisionSource() const {
        return {};
    }
    virtual std::filesystem::path trustedMaterializationInstancesRoot() const {
        return {};
    }
};

struct RequirementResolveInputs {
    catalog::LocalGameCatalog catalog;
    std::vector<plan::ProviderAdapterBinding> providers;
    // Capture-scoped ownership for provider bindings whose interface stores raw
    // adapter pointers. Production capture creates fresh read-only adapters per
    // snapshot so concurrent Host revalidation cannot mutate another snapshot's
    // provider identity while it is being resolved.
    std::vector<std::shared_ptr<provider::LauncherProviderAdapter>> providerOwners;
    std::vector<compat::CompatibilityResult> localEvidence;
    std::vector<ProtectedRuntimeApproval> protectedApprovals;
    RequirementResolveContext context;
};

class IRequirementResolveInputSource {
public:
    virtual ~IRequirementResolveInputSource() = default;
    virtual bool capture(RequirementResolveInputs& output, std::string& error) = 0;
};

// Fixed-store production adapter: every resolve reloads the bounded requirement
// evidence document, then captures current provider/catalog/local-evidence inputs
// and runs the same strict resolver used by the launcher path.
class StoreBackedTrustedRequirementSource final : public ITrustedRequirementSource {
public:
    StoreBackedTrustedRequirementSource(
        std::filesystem::path storePath,
        std::shared_ptr<IRequirementResolveInputSource> inputSource,
        std::shared_ptr<materialization::ITrustedMaterializationDecisionSource>
            materializationSource = {},
        std::filesystem::path materializationInstancesRoot = {})
        : store_(std::move(storePath)), inputSource_(std::move(inputSource)),
          materializationSource_(std::move(materializationSource)),
          materializationInstancesRoot_(std::move(materializationInstancesRoot)) {}

    RequirementSnapshotDiagnostic resolveCurrent(
        TrustedRequirementSnapshot& output) override;
    std::shared_ptr<materialization::ITrustedMaterializationDecisionSource>
    trustedMaterializationDecisionSource() const override {
        return materializationSource_;
    }
    std::filesystem::path trustedMaterializationInstancesRoot() const override {
        return materializationInstancesRoot_;
    }

private:
    GameRuntimeRequirementStore store_;
    std::shared_ptr<IRequirementResolveInputSource> inputSource_;
    std::shared_ptr<materialization::ITrustedMaterializationDecisionSource>
        materializationSource_;
    std::filesystem::path materializationInstancesRoot_;
};

// Captures the same bounded local provider/catalog/technical-evidence inputs in
// every process. The production implementation is read-only: it refreshes native
// providers, reads the fixed local manual-game and compatibility stores, derives
// the current environment identity, and never consumes community data as runtime
// authority. Protected approvals remain empty until v1 has a persisted exact
// approval source.
std::shared_ptr<IRequirementResolveInputSource>
makeProductionRequirementResolveInputSource();

// Uses only defaultGameRuntimeRequirementStorePath(). If the fixed production
// path or native input source is unavailable, the returned source remains
// non-null but resolves fail-closed; callers never fall back to a permissive or
// caller-selected store.
std::shared_ptr<ITrustedRequirementSource>
makeDefaultProductionTrustedRequirementSource();

// Launcher projection helper. A failed current resolve always publishes an empty
// requirement vector so stale requirements from an earlier UI snapshot cannot
// survive a missing/corrupt/stale authority refresh.
RequirementSnapshotDiagnostic resolveCurrentRequirementProjection(
    ITrustedRequirementSource& source,
    std::vector<plan::GameRuntimeRequirement>& output);

std::string_view requirementStoreCodeName(RequirementStoreCode code) noexcept;
std::string_view requirementResolveCodeName(RequirementResolveCode code) noexcept;
std::string_view requirementSnapshotCodeName(RequirementSnapshotCode code) noexcept;
std::string_view trustedPlanRequirementCodeName(TrustedPlanRequirementCode code) noexcept;

} // namespace hydra::requirement
