#include "hydra/runtime_requirement_authority.hpp"

#include "hydra/compatibility_result.hpp"
#include "hydra/input_metrics.hpp"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::requirement;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename T>
concept HasCallerCapabilities = requires(T value) {
    value.capabilities;
};

static_assert(!HasCallerCapabilities<RuntimeRequirementAuthorityReview>,
              "runtime requirement review must never accept caller capabilities");

class TempStore final {
public:
    explicit TempStore(std::string_view label) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() /
                 ("hydraseat-runtime-authority-" + std::string(label) + "-" +
                  std::to_string(stamp) + ".json");
        cleanup();
    }

    ~TempStore() { cleanup(); }

    const std::filesystem::path& path() const noexcept { return m_path; }

    void cleanup() noexcept {
        std::error_code error;
        std::filesystem::remove(m_path, error);
        auto temporary = m_path;
        temporary += L".tmp";
        error.clear();
        std::filesystem::remove(temporary, error);
    }

private:
    std::filesystem::path m_path;
};

provider::ProviderDescriptor providerDescriptor(std::uint64_t revision = 41u) {
    provider::ProviderDescriptor descriptor;
    descriptor.providerId = "fake";
    descriptor.availability = provider::ProviderAvailability::Available;
    descriptor.metadataRevision = revision;
    descriptor.capabilities.installedGameDiscovery = true;
    descriptor.capabilities.launchRequests = true;
    descriptor.capabilities.offlineLaunch = true;
    descriptor.capabilities.processIdentification = true;
    return descriptor;
}

profile::GameRecord gameRecord(std::string gameId = "game:a") {
    profile::GameRecord game;
    game.gameId = std::move(gameId);
    game.providerId = "fake";
    game.providerAppId = "app-100";
    game.title = L"Fixture Game";
    game.installRoot = L"C:\\Games\\Fixture";
    game.executableCandidates = {L"C:\\Games\\Fixture\\fixture.exe"};
    game.localVersion = L"1.0.7";
    game.executableSha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    game.origin = profile::GameOrigin::Discovered;
    return game;
}

catalog::LocalGameCatalogEntry catalogEntry(std::string gameId = "game:a") {
    catalog::LocalGameCatalogEntry entry;
    entry.game = gameRecord(std::move(gameId));
    entry.architecture = catalog::ExecutableArchitecture::X64;
    entry.staleness = catalog::CatalogStaleness::Current;
    entry.mergedCandidateCount = 1u;
    return entry;
}

InputMetricSample inputSample(std::uint64_t correlation,
                              InputMetricStage stage,
                              std::uint64_t timestamp,
                              std::uint32_t seat,
                              std::uint32_t process) {
    InputMetricSample sample;
    sample.correlationId = correlation;
    sample.stage = stage;
    sample.timestampMicros = timestamp;
    sample.expectedSeatId = seat;
    sample.targetProcessId = process;
    sample.eventClass = InputMetricEventClass::Key;
    if (stage == InputMetricStage::TargetApplied ||
        stage == InputMetricStage::TargetQueried) {
        sample.receivingSeatId = seat;
        sample.receivingProcessId = process;
    }
    return sample;
}

void appendCompleteInputEvent(InputMetricsSnapshot& snapshot,
                              std::uint64_t correlation,
                              std::uint64_t base,
                              std::uint32_t seat,
                              std::uint32_t process) {
    snapshot.samples.push_back(inputSample(correlation, InputMetricStage::PhysicalObserved,
                                           base, seat, process));
    snapshot.samples.push_back(inputSample(correlation, InputMetricStage::RouteEnqueued,
                                           base + 10u, seat, process));
    snapshot.samples.push_back(inputSample(correlation, InputMetricStage::RouteDequeued,
                                           base + 20u, seat, process));
    snapshot.samples.push_back(inputSample(correlation, InputMetricStage::RouteWritten,
                                           base + 30u, seat, process));
    snapshot.samples.push_back(inputSample(correlation, InputMetricStage::TargetApplied,
                                           base + 40u, seat, process));
    snapshot.samples.push_back(inputSample(correlation, InputMetricStage::TargetQueried,
                                           base + 60u, seat, process));
}

metrics::SeatSessionMetrics seatMetrics(SeatId seatId, std::uint64_t base) {
    metrics::SeatSessionMetrics seat;
    seat.seatId = seatId;
    seat.launchDurationMicros = base + 100u;
    seat.stopDurationMicros = base + 200u;
    seat.rollbackDurationMicros = base + 300u;
    seat.processStarted = true;
    seat.windowOwnershipVerified = true;
    seat.displayPlacementVerified = true;
    seat.inputRouteReady = true;
    seat.controller = metrics::CapabilityOutcome::Success;
    seat.audio = metrics::CapabilityOutcome::Success;
    return seat;
}

metrics::SessionMetricsReport sessionReport(
    metrics::EvidenceOrigin origin = metrics::EvidenceOrigin::Physical) {
    metrics::SessionMetricsBuildInput input;
    input.planFingerprint = 0x12345678u;
    input.origin = origin;
    appendCompleteInputEvent(input.input, 1u, 1000u, 1u, 101u);
    appendCompleteInputEvent(input.input, 2u, 2000u, 2u, 202u);
    input.seats = {seatMetrics(2u, 2000u), seatMetrics(1u, 1000u)};
    input.finalState = metrics::SessionFinalState::ReturnedToWindows;
    input.rollbackAttempted = true;
    input.rollbackVerified = true;

    metrics::SessionMetricsReport report;
    check(metrics::buildSessionMetricsReport(input, report) ==
              metrics::SessionMetricsResult::Success,
          "fixture session metrics build succeeds");
    return report;
}

metrics::SessionMetricsReport singleSeatSessionReport(
    metrics::EvidenceOrigin origin = metrics::EvidenceOrigin::Physical) {
    metrics::SessionMetricsBuildInput input;
    input.planFingerprint = 0x23456789u;
    input.origin = origin;
    appendCompleteInputEvent(input.input, 1u, 1000u, 1u, 101u);
    input.seats = {seatMetrics(1u, 1000u)};
    input.finalState = metrics::SessionFinalState::ReturnedToWindows;
    input.rollbackAttempted = true;
    input.rollbackVerified = true;

    metrics::SessionMetricsReport report;
    check(metrics::buildSessionMetricsReport(input, report) ==
              metrics::SessionMetricsResult::Success,
          "single-Seat fixture session metrics build succeeds");
    return report;
}

compat::CompatibilityResult compatibilityFor(
    const metrics::SessionMetricsReport& report,
    std::string gameId = "game:a",
    std::string resultId = "result-local-a",
    std::string provenanceId = "local-runtime-evidence",
    std::uint64_t provenanceRevision = 7u,
    compat::Scenario scenario = compat::Scenario::DifferentGames,
    bool protectedExperimental = false) {
    compat::LocalEvidenceContext context;
    context.resultId = std::move(resultId);
    context.timestampClass = compat::TimestampClass::MonthBucket;
    context.timestampBucket = "2026-08";
    context.gameId = std::move(gameId);
    context.providerId = "fake";
    context.providerAppId = "app-100";
    context.gameVersion = "1.0.7";
    context.hydraSeatVersion = "0.1.0";
    context.hydraSeatBuild = "build-a";
    context.windowsBuildClass = "win10-2009";
    context.architecture = "x64";
    context.scenario = scenario;
    context.protectedExperimental = protectedExperimental;
    context.provenanceId = std::move(provenanceId);
    context.provenanceRevision = provenanceRevision;

    compat::CompatibilityResult result;
    const auto built = compat::buildCompatibilityResultFromSessionMetrics(
        context, report, result);
    check(built.succeeded(), "fixture compatibility result builds from exact report");
    return result;
}

launch::Requirements allRequirements(bool highRisk = false) {
    launch::Requirements requirements;
    requirements.display = true;
    requirements.keyboard = true;
    requirements.mouse = true;
    requirements.controller = true;
    requirements.audioOutput = true;
    requirements.windowOwnership = true;
    requirements.recovery = true;
    requirements.highRisk = highRisk;
    return requirements;
}

RuntimeRequirementAuthorityReview reviewFor(
    metrics::EvidenceOrigin origin = metrics::EvidenceOrigin::Physical,
    std::string gameId = "game:a",
    std::string resultId = "result-local-a",
    std::string provenanceId = "local-runtime-evidence",
    std::uint64_t provenanceRevision = 7u) {
    RuntimeRequirementAuthorityReview review;
    review.game = catalogEntry(gameId);
    review.provider = providerDescriptor();
    review.report = sessionReport(origin);
    review.evidence = compatibilityFor(review.report, gameId, std::move(resultId),
                                       std::move(provenanceId), provenanceRevision);
    review.game.game.compatibility = profile::CompatibilityReference{
        review.evidence.resultId,
        review.evidence.provenanceId,
        static_cast<std::uint32_t>(review.evidence.provenanceRevision),
    };
    review.requirements = allRequirements();
    return review;
}

RequirementEvidenceDocument loadDocument(const std::filesystem::path& path) {
    GameRuntimeRequirementStore store(path);
    RequirementEvidenceDocument document;
    const auto loaded = store.load(document);
    check(loaded.succeeded() && loaded.found(), "authority store reload succeeds");
    return document;
}

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

class FakeProvider final : public provider::LauncherProviderAdapter {
public:
    provider::ProviderDescriptor descriptorValue = providerDescriptor();

    provider::ProviderDescriptor descriptor() const noexcept override {
        return descriptorValue;
    }
    provider::DiscoveryResponse discoverInstalledGames() noexcept override {
        return {provider::ProviderResult::UnsupportedOperation,
                descriptorValue.metadataRevision, {}, {}};
    }
    provider::AccountReferenceResponse listAccountReferences() noexcept override {
        return {provider::ProviderResult::UnsupportedOperation,
                descriptorValue.metadataRevision, {}, {}};
    }
    provider::LaunchResponse buildLaunchRequest(
        const provider::LaunchSelection&) noexcept override {
        return {provider::ProviderResult::UnsupportedOperation, {}, {}};
    }
    provider::ProcessIdentificationResponse identifyProcesses(
        const provider::ProcessIdentificationQuery&) noexcept override {
        return {provider::ProviderResult::UnsupportedOperation,
                descriptorValue.metadataRevision, {}, {}};
    }
};

TrustedRequirementSnapshot resolveStored(
    const RequirementEvidenceDocument& stored,
    const RuntimeRequirementAuthorityReview& review,
    LocalEvidenceTrust trust,
    std::span<const ProtectedRuntimeApproval> approvals = {}) {
    FakeProvider provider;
    provider.descriptorValue = review.provider;
    const std::vector<plan::ProviderAdapterBinding> bindings{
        {review.game.game.providerId, &provider, review.game.game.providerAppId},
    };
    const std::vector<compat::CompatibilityResult> evidence{review.evidence};
    catalog::LocalGameCatalog catalog;
    catalog.entries.push_back(review.game);

    RequirementResolveContext context;
    context.referenceMonth = "2026-08";
    context.staleAfterMonths = 0u;
    context.hydraSeatVersion = review.evidence.hydraSeatVersion;
    context.hydraSeatBuild = review.evidence.hydraSeatBuild;
    context.windowsBuildClass = review.evidence.windowsBuildClass;
    context.architecture = review.evidence.architecture;
    context.trust = trust;

    TrustedRequirementSnapshot snapshot;
    const auto status = resolveTrustedGameRuntimeRequirements(
        stored, catalog, bindings, evidence, approvals, context, snapshot);
    check(status.succeeded(), "existing trusted resolver accepts test input envelope");
    return snapshot;
}

void checkStoreUnchanged(const std::filesystem::path& path,
                         const RequirementEvidenceDocument& before,
                         std::string_view message) {
    const auto after = loadDocument(path);
    check(after == before, message);
}

void testFirstPhysicalPublicationAndResolverRoundTrip() {
    TempStore temp("physical");
    auto review = reviewFor();
    LocalRequirementEvidenceRecord published;
    const auto status = publishRuntimeRequirementAuthority(
        review, temp.path(), &published);
    check(status.succeeded(), "first exact Physical authority publication succeeds");
    check(published.recordId == review.game.game.gameId && published.revision == 1u,
          "record ID is deterministic current Game identity and first revision is one");
    check(published.providerId == review.game.game.providerId &&
              published.providerAppId == review.game.game.providerAppId &&
              published.providerMetadataRevision == review.provider.metadataRevision &&
              published.gameVersionUtf8 == std::optional<std::string>{"1.0.7"} &&
              published.executableSha256 == review.game.game.executableSha256 &&
              published.catalogCompatibility == review.game.game.compatibility,
          "published record binds exact current Game/provider/AppID/version/hash/compatibility identity");
    check(published.evidenceResultId == review.evidence.resultId &&
              published.evidenceProvenanceId == review.evidence.provenanceId &&
              published.evidenceProvenanceRevision == review.evidence.provenanceRevision,
          "published record binds exact local evidence provenance");
    check(published.capabilities.process && published.capabilities.window &&
              published.capabilities.display && published.capabilities.input &&
              published.capabilities.controller && published.capabilities.audio &&
              published.capabilities.recovery,
          "complete observations derive every runtime capability without caller authority");

    const auto stored = loadDocument(temp.path());
    check(stored.records.size() == 1u && stored.records.front() == published,
          "transactional store round-trips exact published record");
    const auto snapshot = resolveStored(stored, review, LocalEvidenceTrust::PhysicalOnly);
    check(snapshot.authorities.size() == 1u && snapshot.requirements.size() == 1u &&
              snapshot.blockedGames.empty() &&
              snapshot.authorities.front().evidenceOrigin == compat::ResultOrigin::Physical,
          "new Physical record actually resolves through existing production PhysicalOnly authority");
    check(!snapshot.requirements.front().highRiskApproved,
          "ordinary publication never manufactures protected approval");
}

void testSingleSeatPublicationPreservesValidationScope() {
    TempStore temp("single-seat-scope");
    auto review = reviewFor();
    review.report = singleSeatSessionReport();
    review.evidence = compatibilityFor(
        review.report, "game:a", "result-single-seat", "prov-single-seat", 12u);
    review.game.game.compatibility = profile::CompatibilityReference{
        review.evidence.resultId,
        review.evidence.provenanceId,
        static_cast<std::uint32_t>(review.evidence.provenanceRevision),
    };

    LocalRequirementEvidenceRecord published;
    const auto status = publishRuntimeRequirementAuthority(
        review, temp.path(), &published);
    check(status.succeeded() && published.validatedSeatCount == 1u,
          "single-Seat Physical report publishes authority scoped to exactly one Seat");

    const auto stored = loadDocument(temp.path());
    check(stored.records.size() == 1u &&
              stored.records.front().validatedSeatCount == 1u,
          "transactional requirement store preserves single-Seat validation scope");
    const auto snapshot = resolveStored(stored, review, LocalEvidenceTrust::PhysicalOnly);
    check(snapshot.requirements.size() == 1u &&
              snapshot.requirements.front().validatedSeatCount == 1u &&
              snapshot.authorities.size() == 1u &&
              snapshot.authorities.front().requirement.validatedSeatCount == 1u,
          "fresh PhysicalOnly resolution preserves single-Seat scope into Host authority");
}

void testControlledPublicationRemainsControlled() {
    TempStore temp("controlled");
    auto review = reviewFor(metrics::EvidenceOrigin::ControlledProcess);
    check(publishRuntimeRequirementAuthority(review, temp.path()).succeeded(),
          "exact ControlledProcess-backed authority may be stored");
    const auto stored = loadDocument(temp.path());

    const auto controlled = resolveStored(
        stored, review, LocalEvidenceTrust::ControlledOrPhysical);
    check(controlled.authorities.size() == 1u &&
              controlled.authorities.front().evidenceOrigin ==
                  compat::ResultOrigin::ControlledProcess,
          "ControlledProcess publication retains Controlled origin through resolver");

    const auto physicalOnly = resolveStored(
        stored, review, LocalEvidenceTrust::PhysicalOnly);
    check(physicalOnly.authorities.empty() && physicalOnly.requirements.empty() &&
              physicalOnly.blockedGames.size() == 1u &&
              physicalOnly.blockedGames.front().code ==
                  RequirementResolveCode::UntrustedLocalEvidenceOrigin,
          "production PhysicalOnly resolver rejects stored ControlledProcess authority");
}

void testSyntheticAndCommunityPublicationRejected() {
    TempStore syntheticStore("synthetic");
    auto synthetic = reviewFor(metrics::EvidenceOrigin::Synthetic);
    check(publishRuntimeRequirementAuthority(synthetic, syntheticStore.path()).code ==
              RuntimeRequirementAuthorityCode::EvidenceOriginRejected &&
              !std::filesystem::exists(syntheticStore.path()),
          "Synthetic evidence cannot create runtime authority store");

    TempStore communityStore("community");
    auto community = reviewFor();
    community.evidence.origin = compat::ResultOrigin::ImportedCommunity;
    check(publishRuntimeRequirementAuthority(community, communityStore.path()).code ==
              RuntimeRequirementAuthorityCode::EvidenceOriginRejected &&
              !std::filesystem::exists(communityStore.path()),
          "ImportedCommunity evidence cannot create runtime authority store");
}

void testCapabilitiesAreDerivedOnlyFromObservations() {
    const auto valid = reviewFor();
    auto caps = deriveRuntimeRequirementCapabilities(valid);
    check(caps.process && caps.window && caps.display && caps.input && caps.controller &&
              caps.audio && caps.recovery,
          "complete exact report/result facts derive all capabilities");

    auto changed = valid;
    changed.provider.capabilities.launchRequests = false;
    caps = deriveRuntimeRequirementCapabilities(changed);
    check(!caps.process && !caps.window && !caps.display && !caps.input &&
              !caps.controller && !caps.audio && !caps.recovery,
          "missing provider launch capability prevents process authority and every dependent capability");

    changed = valid;
    changed.report.seats.front().processStarted = false;
    caps = deriveRuntimeRequirementCapabilities(changed);
    check(!caps.process, "missing process-start observation cannot derive process capability");

    changed = valid;
    changed.report.seats.front().windowOwnershipVerified = false;
    caps = deriveRuntimeRequirementCapabilities(changed);
    check(!caps.window && caps.process,
          "missing window ownership observation only removes window capability");

    changed = valid;
    changed.report.seats.front().displayPlacementVerified = false;
    caps = deriveRuntimeRequirementCapabilities(changed);
    check(!caps.display && caps.process,
          "missing display placement observation removes display capability");

    changed = valid;
    changed.report.seats.front().inputRouteReady = false;
    caps = deriveRuntimeRequirementCapabilities(changed);
    check(!caps.input && caps.process,
          "missing Seat input-route observation removes input capability");

    changed = valid;
    changed.report.receiverEvidenceComplete = false;
    caps = deriveRuntimeRequirementCapabilities(changed);
    check(!caps.input, "missing receiver evidence cannot become input isolation capability");

    changed = valid;
    changed.report.seats.front().controller = metrics::CapabilityOutcome::MissingEvidence;
    caps = deriveRuntimeRequirementCapabilities(changed);
    check(!caps.controller, "missing controller observation cannot derive controller capability");

    changed = valid;
    changed.report.seats.front().audio = metrics::CapabilityOutcome::Unsupported;
    caps = deriveRuntimeRequirementCapabilities(changed);
    check(!caps.audio, "unsupported audio observation cannot derive audio capability");

    changed = valid;
    changed.report.rollbackVerified = false;
    caps = deriveRuntimeRequirementCapabilities(changed);
    check(!caps.recovery, "unverified rollback cannot derive recovery capability");
}

void testIdentityMismatchesRejectWithoutMutation() {
    TempStore temp("identity");
    auto base = reviewFor();
    check(publishRuntimeRequirementAuthority(base, temp.path()).succeeded(),
          "identity test seeds a valid authority");
    const auto before = loadDocument(temp.path());

    auto wrongGame = base;
    wrongGame.evidence.gameId = "game:other";
    check(publishRuntimeRequirementAuthority(wrongGame, temp.path()).code ==
              RuntimeRequirementAuthorityCode::ProviderMismatch,
          "Game ID mismatch is rejected");
    checkStoreUnchanged(temp.path(), before, "Game ID mismatch does not mutate prior store");

    auto wrongProvider = base;
    wrongProvider.provider.providerId = "other";
    check(publishRuntimeRequirementAuthority(wrongProvider, temp.path()).code ==
              RuntimeRequirementAuthorityCode::ProviderMismatch,
          "provider descriptor identity mismatch is rejected");
    checkStoreUnchanged(temp.path(), before, "provider mismatch does not mutate prior store");

    auto wrongApp = base;
    wrongApp.evidence.providerAppId = "app-200";
    check(publishRuntimeRequirementAuthority(wrongApp, temp.path()).code ==
              RuntimeRequirementAuthorityCode::ProviderMismatch,
          "provider AppID mismatch is rejected");
    checkStoreUnchanged(temp.path(), before, "AppID mismatch does not mutate prior store");

    auto wrongVersion = base;
    wrongVersion.game.game.localVersion = L"1.0.8";
    check(publishRuntimeRequirementAuthority(wrongVersion, temp.path()).code ==
              RuntimeRequirementAuthorityCode::ProviderMismatch,
          "Game version mismatch is rejected");
    checkStoreUnchanged(temp.path(), before, "version mismatch does not mutate prior store");

    auto wrongHash = base;
    wrongHash.game.game.executableSha256 =
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    check(publishRuntimeRequirementAuthority(wrongHash, temp.path()).code ==
              RuntimeRequirementAuthorityCode::EvidenceRebindRejected,
          "same exact evidence cannot be rebound to a different executable hash");
    checkStoreUnchanged(temp.path(), before, "hash rebind rejection does not mutate prior store");
}

void testProviderRevisionAvailabilityAndEvidenceProvenanceFailures() {
    TempStore temp("provider");
    auto base = reviewFor();
    check(publishRuntimeRequirementAuthority(base, temp.path()).succeeded(),
          "provider test seeds a valid authority");
    const auto before = loadDocument(temp.path());

    auto zeroRevision = base;
    zeroRevision.provider.metadataRevision = 0u;
    check(publishRuntimeRequirementAuthority(zeroRevision, temp.path()).code ==
              RuntimeRequirementAuthorityCode::InvalidProviderRevision,
          "zero provider metadata revision is rejected");
    checkStoreUnchanged(temp.path(), before, "zero provider revision does not mutate prior store");

    auto offline = base;
    offline.provider.availability = provider::ProviderAvailability::Offline;
    check(publishRuntimeRequirementAuthority(offline, temp.path()).code ==
              RuntimeRequirementAuthorityCode::ProviderUnavailable,
          "offline current provider is rejected");
    checkStoreUnchanged(temp.path(), before, "offline provider does not mutate prior store");

    auto changedRevision = base;
    changedRevision.provider.metadataRevision = 42u;
    check(publishRuntimeRequirementAuthority(changedRevision, temp.path()).code ==
              RuntimeRequirementAuthorityCode::EvidenceRebindRejected,
          "same exact evidence cannot be rebound to a different provider metadata revision");
    checkStoreUnchanged(temp.path(), before, "provider revision rebind does not mutate prior store");

    auto wrongResult = base;
    wrongResult.evidence.resultId = "result-other";
    check(publishRuntimeRequirementAuthority(wrongResult, temp.path()).code ==
              RuntimeRequirementAuthorityCode::ProviderMismatch,
          "compatibility reference/result ID mismatch is rejected");
    checkStoreUnchanged(temp.path(), before, "result ID mismatch does not mutate prior store");

    auto wrongProvenance = base;
    wrongProvenance.evidence.provenanceId = "other-provenance";
    check(publishRuntimeRequirementAuthority(wrongProvenance, temp.path()).code ==
              RuntimeRequirementAuthorityCode::ProviderMismatch,
          "compatibility reference/provenance mismatch is rejected");
    checkStoreUnchanged(temp.path(), before, "provenance mismatch does not mutate prior store");

    auto mismatchedReport = base;
    auto failedSource = sessionReport();
    failedSource.seats.front().controller = metrics::CapabilityOutcome::Failed;
    failedSource.sessionVerdict = metrics::EvidenceVerdict::Fail;
    failedSource.physicalValidationEligible = false;
    mismatchedReport.report = failedSource;
    check(publishRuntimeRequirementAuthority(mismatchedReport, temp.path()).code ==
              RuntimeRequirementAuthorityCode::EvidenceReportMismatch,
          "compatibility result that did not come from the supplied report is rejected");
    checkStoreUnchanged(temp.path(), before, "result/report mismatch does not mutate prior store");
}

void testPhysicalPublicationRequiresExistingEligibility() {
    TempStore temp("physical-ineligible");
    auto review = reviewFor();

    auto report = sessionReport(metrics::EvidenceOrigin::Physical);
    report.seats.front().windowOwnershipVerified = false;
    report.sessionVerdict = metrics::EvidenceVerdict::InsufficientEvidence;
    report.physicalValidationEligible = false;
    review.report = report;
    review.evidence = compatibilityFor(report);
    review.game.game.compatibility = profile::CompatibilityReference{
        review.evidence.resultId, review.evidence.provenanceId,
        static_cast<std::uint32_t>(review.evidence.provenanceRevision)};

    check(publishRuntimeRequirementAuthority(review, temp.path()).code ==
              RuntimeRequirementAuthorityCode::PhysicalEvidenceIneligible &&
              !std::filesystem::exists(temp.path()),
          "Physical result/report without pre-existing physical eligibility cannot publish authority");
}

void testStaleCatalogAndInsufficientCapabilitiesReject() {
    TempStore staleStore("stale");
    auto stale = reviewFor();
    stale.game.staleness = catalog::CatalogStaleness::Stale;
    check(publishRuntimeRequirementAuthority(stale, staleStore.path()).code ==
              RuntimeRequirementAuthorityCode::StaleCatalogGame &&
              !std::filesystem::exists(staleStore.path()),
          "stale catalog entry cannot publish authority");

    TempStore missingCapsStore("missing-caps");
    auto review = reviewFor();
    review.requirements.controller = true;
    review.evidence.controller = compat::EvidenceStatus::NotMeasured;
    check(publishRuntimeRequirementAuthority(review, missingCapsStore.path()).code ==
              RuntimeRequirementAuthorityCode::EvidenceReportMismatch &&
              !std::filesystem::exists(missingCapsStore.path()),
          "fabricating a missing controller result without matching report is rejected before publication");

    auto report = sessionReport();
    report.seats.front().controller = metrics::CapabilityOutcome::NotRequired;
    report.sessionVerdict = metrics::EvidenceVerdict::Pass;
    auto coherent = reviewFor();
    coherent.report = report;
    coherent.evidence = compatibilityFor(report);
    coherent.game.game.compatibility = profile::CompatibilityReference{
        coherent.evidence.resultId, coherent.evidence.provenanceId,
        static_cast<std::uint32_t>(coherent.evidence.provenanceRevision)};
    check(publishRuntimeRequirementAuthority(coherent, missingCapsStore.path()).code ==
              RuntimeRequirementAuthorityCode::InsufficientCapabilityEvidence &&
              !std::filesystem::exists(missingCapsStore.path()),
          "coherent but unmeasured required controller evidence cannot publish authority");
}

void testPreserveUnrelatedAndIncrementSameGameRevision() {
    TempStore temp("replace");
    auto other = reviewFor(metrics::EvidenceOrigin::Physical, "game:b",
                           "result-local-b", "local-runtime-evidence-b", 3u);
    check(publishRuntimeRequirementAuthority(other, temp.path()).succeeded(),
          "unrelated Game authority publishes first");
    const auto unrelated = loadDocument(temp.path()).records.front();

    auto current = reviewFor();
    LocalRequirementEvidenceRecord first;
    check(publishRuntimeRequirementAuthority(current, temp.path(), &first).succeeded() &&
              first.revision == 1u,
          "second unrelated Game starts at revision one");
    auto two = loadDocument(temp.path());
    check(two.records.size() == 2u && two.records.front() == unrelated,
          "publishing current Game preserves unrelated record exactly");

    auto updated = reviewFor(metrics::EvidenceOrigin::Physical, "game:a",
                             "result-local-a-v2", "local-runtime-evidence-v2", 8u);
    LocalRequirementEvidenceRecord second;
    check(publishRuntimeRequirementAuthority(updated, temp.path(), &second).succeeded() &&
              second.revision == 2u,
          "same Game replacement monotonically increments revision");
    const auto after = loadDocument(temp.path());
    check(after.records.size() == 2u && after.records.front() == unrelated,
          "same Game update still preserves unrelated record");
    std::size_t currentCount = 0u;
    for (const auto& record : after.records) {
        if (record.gameId == "game:a") {
            ++currentCount;
            check(record == second && record.evidenceResultId == "result-local-a-v2",
                  "same Game update replaces exactly one authority record");
        }
    }
    check(currentCount == 1u, "same Game update never creates duplicate Game authority");
}

void testRevisionOverflowAndCorruptExistingStoreFailClosed() {
    TempStore overflowStore("overflow");
    auto review = reviewFor();
    check(publishRuntimeRequirementAuthority(review, overflowStore.path()).succeeded(),
          "overflow test seeds valid store");
    auto document = loadDocument(overflowStore.path());
    document.records.front().revision = (std::numeric_limits<std::uint64_t>::max)();
    GameRuntimeRequirementStore store(overflowStore.path());
    check(store.save(document).succeeded(), "overflow fixture publishes valid maximum revision");
    const auto bytesBeforeOverflow = readBytes(overflowStore.path());

    auto update = reviewFor(metrics::EvidenceOrigin::Physical, "game:a",
                            "result-overflow-next", "prov-overflow-next", 9u);
    check(publishRuntimeRequirementAuthority(update, overflowStore.path()).code ==
              RuntimeRequirementAuthorityCode::RevisionOverflow &&
              readBytes(overflowStore.path()) == bytesBeforeOverflow,
          "maximum existing revision fails closed without changing store bytes");

    TempStore corruptStore("corrupt");
    {
        std::ofstream corrupt(corruptStore.path(), std::ios::binary | std::ios::trunc);
        corrupt << "{corrupt-runtime-authority";
    }
    const auto corruptBefore = readBytes(corruptStore.path());
    check(publishRuntimeRequirementAuthority(review, corruptStore.path()).code ==
              RuntimeRequirementAuthorityCode::StoreLoadFailed &&
              readBytes(corruptStore.path()) == corruptBefore,
          "corrupt existing store is rejected without overwrite or repair guessing");

    TempStore futureStore("future-schema");
    {
        std::ofstream future(futureStore.path(), std::ios::binary | std::ios::trunc);
        future << "{\"schema_version\":"
               << (kRequirementEvidenceStoreSchemaVersion + 1u)
               << ",\"records\":[]}";
    }
    const auto futureBefore = readBytes(futureStore.path());
    check(publishRuntimeRequirementAuthority(review, futureStore.path()).code ==
              RuntimeRequirementAuthorityCode::StoreLoadFailed &&
              readBytes(futureStore.path()) == futureBefore,
          "future requirement store schema is rejected without mutation");
}

void testHighRiskDoesNotManufactureProtectedApproval() {
    TempStore temp("high-risk");
    auto review = reviewFor();
    review.report = sessionReport(metrics::EvidenceOrigin::Physical);
    review.evidence = compatibilityFor(
        review.report, "game:a", "result-protected", "prov-protected", 11u,
        compat::Scenario::ProtectedExperiment, true);
    review.game.game.compatibility = profile::CompatibilityReference{
        review.evidence.resultId, review.evidence.provenanceId,
        static_cast<std::uint32_t>(review.evidence.provenanceRevision)};
    review.requirements = allRequirements(true);

    LocalRequirementEvidenceRecord published;
    check(publishRuntimeRequirementAuthority(review, temp.path(), &published).succeeded() &&
              published.requirements.highRisk,
          "high-risk exact evidence may publish its requirement record without approval persistence");
    const auto stored = loadDocument(temp.path());
    const auto unapproved = resolveStored(stored, review, LocalEvidenceTrust::PhysicalOnly);
    check(unapproved.authorities.empty() && unapproved.requirements.empty() &&
              unapproved.blockedGames.size() == 1u &&
              unapproved.blockedGames.front().code ==
                  RequirementResolveCode::ProtectedApprovalRequired,
          "saved high-risk authority remains blocked by separate exact protected approval gate");

    ProtectedRuntimeApproval approval;
    approval.gameId = published.gameId;
    approval.providerId = published.providerId;
    approval.providerAppId = published.providerAppId;
    approval.providerMetadataRevision = published.providerMetadataRevision;
    approval.requirementRecordId = published.recordId;
    approval.requirementRevision = published.revision;
    approval.evidenceResultId = published.evidenceResultId;
    approval.evidenceProvenanceRevision = published.evidenceProvenanceRevision;
    const std::vector<ProtectedRuntimeApproval> approvals{approval};
    const auto approved = resolveStored(
        stored, review, LocalEvidenceTrust::PhysicalOnly, approvals);
    check(approved.authorities.size() == 1u && approved.requirements.size() == 1u &&
              approved.requirements.front().highRiskApproved,
          "only an externally supplied exact protected approval unlocks high-risk authority");
}

} // namespace

int main() {
    testFirstPhysicalPublicationAndResolverRoundTrip();
    testSingleSeatPublicationPreservesValidationScope();
    testControlledPublicationRemainsControlled();
    testSyntheticAndCommunityPublicationRejected();
    testCapabilitiesAreDerivedOnlyFromObservations();
    testIdentityMismatchesRejectWithoutMutation();
    testProviderRevisionAvailabilityAndEvidenceProvenanceFailures();
    testPhysicalPublicationRequiresExistingEligibility();
    testStaleCatalogAndInsufficientCapabilitiesReject();
    testPreserveUnrelatedAndIncrementSameGameRevision();
    testRevisionOverflowAndCorruptExistingStoreFailClosed();
    testHighRiskDoesNotManufactureProtectedApproval();

    if (failures != 0) {
        std::cerr << failures << " runtime requirement authority checks failed.\n";
        return 1;
    }
    std::cout << "Runtime requirement authority tests passed.\n";
    return 0;
}
