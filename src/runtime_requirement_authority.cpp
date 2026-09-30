#include "hydra/runtime_requirement_authority.hpp"

#include "hydra/profile_schema.hpp"
#include "hydra/provider_launch_plan.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace hydra::requirement {
namespace {

RuntimeRequirementAuthorityDiagnostic failure(
    RuntimeRequirementAuthorityCode code,
    std::string message) {
    return {code, std::move(message)};
}

bool wideToUtf8(std::wstring_view input, std::string& output) {
    std::string converted;
    converted.reserve(input.size());
    auto append = [&](std::uint32_t codePoint) {
        if (codePoint <= 0x7fu) {
            converted.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ffu) {
            converted.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
            converted.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else if (codePoint <= 0xffffu) {
            converted.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
            converted.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            converted.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else {
            converted.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
            converted.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
            converted.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            converted.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        }
    };

    for (std::size_t index = 0u; index < input.size(); ++index) {
        std::uint32_t codePoint = static_cast<std::uint32_t>(input[index]);
        if constexpr (sizeof(wchar_t) == 2u) {
            if (codePoint >= 0xd800u && codePoint <= 0xdbffu) {
                if (index + 1u >= input.size()) return false;
                const auto low = static_cast<std::uint32_t>(input[++index]);
                if (low < 0xdc00u || low > 0xdfffu) return false;
                codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u) +
                            (low - 0xdc00u);
            } else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu) {
                return false;
            }
        }
        if (codePoint > 0x10ffffu ||
            (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            return false;
        }
        append(codePoint);
    }
    output = std::move(converted);
    return true;
}

bool knownEvidenceOrigin(metrics::EvidenceOrigin value) noexcept {
    switch (value) {
    case metrics::EvidenceOrigin::Synthetic:
    case metrics::EvidenceOrigin::ControlledProcess:
    case metrics::EvidenceOrigin::Physical:
        return true;
    }
    return false;
}

bool knownEvidenceVerdict(metrics::EvidenceVerdict value) noexcept {
    switch (value) {
    case metrics::EvidenceVerdict::InsufficientEvidence:
    case metrics::EvidenceVerdict::Pass:
    case metrics::EvidenceVerdict::Fail:
        return true;
    }
    return false;
}

bool knownCapabilityOutcome(metrics::CapabilityOutcome value) noexcept {
    switch (value) {
    case metrics::CapabilityOutcome::NotRequired:
    case metrics::CapabilityOutcome::Success:
    case metrics::CapabilityOutcome::Unsupported:
    case metrics::CapabilityOutcome::Failed:
    case metrics::CapabilityOutcome::MissingEvidence:
        return true;
    }
    return false;
}

bool knownFinalState(metrics::SessionFinalState value) noexcept {
    switch (value) {
    case metrics::SessionFinalState::Running:
    case metrics::SessionFinalState::ReturnedToWindows:
    case metrics::SessionFinalState::RecoveryRequired:
        return true;
    }
    return false;
}

bool capabilityFailed(metrics::CapabilityOutcome value) noexcept {
    return value == metrics::CapabilityOutcome::Failed;
}

bool capabilityComplete(metrics::CapabilityOutcome value) noexcept {
    return value == metrics::CapabilityOutcome::NotRequired ||
           value == metrics::CapabilityOutcome::Success;
}

bool seatFailed(const metrics::SeatSessionMetrics& seat) noexcept {
    return capabilityFailed(seat.controller) || capabilityFailed(seat.audio);
}

bool seatComplete(const metrics::SeatSessionMetrics& seat) noexcept {
    return seat.processStarted && seat.windowOwnershipVerified &&
           seat.displayPlacementVerified && seat.inputRouteReady &&
           capabilityComplete(seat.controller) && capabilityComplete(seat.audio);
}

metrics::EvidenceVerdict expectedIsolationVerdict(
    const InputMetricsReport& input,
    bool receiverComplete,
    bool lossFree) noexcept {
    if (input.crossSeatEvents != 0u || input.crossProcessEvents != 0u) {
        return metrics::EvidenceVerdict::Fail;
    }
    if (!receiverComplete || !lossFree || input.missingStageEvents != 0u ||
        input.completeInputEvents != input.uniqueInputEvents ||
        input.uniqueInputEvents == 0u) {
        return metrics::EvidenceVerdict::InsufficientEvidence;
    }
    return metrics::EvidenceVerdict::Pass;
}

metrics::EvidenceVerdict expectedSessionVerdict(
    const metrics::SessionMetricsReport& report) noexcept {
    if (report.isolationVerdict == metrics::EvidenceVerdict::Fail ||
        report.finalState == metrics::SessionFinalState::RecoveryRequired ||
        (report.rollbackAttempted && !report.rollbackVerified) ||
        (report.finalState == metrics::SessionFinalState::ReturnedToWindows &&
         (!report.rollbackAttempted || !report.rollbackVerified)) ||
        std::any_of(report.seats.begin(), report.seats.end(), seatFailed)) {
        return metrics::EvidenceVerdict::Fail;
    }
    if (report.isolationVerdict != metrics::EvidenceVerdict::Pass ||
        std::any_of(report.seats.begin(), report.seats.end(),
                    [](const auto& seat) { return !seatComplete(seat); })) {
        return metrics::EvidenceVerdict::InsufficientEvidence;
    }
    return metrics::EvidenceVerdict::Pass;
}

RuntimeRequirementAuthorityDiagnostic validateSessionMetricsReport(
    const metrics::SessionMetricsReport& report) {
    if (report.schemaVersion != metrics::kSessionMetricsSchemaVersion ||
        report.planFingerprint == 0u ||
        report.seats.size() < metrics::kMinimumSessionMetricSeats ||
        report.seats.size() > metrics::kMaximumSessionMetricSeats ||
        report.input.schemaVersion != kInputMetricsSchemaVersion ||
        !knownEvidenceOrigin(report.origin) ||
        !knownEvidenceVerdict(report.isolationVerdict) ||
        !knownEvidenceVerdict(report.sessionVerdict) ||
        !knownFinalState(report.finalState) ||
        (report.rollbackVerified && !report.rollbackAttempted)) {
        return failure(RuntimeRequirementAuthorityCode::InvalidSessionMetrics,
                       "session metrics report is not a valid canonical one/two-Seat report");
    }

    std::set<SeatId> seatIds;
    SeatId previousSeat = 0u;
    std::uint64_t maximumLaunch = 0u;
    std::uint64_t maximumStop = 0u;
    std::uint64_t maximumRollback = 0u;
    for (const auto& seat : report.seats) {
        if (seat.seatId == 0u || !seatIds.insert(seat.seatId).second ||
            !knownCapabilityOutcome(seat.controller) ||
            !knownCapabilityOutcome(seat.audio) ||
            (previousSeat != 0u && seat.seatId <= previousSeat)) {
            return failure(RuntimeRequirementAuthorityCode::InvalidSessionMetrics,
                           "session metrics Seat records are invalid, duplicate, or non-canonical");
        }
        previousSeat = seat.seatId;
        maximumLaunch = std::max(maximumLaunch, seat.launchDurationMicros);
        maximumStop = std::max(maximumStop, seat.stopDurationMicros);
        maximumRollback = std::max(maximumRollback, seat.rollbackDurationMicros);
    }

    const auto& input = report.input;
    if (input.completeInputEvents > input.uniqueInputEvents ||
        input.receiverVerifiedEvents > input.uniqueInputEvents ||
        input.missingStageEvents > input.uniqueInputEvents ||
        input.missingReceiverEvidenceEvents > input.uniqueInputEvents ||
        input.crossSeatEvents > input.uniqueInputEvents ||
        input.crossProcessEvents > input.uniqueInputEvents) {
        return failure(RuntimeRequirementAuthorityCode::InvalidSessionMetrics,
                       "session metrics input counters are internally inconsistent");
    }

    const bool receiverComplete =
        input.uniqueInputEvents != 0u &&
        input.receiverVerifiedEvents == input.uniqueInputEvents &&
        input.missingReceiverEvidenceEvents == 0u;
    const bool lossFree =
        input.queueDroppedFrames == 0u &&
        input.recorderRotationDrops == 0u &&
        input.recorderContentionDrops == 0u &&
        input.recorderInvalidSamples == 0u;
    const auto isolation = expectedIsolationVerdict(input, receiverComplete, lossFree);

    if (report.receiverEvidenceComplete != receiverComplete ||
        report.lossFreeEvidence != lossFree ||
        report.isolationVerdict != isolation ||
        report.maximumLaunchDurationMicros != maximumLaunch ||
        report.maximumStopDurationMicros != maximumStop ||
        report.maximumRollbackDurationMicros != maximumRollback) {
        return failure(RuntimeRequirementAuthorityCode::InvalidSessionMetrics,
                       "session metrics derived evidence fields do not match their source observations");
    }

    const auto session = expectedSessionVerdict(report);
    const bool physicalEligible =
        report.origin == metrics::EvidenceOrigin::Physical &&
        session == metrics::EvidenceVerdict::Pass;
    if (report.sessionVerdict != session ||
        report.physicalValidationEligible != physicalEligible) {
        return failure(RuntimeRequirementAuthorityCode::InvalidSessionMetrics,
                       "session metrics verdict/physical eligibility was not derived from the report facts");
    }
    return {};
}

RuntimeRequirementAuthorityDiagnostic canonicalEvidenceForReport(
    const RuntimeRequirementAuthorityReview& review,
    compat::CompatibilityResult& canonical) {
    canonical = review.evidence;
    const auto canonicalized = compat::canonicalizeCompatibilityResult(canonical);
    if (!canonicalized.succeeded()) {
        return failure(RuntimeRequirementAuthorityCode::InvalidCompatibilityEvidence,
                       "compatibility result is invalid: " + canonicalized.message);
    }
    if (canonical != review.evidence) {
        return failure(RuntimeRequirementAuthorityCode::InvalidCompatibilityEvidence,
                       "compatibility result must already be in canonical local form");
    }
    if (canonical.origin == compat::ResultOrigin::Synthetic ||
        canonical.origin == compat::ResultOrigin::ImportedCommunity) {
        return failure(RuntimeRequirementAuthorityCode::EvidenceOriginRejected,
                       "Synthetic/ImportedCommunity evidence cannot publish local runtime authority");
    }

    compat::LocalEvidenceContext context;
    context.resultId = canonical.resultId;
    context.timestampClass = canonical.timestampClass;
    context.timestampBucket = canonical.timestampBucket;
    context.gameId = canonical.gameId;
    context.providerId = canonical.providerId;
    context.providerAppId = canonical.providerAppId;
    context.gameVersion = canonical.gameVersion;
    context.hydraSeatVersion = canonical.hydraSeatVersion;
    context.hydraSeatBuild = canonical.hydraSeatBuild;
    context.windowsBuildClass = canonical.windowsBuildClass;
    context.architecture = canonical.architecture;
    context.scenario = canonical.scenario;
    context.protectedExperimental = canonical.protectedExperimental;
    context.setupRevision = canonical.setupRevision;
    context.backends = canonical.backends;
    context.provenanceId = canonical.provenanceId;
    context.provenanceRevision = canonical.provenanceRevision;

    compat::CompatibilityResult rebuilt;
    const auto rebuiltStatus = compat::buildCompatibilityResultFromSessionMetrics(
        context, review.report, rebuilt);
    if (!rebuiltStatus.succeeded() || rebuilt != canonical) {
        return failure(RuntimeRequirementAuthorityCode::EvidenceReportMismatch,
                       "compatibility result is not the exact canonical projection of the supplied session metrics report");
    }

    if (canonical.origin == compat::ResultOrigin::Physical &&
        !review.report.physicalValidationEligible) {
        return failure(RuntimeRequirementAuthorityCode::PhysicalEvidenceIneligible,
                       "Physical authority requires an existing physically eligible session report; the writer never sets eligibility");
    }
    return {};
}

RuntimeRequirementAuthorityDiagnostic validateCurrentIdentity(
    const RuntimeRequirementAuthorityReview& review,
    const compat::CompatibilityResult& evidence,
    std::optional<std::string>& gameVersionUtf8) {
    profile::GameRecordDocument gameDocument;
    gameDocument.games.push_back(review.game.game);
    const auto gameValidation = profile::validateGameRecordDocument(gameDocument);
    if (!gameValidation.succeeded()) {
        return failure(RuntimeRequirementAuthorityCode::InvalidCatalogGame,
                       "current catalog Game is invalid: " + gameValidation.message);
    }
    if (review.game.staleness != catalog::CatalogStaleness::Current ||
        review.game.mergedCandidateCount == 0u) {
        return failure(RuntimeRequirementAuthorityCode::StaleCatalogGame,
                       "runtime requirement authority can be published only for a current reconciled catalog Game");
    }

    const auto& game = review.game.game;
    if (review.provider.providerId != game.providerId ||
        evidence.providerId != game.providerId ||
        evidence.gameId != game.gameId ||
        evidence.providerAppId != game.providerAppId) {
        return failure(RuntimeRequirementAuthorityCode::ProviderMismatch,
                       "current Game/provider/AppID identity does not exactly match the local evidence");
    }
    if (review.provider.availability != provider::ProviderAvailability::Available) {
        return failure(RuntimeRequirementAuthorityCode::ProviderUnavailable,
                       "current provider descriptor is not available");
    }
    if (review.provider.metadataRevision == 0u) {
        return failure(RuntimeRequirementAuthorityCode::InvalidProviderRevision,
                       "current provider descriptor has no valid metadata revision");
    }

    gameVersionUtf8.reset();
    if (game.localVersion) {
        std::string converted;
        if (!wideToUtf8(*game.localVersion, converted)) {
            return failure(RuntimeRequirementAuthorityCode::InvalidCatalogGame,
                           "current Game version cannot be represented as exact UTF-8 identity");
        }
        gameVersionUtf8 = std::move(converted);
    }
    if (evidence.gameVersion != gameVersionUtf8) {
        return failure(RuntimeRequirementAuthorityCode::ProviderMismatch,
                       "current Game version does not exactly match the compatibility evidence");
    }

    if (game.compatibility) {
        if (evidence.provenanceRevision >
                (std::numeric_limits<std::uint32_t>::max)() ||
            game.compatibility->recordId != evidence.resultId ||
            game.compatibility->provenance != evidence.provenanceId ||
            game.compatibility->evidenceRevision !=
                static_cast<std::uint32_t>(evidence.provenanceRevision)) {
            return failure(RuntimeRequirementAuthorityCode::ProviderMismatch,
                           "current Game compatibility reference does not identify the exact supplied local result");
        }
    }
    return {};
}

bool sameEvidenceIdentity(const LocalRequirementEvidenceRecord& record,
                          const compat::CompatibilityResult& evidence) noexcept {
    return record.evidenceResultId == evidence.resultId &&
           record.evidenceProvenanceId == evidence.provenanceId &&
           record.evidenceProvenanceRevision == evidence.provenanceRevision;
}

bool exactStoredSubjectMatches(const LocalRequirementEvidenceRecord& record,
                               const RuntimeRequirementAuthorityReview& review,
                               const std::optional<std::string>& version) noexcept {
    const auto& game = review.game.game;
    return record.gameId == game.gameId &&
           record.providerId == game.providerId &&
           record.providerAppId == game.providerAppId &&
           record.gameVersionUtf8 == version &&
           record.executableSha256 == game.executableSha256 &&
           record.catalogCompatibility == game.compatibility &&
           record.providerMetadataRevision == review.provider.metadataRevision;
}

class DescriptorOnlyProviderAdapter final : public provider::LauncherProviderAdapter {
public:
    explicit DescriptorOnlyProviderAdapter(provider::ProviderDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}

    provider::ProviderDescriptor descriptor() const noexcept override {
        return descriptor_;
    }
    provider::DiscoveryResponse discoverInstalledGames() noexcept override {
        return {provider::ProviderResult::UnsupportedOperation,
                descriptor_.metadataRevision, {}, {}};
    }
    provider::AccountReferenceResponse listAccountReferences() noexcept override {
        return {provider::ProviderResult::UnsupportedOperation,
                descriptor_.metadataRevision, {}, {}};
    }
    provider::LaunchResponse buildLaunchRequest(
        const provider::LaunchSelection&) noexcept override {
        return {provider::ProviderResult::UnsupportedOperation, {}, {}};
    }
    provider::ProcessIdentificationResponse identifyProcesses(
        const provider::ProcessIdentificationQuery&) noexcept override {
        return {provider::ProviderResult::UnsupportedOperation,
                descriptor_.metadataRevision, {}, {}};
    }

private:
    provider::ProviderDescriptor descriptor_;
};

RuntimeRequirementAuthorityDiagnostic validateCandidateThroughResolver(
    const RequirementEvidenceDocument& document,
    const RuntimeRequirementAuthorityReview& review,
    const compat::CompatibilityResult& evidence) {
    DescriptorOnlyProviderAdapter adapter(review.provider);
    const std::vector<plan::ProviderAdapterBinding> bindings{
        {review.game.game.providerId, &adapter, review.game.game.providerAppId},
    };
    const std::vector<compat::CompatibilityResult> evidenceSet{evidence};

    RequirementResolveContext context;
    context.referenceMonth = evidence.timestampBucket.substr(0u, 7u);
    context.staleAfterMonths = 0u;
    context.hydraSeatVersion = evidence.hydraSeatVersion;
    context.hydraSeatBuild = evidence.hydraSeatBuild;
    context.windowsBuildClass = evidence.windowsBuildClass;
    context.architecture = evidence.architecture;
    context.trust = evidence.origin == compat::ResultOrigin::Physical
        ? LocalEvidenceTrust::PhysicalOnly
        : LocalEvidenceTrust::ControlledOrPhysical;

    catalog::LocalGameCatalog currentCatalog;
    currentCatalog.entries.push_back(review.game);
    TrustedRequirementSnapshot snapshot;
    const auto resolved = resolveTrustedGameRuntimeRequirements(
        document, currentCatalog, bindings, evidenceSet, {}, context, snapshot);
    if (!resolved.succeeded()) {
        return failure(RuntimeRequirementAuthorityCode::InvalidPublishedDocument,
                       "candidate authority could not be revalidated by the production resolver: " +
                           resolved.message);
    }

    if (review.requirements.highRisk) {
        if (!snapshot.requirements.empty() || !snapshot.authorities.empty() ||
            snapshot.blockedGames.size() != 1u ||
            snapshot.blockedGames.front().gameId != review.game.game.gameId ||
            snapshot.blockedGames.front().code !=
                RequirementResolveCode::ProtectedApprovalRequired) {
            return failure(RuntimeRequirementAuthorityCode::InvalidPublishedDocument,
                           "high-risk authority must remain blocked solely by the separate exact protected approval gate");
        }
        return {};
    }

    if (snapshot.requirements.size() == 1u && snapshot.authorities.size() == 1u &&
        snapshot.blockedGames.empty()) {
        return {};
    }
    if (snapshot.blockedGames.size() == 1u &&
        snapshot.blockedGames.front().code ==
            RequirementResolveCode::InsufficientCapabilityEvidence) {
        return failure(RuntimeRequirementAuthorityCode::InsufficientCapabilityEvidence,
                       "the exact local observations do not prove every requested runtime capability");
    }
    const std::string reason = snapshot.blockedGames.empty()
        ? "candidate did not resolve to exactly one current authority"
        : snapshot.blockedGames.front().message;
    return failure(RuntimeRequirementAuthorityCode::InvalidPublishedDocument,
                   "candidate authority was rejected by the production resolver: " + reason);
}

} // namespace

launch::Capabilities deriveRuntimeRequirementCapabilities(
    const RuntimeRequirementAuthorityReview& review) noexcept {
    launch::Capabilities capabilities{};
    capabilities.process = false;
    capabilities.window = false;
    capabilities.display = false;
    capabilities.input = false;
    capabilities.controller = false;
    capabilities.audio = false;
    capabilities.recovery = false;

    if (review.report.seats.empty()) return capabilities;
    const bool allProcesses = std::all_of(
        review.report.seats.begin(), review.report.seats.end(),
        [](const auto& seat) { return seat.processStarted; });
    capabilities.process = review.provider.capabilities.launchRequests &&
                           allProcesses &&
                           review.evidence.launch == compat::EvidenceStatus::Pass;
    if (!capabilities.process) return capabilities;

    capabilities.window = std::all_of(
        review.report.seats.begin(), review.report.seats.end(),
        [](const auto& seat) { return seat.windowOwnershipVerified; });
    capabilities.display = std::all_of(
        review.report.seats.begin(), review.report.seats.end(),
        [](const auto& seat) { return seat.displayPlacementVerified; });
    capabilities.input = review.report.receiverEvidenceComplete &&
                         review.report.lossFreeEvidence &&
                         review.report.isolationVerdict == metrics::EvidenceVerdict::Pass &&
                         review.evidence.inputIsolation == compat::EvidenceStatus::Pass &&
                         std::all_of(review.report.seats.begin(), review.report.seats.end(),
                                     [](const auto& seat) { return seat.inputRouteReady; });
    capabilities.controller =
        review.evidence.controller == compat::EvidenceStatus::Pass &&
        std::all_of(review.report.seats.begin(), review.report.seats.end(),
                    [](const auto& seat) {
                        return seat.controller == metrics::CapabilityOutcome::Success;
                    });
    capabilities.audio =
        review.evidence.audio == compat::EvidenceStatus::Pass &&
        std::all_of(review.report.seats.begin(), review.report.seats.end(),
                    [](const auto& seat) {
                        return seat.audio == metrics::CapabilityOutcome::Success;
                    });
    capabilities.recovery =
        review.report.finalState == metrics::SessionFinalState::ReturnedToWindows &&
        review.report.rollbackAttempted && review.report.rollbackVerified &&
        review.evidence.cleanExit == compat::EvidenceStatus::Pass &&
        review.evidence.rollback == compat::EvidenceStatus::Pass;
    return capabilities;
}

std::string deterministicRuntimeRequirementRecordId(
    const catalog::LocalGameCatalogEntry& game) {
    // buildLocalGameCatalog already collision-checks distinct canonical Game
    // identities before publishing this bounded ID. It is therefore a stronger
    // persistent identity than any absolute executable path available here.
    return game.game.gameId;
}

RuntimeRequirementAuthorityDiagnostic publishRuntimeRequirementAuthority(
    const RuntimeRequirementAuthorityReview& review,
    const std::filesystem::path& storePath,
    LocalRequirementEvidenceRecord* published) {
    if (published != nullptr) *published = {};
    if (storePath.empty()) {
        return failure(RuntimeRequirementAuthorityCode::StorePathUnavailable,
                       "runtime requirement authority store path is empty");
    }

    const auto reportStatus = validateSessionMetricsReport(review.report);
    if (!reportStatus.succeeded()) return reportStatus;

    compat::CompatibilityResult evidence;
    const auto evidenceStatus = canonicalEvidenceForReport(review, evidence);
    if (!evidenceStatus.succeeded()) return evidenceStatus;

    std::optional<std::string> gameVersionUtf8;
    const auto identityStatus = validateCurrentIdentity(
        review, evidence, gameVersionUtf8);
    if (!identityStatus.succeeded()) return identityStatus;

    GameRuntimeRequirementStore store(storePath);
    RequirementEvidenceDocument document;
    const auto loaded = store.load(document);
    if (!loaded.succeeded()) {
        return failure(RuntimeRequirementAuthorityCode::StoreLoadFailed,
                       "existing runtime requirement authority store failed validation: " +
                           loaded.message);
    }
    if (!loaded.found()) document = {};

    std::optional<std::size_t> replacementIndex;
    for (std::size_t index = 0u; index < document.records.size(); ++index) {
        if (document.records[index].gameId == review.game.game.gameId) {
            replacementIndex = index;
            break;
        }
    }

    std::uint64_t revision = 1u;
    if (replacementIndex) {
        const auto& previous = document.records[*replacementIndex];
        if (previous.revision == (std::numeric_limits<std::uint64_t>::max)()) {
            return failure(RuntimeRequirementAuthorityCode::RevisionOverflow,
                           "existing runtime requirement authority revision cannot advance safely");
        }
        if (sameEvidenceIdentity(previous, evidence) &&
            !exactStoredSubjectMatches(previous, review, gameVersionUtf8)) {
            return failure(RuntimeRequirementAuthorityCode::EvidenceRebindRejected,
                           "the same local evidence identity cannot be rebound to a different Game/hash/provider revision");
        }
        revision = previous.revision + 1u;
    }

    LocalRequirementEvidenceRecord record;
    record.recordId = deterministicRuntimeRequirementRecordId(review.game);
    record.revision = revision;
    record.gameId = review.game.game.gameId;
    record.providerId = review.game.game.providerId;
    record.providerAppId = review.game.game.providerAppId;
    record.gameVersionUtf8 = gameVersionUtf8;
    record.executableSha256 = review.game.game.executableSha256;
    record.catalogCompatibility = review.game.game.compatibility;
    record.providerMetadataRevision = review.provider.metadataRevision;
    record.evidenceResultId = evidence.resultId;
    record.evidenceProvenanceId = evidence.provenanceId;
    record.evidenceProvenanceRevision = evidence.provenanceRevision;
    record.validatedSeatCount = static_cast<std::uint8_t>(review.report.seats.size());
    record.requirements = review.requirements;
    record.capabilities = deriveRuntimeRequirementCapabilities(review);

    RequirementEvidenceDocument candidate = document;
    if (replacementIndex) {
        candidate.records[*replacementIndex] = record;
    } else {
        candidate.records.push_back(record);
    }

    const auto candidateValidation = validateRequirementEvidenceDocument(candidate);
    if (!candidateValidation.succeeded()) {
        return failure(RuntimeRequirementAuthorityCode::InvalidPublishedDocument,
                       "candidate runtime requirement document is invalid: " +
                           candidateValidation.message);
    }

    const auto resolverValidation = validateCandidateThroughResolver(
        candidate, review, evidence);
    if (!resolverValidation.succeeded()) return resolverValidation;

    const auto saved = store.save(candidate);
    if (!saved.succeeded()) {
        return failure(RuntimeRequirementAuthorityCode::StoreSaveFailed,
                       "runtime requirement authority transactional save failed: " +
                           saved.message);
    }
    if (published != nullptr) *published = std::move(record);
    return {};
}

RuntimeRequirementAuthorityDiagnostic publishRuntimeRequirementAuthorityToDefaultStore(
    const RuntimeRequirementAuthorityReview& review,
    LocalRequirementEvidenceRecord* published) {
    std::string error;
    const auto path = defaultGameRuntimeRequirementStorePath(&error);
    if (!path) {
        return failure(RuntimeRequirementAuthorityCode::StorePathUnavailable,
                       error.empty()
                           ? "default runtime requirement authority store path is unavailable"
                           : std::move(error));
    }
    return publishRuntimeRequirementAuthority(review, *path, published);
}

std::string_view runtimeRequirementAuthorityCodeName(
    RuntimeRequirementAuthorityCode code) noexcept {
    switch (code) {
    case RuntimeRequirementAuthorityCode::Success: return "success";
    case RuntimeRequirementAuthorityCode::InvalidCatalogGame: return "invalid-catalog-game";
    case RuntimeRequirementAuthorityCode::StaleCatalogGame: return "stale-catalog-game";
    case RuntimeRequirementAuthorityCode::ProviderMismatch: return "provider-mismatch";
    case RuntimeRequirementAuthorityCode::ProviderUnavailable: return "provider-unavailable";
    case RuntimeRequirementAuthorityCode::InvalidProviderRevision: return "invalid-provider-revision";
    case RuntimeRequirementAuthorityCode::InvalidCompatibilityEvidence: return "invalid-compatibility-evidence";
    case RuntimeRequirementAuthorityCode::InvalidSessionMetrics: return "invalid-session-metrics";
    case RuntimeRequirementAuthorityCode::EvidenceOriginRejected: return "evidence-origin-rejected";
    case RuntimeRequirementAuthorityCode::EvidenceReportMismatch: return "evidence-report-mismatch";
    case RuntimeRequirementAuthorityCode::PhysicalEvidenceIneligible: return "physical-evidence-ineligible";
    case RuntimeRequirementAuthorityCode::InsufficientCapabilityEvidence: return "insufficient-capability-evidence";
    case RuntimeRequirementAuthorityCode::EvidenceRebindRejected: return "evidence-rebind-rejected";
    case RuntimeRequirementAuthorityCode::StorePathUnavailable: return "store-path-unavailable";
    case RuntimeRequirementAuthorityCode::StoreLoadFailed: return "store-load-failed";
    case RuntimeRequirementAuthorityCode::RevisionOverflow: return "revision-overflow";
    case RuntimeRequirementAuthorityCode::InvalidPublishedDocument: return "invalid-published-document";
    case RuntimeRequirementAuthorityCode::StoreSaveFailed: return "store-save-failed";
    }
    return "unknown";
}

} // namespace hydra::requirement
