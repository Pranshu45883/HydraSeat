#include "hydra/game_runtime_requirement_resolver.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::requirement;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class FakeProvider final : public provider::LauncherProviderAdapter {
public:
    provider::ProviderDescriptor descriptorValue{
        "fake", provider::ProviderAvailability::Available, 41u,
        {true, false, true, true, true}};

    provider::ProviderDescriptor descriptor() const noexcept override { return descriptorValue; }
    provider::DiscoveryResponse discoverInstalledGames() noexcept override {
        return {provider::ProviderResult::Success, descriptorValue.metadataRevision, {}, {}};
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

profile::GameRecord game(std::string id = "game:a") {
    profile::GameRecord value;
    value.gameId = std::move(id);
    value.providerId = "fake";
    value.providerAppId = "app-100";
    value.title = L"Fixture Game";
    value.installRoot = L"C:\\Games\\Fixture";
    value.executableCandidates = {L"C:\\Games\\Fixture\\fixture.exe"};
    value.localVersion = L"1.0.7";
    value.executableSha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    value.origin = profile::GameOrigin::Discovered;
    return value;
}

catalog::LocalGameCatalog catalogFor(const profile::GameRecord& value = game()) {
    catalog::LocalGameCatalog catalog;
    catalog::LocalGameCatalogEntry entry;
    entry.game = value;
    entry.architecture = catalog::ExecutableArchitecture::X64;
    entry.staleness = catalog::CatalogStaleness::Current;
    entry.mergedCandidateCount = 1u;
    catalog.entries.push_back(std::move(entry));
    return catalog;
}

compat::CompatibilityResult evidence(std::string id = "result-local-a") {
    compat::CompatibilityResult value;
    value.resultId = std::move(id);
    value.timestampClass = compat::TimestampClass::MonthBucket;
    value.timestampBucket = "2026-08";
    value.gameId = "game:a";
    value.providerId = "fake";
    value.providerAppId = "app-100";
    value.gameVersion = "1.0.7";
    value.hydraSeatVersion = "0.1.0";
    value.hydraSeatBuild = "build-a";
    value.windowsBuildClass = "win10-2009";
    value.architecture = "x64";
    value.scenario = compat::Scenario::DifferentGames;
    value.launch = compat::EvidenceStatus::Pass;
    value.inputIsolation = compat::EvidenceStatus::Pass;
    value.cleanExit = compat::EvidenceStatus::Pass;
    value.rollback = compat::EvidenceStatus::Pass;
    value.origin = compat::ResultOrigin::Physical;
    value.provenanceId = "local-runtime-evidence";
    value.provenanceRevision = 7u;
    return value;
}

LocalRequirementEvidenceRecord record(std::string gameId = "game:a") {
    LocalRequirementEvidenceRecord value;
    value.recordId = "requirement-a";
    value.revision = 13u;
    value.gameId = std::move(gameId);
    value.providerId = "fake";
    value.providerAppId = "app-100";
    value.gameVersionUtf8 = "1.0.7";
    value.executableSha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    value.providerMetadataRevision = 41u;
    value.evidenceResultId = "result-local-a";
    value.evidenceProvenanceId = "local-runtime-evidence";
    value.evidenceProvenanceRevision = 7u;
    value.validatedSeatCount = 2u;
    value.requirements.display = true;
    value.requirements.keyboard = true;
    value.requirements.mouse = true;
    value.requirements.controller = false;
    value.requirements.audioOutput = false;
    value.requirements.windowOwnership = true;
    value.requirements.recovery = true;
    value.requirements.highRisk = false;
    value.capabilities = {};
    return value;
}

RequirementEvidenceDocument document() {
    RequirementEvidenceDocument value;
    value.records = {record()};
    return value;
}

RequirementResolveContext context() {
    RequirementResolveContext value;
    value.referenceMonth = "2026-08";
    value.staleAfterMonths = 2u;
    value.hydraSeatVersion = "0.1.0";
    value.hydraSeatBuild = "build-a";
    value.windowsBuildClass = "win10-2009";
    value.architecture = "x64";
    value.trust = LocalEvidenceTrust::PhysicalOnly;
    return value;
}

std::vector<plan::ProviderAdapterBinding> providers(FakeProvider& provider) {
    return {{"fake", &provider, "app-100"}};
}

TrustedRequirementSnapshot resolve(const RequirementEvidenceDocument& stored,
                                   const catalog::LocalGameCatalog& localCatalog,
                                   FakeProvider& provider,
                                   const std::vector<compat::CompatibilityResult>& results,
                                   const std::vector<ProtectedRuntimeApproval>& approvals,
                                   const RequirementResolveContext& resolveContext,
                                   RequirementSnapshotDiagnostic* diagnostic = nullptr) {
    TrustedRequirementSnapshot snapshot;
    const auto bindings = providers(provider);
    const auto status = resolveTrustedGameRuntimeRequirements(
        stored, localCatalog, bindings, results, approvals, resolveContext, snapshot);
    if (diagnostic != nullptr) *diagnostic = status;
    return snapshot;
}

plan::ProviderAwareLaunchPlan planFor(const TrustedRequirementSnapshot& snapshot) {
    plan::ProviderAwareLaunchPlan result;
    if (snapshot.authorities.empty()) return result;
    const auto& authority = snapshot.authorities.front();
    plan::SeatProviderLaunchPlan seat;
    seat.seatId = 1u;
    seat.playerId = "player-a";
    seat.gameId = authority.requirement.gameId;
    seat.requirementRevision = authority.requirement.revision;
    seat.requirements = authority.requirement.requirements;
    seat.capabilities = authority.requirement.capabilities;
    seat.compatibility = authority.requirement.compatibility;
    seat.hardwareFingerprint = 1u;
    seat.launchRequest.providerId = authority.providerId;
    seat.launchRequest.gameId = authority.requirement.gameId;
    seat.launchRequest.providerAppId = authority.providerAppId;
    seat.launchRequest.metadataRevision = authority.providerMetadataRevision;
    seat.launchRequest.targetKind = provider::LaunchTargetKind::Executable;
    seat.launchRequest.target = L"C:\\Games\\Fixture\\fixture.exe";
    seat.launchRequest.launchCorrelationId = "trusted-plan-test";
    result.seats = {std::move(seat)};
    return result;
}

class FakeResolveInputSource final : public IRequirementResolveInputSource {
public:
    explicit FakeResolveInputSource(FakeProvider& provider) : provider_(&provider) {}

    bool capture(RequirementResolveInputs& output, std::string& error) override {
        ++captureCount;
        if (failCapture) {
            error = "injected input capture failure";
            return false;
        }
        output.catalog = catalogFor();
        output.providers = providers(*provider_);
        output.localEvidence = {evidence()};
        output.context = context();
        error.clear();
        return true;
    }

    std::size_t captureCount{0u};
    bool failCapture{false};

private:
    FakeProvider* provider_{nullptr};
};

void testStoreRoundTripAndTransactionalFailures() {
    const auto source = document();
    std::string json;
    check(encodeRequirementEvidenceDocumentJson(source, json).succeeded() && !json.empty(),
          "valid requirement evidence encodes");

    RequirementEvidenceDocument decoded;
    check(decodeRequirementEvidenceDocumentJson(json, decoded).succeeded() && decoded == source,
          "requirement evidence JSON round-trips exactly");

    const std::string currentSchemaToken =
        "\"schema_version\":" + std::to_string(kRequirementEvidenceStoreSchemaVersion);
    const std::string legacySchemaToken =
        "\"schema_version\":" + std::to_string(kLegacyRequirementEvidenceStoreSchemaVersion);
    auto legacyJson = json;
    const auto schemaPosition = legacyJson.find(currentSchemaToken);
    const std::string seatScopeField = "\"validated_seat_count\":2,";
    const auto seatScopePosition = legacyJson.find(seatScopeField);
    check(schemaPosition != std::string::npos && seatScopePosition != std::string::npos,
          "current store fixture exposes schema and validation Seat scope fields");
    if (schemaPosition != std::string::npos && seatScopePosition != std::string::npos) {
        legacyJson.replace(schemaPosition, currentSchemaToken.size(), legacySchemaToken);
        const auto legacySeatScopePosition = legacyJson.find(seatScopeField);
        if (legacySeatScopePosition != std::string::npos) {
            legacyJson.erase(legacySeatScopePosition, seatScopeField.size());
        }
        RequirementEvidenceDocument migrated;
        check(decodeRequirementEvidenceDocumentJson(legacyJson, migrated).succeeded() &&
                  migrated.schemaVersion == kRequirementEvidenceStoreSchemaVersion &&
                  migrated.records.size() == 1u &&
                  migrated.records.front().validatedSeatCount == 2u,
              "legacy v1 authority migrates conservatively to two Seats because v1 evidence required exactly two Seats");
    }

    RequirementEvidenceDocument sentinel;
    sentinel.records = {record("game:sentinel")};
    const auto before = sentinel;
    auto future = json;
    const auto futureSchemaPosition = future.find(currentSchemaToken);
    check(futureSchemaPosition != std::string::npos,
          "future-schema fixture finds the current schema token");
    if (futureSchemaPosition != std::string::npos) {
        future.replace(
            futureSchemaPosition, currentSchemaToken.size(),
            "\"schema_version\":" +
                std::to_string(kRequirementEvidenceStoreSchemaVersion + 1u));
        check(decodeRequirementEvidenceDocumentJson(future, sentinel).code ==
                  RequirementStoreCode::UnsupportedSchema && sentinel == before,
              "future requirement evidence schema fails closed without replacing caller state");
    }

    auto invalidSeatScope = json;
    const auto invalidScopePosition = invalidSeatScope.find("\"validated_seat_count\":2");
    check(invalidScopePosition != std::string::npos,
          "invalid-scope fixture finds the validation Seat field");
    if (invalidScopePosition != std::string::npos) {
        invalidSeatScope.replace(
            invalidScopePosition, std::string("\"validated_seat_count\":2").size(),
            "\"validated_seat_count\":0");
        check(decodeRequirementEvidenceDocumentJson(invalidSeatScope, sentinel).code ==
                  RequirementStoreCode::InvalidRecord && sentinel == before,
              "zero validation Seat scope is rejected transactionally");
    }

    auto duplicate = source;
    duplicate.records.push_back(duplicate.records.front());
    duplicate.records.back().recordId = "requirement-b";
    check(validateRequirementEvidenceDocument(duplicate).code ==
              RequirementStoreCode::DuplicateGameAuthority,
          "two stored authorities for one Game fail closed");

    auto unknown = json;
    unknown.insert(1u, "\"community_score\":100,");
    check(decodeRequirementEvidenceDocumentJson(unknown, sentinel).code ==
              RequirementStoreCode::UnknownField && sentinel == before,
          "unknown popularity/authority field is rejected transactionally");
    check(decodeRequirementEvidenceDocumentJson(json + " trailing", sentinel).code ==
              RequirementStoreCode::ParseError && sentinel == before,
          "trailing malformed store bytes do not replace caller state");

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("hydraseat-runtime-requirements-" + std::to_string(stamp) + ".json");
    GameRuntimeRequirementStore store(path);
    check(store.save(source).succeeded(), "file store publishes a valid staged document");
    RequirementEvidenceDocument loaded;
    check(store.load(loaded).succeeded() && loaded == source,
          "file store reloads the exact requirement evidence document");
    check(store.remove().succeeded(), "file store removes its own fixed-purpose file");
}

void testExactLocalEvidenceProducesTrustedSnapshot() {
    FakeProvider provider;
    const std::vector<compat::CompatibilityResult> results{evidence()};
    RequirementSnapshotDiagnostic diagnostic;
    const auto snapshot = resolve(document(), catalogFor(), provider, results, {}, context(),
                                  &diagnostic);
    check(diagnostic.succeeded(), "valid exact resolver inputs complete");
    check(snapshot.requirements.size() == 1u && snapshot.authorities.size() == 1u &&
              snapshot.blockedGames.empty(),
          "exact current local evidence produces one trusted requirement authority");
    if (snapshot.requirements.size() == 1u && snapshot.authorities.size() == 1u) {
        const auto& requirement = snapshot.requirements.front();
        const auto& authority = snapshot.authorities.front();
        check(requirement.gameId == "game:a" && requirement.revision == 13u &&
                  requirement.requirements.keyboard && requirement.requirements.mouse &&
                  !requirement.highRiskApproved,
              "trusted snapshot preserves only the revisioned local requirement facts");
        check(authority.requirement == requirement &&
                  authority.providerMetadataRevision == 41u &&
                  authority.evidenceResultId == "result-local-a" &&
                  authority.evidenceOrigin == compat::ResultOrigin::Physical &&
                  authority.evidenceTimestampBucket == "2026-08",
              "host authority retains exact provider and local evidence provenance");
        check(validateProviderAwareLaunchPlanAgainstTrustedRequirements(
                  planFor(snapshot), snapshot).succeeded(),
              "exact provider plan consumes the freshly resolved trusted authority");
    }
}

void testTrustedRuntimeRejectsSeatScopeExpansion() {
    FakeProvider provider;
    const std::vector<compat::CompatibilityResult> results{evidence()};
    auto snapshot = resolve(document(), catalogFor(), provider, results, {}, context());
    check(snapshot.requirements.size() == 1u && snapshot.authorities.size() == 1u,
          "scope test starts from one freshly resolved trusted authority");
    if (snapshot.requirements.size() != 1u || snapshot.authorities.size() != 1u) return;

    snapshot.requirements.front().validatedSeatCount = 1u;
    snapshot.authorities.front().requirement.validatedSeatCount = 1u;
    auto oneSeatPlan = planFor(snapshot);
    check(validateProviderAwareLaunchPlanAgainstTrustedRequirements(
              oneSeatPlan, snapshot).succeeded(),
          "fresh Host-side trusted gate accepts one planned Seat for one-Seat authority");

    auto twoSeatPlan = oneSeatPlan;
    auto second = twoSeatPlan.seats.front();
    second.seatId = 2u;
    second.playerId = "player-b";
    second.launchRequest.launchCorrelationId = "trusted-plan-test-seat-2";
    twoSeatPlan.seats.push_back(std::move(second));
    const auto rejected = validateProviderAwareLaunchPlanAgainstTrustedRequirements(
        twoSeatPlan, snapshot);
    check(rejected.code == TrustedPlanRequirementCode::ValidationSeatScopeExceeded &&
              rejected.gameId == "game:a",
          "Host-side fresh trusted gate independently rejects one-Seat authority reused for two Seats");
}

void testProviderCatalogAndMissingEvidenceFailClosedPerGame() {
    FakeProvider provider;
    const std::vector<compat::CompatibilityResult> results{evidence()};

    provider.descriptorValue.metadataRevision = 42u;
    auto snapshot = resolve(document(), catalogFor(), provider, results, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.size() == 1u &&
              snapshot.blockedGames.front().code == RequirementResolveCode::StaleProviderRevision,
          "provider revision drift removes runtime authority");

    provider.descriptorValue.metadataRevision = 41u;
    {
        const std::vector<plan::ProviderAdapterBinding> duplicateProviders{
            {"fake", &provider, "app-100"},
            {"fake", &provider, "app-100"},
        };
        TrustedRequirementSnapshot duplicateSnapshot;
        const auto duplicateDiagnostic = resolveTrustedGameRuntimeRequirements(
            document(), catalogFor(), duplicateProviders, results, {}, context(), duplicateSnapshot);
        check(duplicateDiagnostic.succeeded() && duplicateSnapshot.requirements.empty() &&
                  duplicateSnapshot.blockedGames.size() == 1u &&
                  duplicateSnapshot.blockedGames.front().code ==
                      RequirementResolveCode::DuplicateProvider,
              "duplicate exact provider authority removes runtime authority");
    }

    auto changedGame = game();
    changedGame.localVersion = L"1.0.8";
    snapshot = resolve(document(), catalogFor(changedGame), provider, results, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::GameIdentityMismatch,
          "local game identity/version drift removes runtime authority");

    auto staleCatalog = catalogFor();
    staleCatalog.entries.front().staleness = catalog::CatalogStaleness::Stale;
    snapshot = resolve(document(), staleCatalog, provider, results, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::StaleCatalogGame,
          "stale catalog evidence cannot authorize requirements");

    RequirementEvidenceDocument emptyStore;
    snapshot = resolve(emptyStore, catalogFor(), provider, results, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::MissingStoredEvidence,
          "missing stored evidence leaves the Game without runtime authority");

    snapshot = resolve(document(), catalogFor(), provider, {}, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::MissingLocalEvidence,
          "missing exact local compatibility result leaves the Game without runtime authority");
}

void testCommunityFreshnessEnvironmentAndCapabilityEvidenceFailClosed() {
    FakeProvider provider;
    auto result = evidence();
    std::vector<compat::CompatibilityResult> results{result};

    result.origin = compat::ResultOrigin::ImportedCommunity;
    results = {result};
    auto snapshot = resolve(document(), catalogFor(), provider, results, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::CommunityEvidenceRejected,
          "imported community result cannot become runtime authority");

    result = evidence();
    result.origin = compat::ResultOrigin::ControlledProcess;
    results = {result};
    snapshot = resolve(document(), catalogFor(), provider, results, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::UntrustedLocalEvidenceOrigin,
          "production PhysicalOnly policy rejects controlled-process evidence");
    auto controlledContext = context();
    controlledContext.trust = LocalEvidenceTrust::ControlledOrPhysical;
    snapshot = resolve(document(), catalogFor(), provider, results, {}, controlledContext);
    check(snapshot.requirements.size() == 1u,
          "explicit controlled integration policy may consume controlled local evidence");

    result = evidence();
    result.timestampBucket = "2026-04";
    results = {result};
    snapshot = resolve(document(), catalogFor(), provider, results, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::StaleLocalEvidence,
          "evidence outside freshness horizon is rejected");

    result = evidence();
    result.timestampBucket = "2026-09";
    results = {result};
    snapshot = resolve(document(), catalogFor(), provider, results, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::FutureLocalEvidence,
          "future evidence is rejected");

    result = evidence();
    result.windowsBuildClass = "win11-other";
    results = {result};
    snapshot = resolve(document(), catalogFor(), provider, results, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::EvidenceEnvironmentMismatch,
          "different local Windows environment cannot reuse runtime evidence");

    result = evidence();
    result.inputIsolation = compat::EvidenceStatus::NotMeasured;
    results = {result};
    snapshot = resolve(document(), catalogFor(), provider, results, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::InsufficientCapabilityEvidence,
          "unmeasured required input isolation cannot authorize capabilities");
}

ProtectedRuntimeApproval approvalFor(const LocalRequirementEvidenceRecord& source) {
    ProtectedRuntimeApproval approval;
    approval.gameId = source.gameId;
    approval.providerId = source.providerId;
    approval.providerAppId = source.providerAppId;
    approval.providerMetadataRevision = source.providerMetadataRevision;
    approval.requirementRecordId = source.recordId;
    approval.requirementRevision = source.revision;
    approval.evidenceResultId = source.evidenceResultId;
    approval.evidenceProvenanceRevision = source.evidenceProvenanceRevision;
    return approval;
}

void testTrustedPlanGateRejectsTamperedOrStaleAuthority() {
    FakeProvider provider;
    const std::vector<compat::CompatibilityResult> results{evidence()};
    const auto snapshot = resolve(document(), catalogFor(), provider, results, {}, context());
    auto providerPlan = planFor(snapshot);

    auto staleProvider = providerPlan;
    ++staleProvider.seats.front().launchRequest.metadataRevision;
    check(validateProviderAwareLaunchPlanAgainstTrustedRequirements(
              staleProvider, snapshot).code == TrustedPlanRequirementCode::ProviderRevisionMismatch,
          "runtime plan gate rejects provider revision drift even when the plan is otherwise well-formed");

    auto wrongExecutable = providerPlan;
    wrongExecutable.seats.front().launchRequest.target = L"C:\\Games\\Other\\other.exe";
    check(validateProviderAwareLaunchPlanAgainstTrustedRequirements(
              wrongExecutable, snapshot).code == TrustedPlanRequirementCode::GameIdentityMismatch,
          "runtime plan gate rejects an executable target outside the current catalog Game identity");

    auto tamperedRequirement = providerPlan;
    ++tamperedRequirement.seats.front().requirementRevision;
    check(validateProviderAwareLaunchPlanAgainstTrustedRequirements(
              tamperedRequirement, snapshot).code == TrustedPlanRequirementCode::RequirementMismatch,
          "runtime plan gate rejects a recomputed/tampered requirement revision");

    auto community = snapshot;
    community.authorities.front().evidenceOrigin = compat::ResultOrigin::ImportedCommunity;
    check(validateProviderAwareLaunchPlanAgainstTrustedRequirements(
              providerPlan, community).code == TrustedPlanRequirementCode::UntrustedEvidenceOrigin,
          "runtime plan gate rejects community-only authority after snapshot construction");

    auto stale = snapshot;
    stale.authorities.front().evidenceTimestampBucket = "2026-01";
    check(validateProviderAwareLaunchPlanAgainstTrustedRequirements(
              providerPlan, stale).code == TrustedPlanRequirementCode::StaleEvidence,
          "runtime plan gate independently rechecks trusted evidence freshness");

    auto duplicate = snapshot;
    duplicate.authorities.push_back(duplicate.authorities.front());
    duplicate.requirements.push_back(duplicate.requirements.front());
    check(validateProviderAwareLaunchPlanAgainstTrustedRequirements(
              providerPlan, duplicate).code == TrustedPlanRequirementCode::DuplicateAuthority,
          "runtime plan gate rejects duplicate Game authority rather than choosing one");
}

void testStoreBackedSourceReloadsBeforeEachAuthorityDecision() {
    FakeProvider provider;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("hydraseat-runtime-source-" + std::to_string(stamp) + ".json");
    GameRuntimeRequirementStore store(path);
    check(store.save(document()).succeeded(),
          "store-backed authority fixture publishes requirement evidence");

    auto inputs = std::make_shared<FakeResolveInputSource>(provider);
    StoreBackedTrustedRequirementSource source(path, inputs);
    TrustedRequirementSnapshot current;
    check(source.resolveCurrent(current).succeeded() && current.authorities.size() == 1u &&
              inputs->captureCount == 1u,
          "store-backed source reloads store and captures current local inputs");
    const auto previouslyTrustedPlan = planFor(current);

    provider.descriptorValue.metadataRevision = 42u;
    TrustedRequirementSnapshot drifted;
    check(source.resolveCurrent(drifted).succeeded() && drifted.authorities.empty() &&
              drifted.requirements.empty() && drifted.blockedGames.size() == 1u &&
              drifted.blockedGames.front().code == RequirementResolveCode::StaleProviderRevision &&
              inputs->captureCount == 2u,
          "later resolve does not reuse an earlier provider revision snapshot");
    check(validateProviderAwareLaunchPlanAgainstTrustedRequirements(
              previouslyTrustedPlan, drifted).code == TrustedPlanRequirementCode::MissingAuthority,
          "previously trusted plan loses runtime authority after provider revision drift");

    inputs->failCapture = true;
    TrustedRequirementSnapshot unavailable;
    check(source.resolveCurrent(unavailable).code == RequirementSnapshotCode::InputUnavailable,
          "failed current-input capture cannot fall back to a cached trusted snapshot");
    inputs->failCapture = false;
    check(store.remove().succeeded(), "store-backed authority fixture removes its local file");
    TrustedRequirementSnapshot missingStore;
    check(source.resolveCurrent(missingStore).code == RequirementSnapshotCode::InvalidStore,
          "missing requirement store cannot fall back to a previously trusted snapshot");
}

void testLauncherProjectionUsesOnlyCurrentTrustedStore() {
    FakeProvider provider;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("hydraseat-runtime-projection-" + std::to_string(stamp) + ".json");
    GameRuntimeRequirementStore store(path);
    check(store.save(document()).succeeded(),
          "launcher projection fixture publishes a valid local requirement store");

    auto inputs = std::make_shared<FakeResolveInputSource>(provider);
    StoreBackedTrustedRequirementSource source(path, inputs);
    std::vector<plan::GameRuntimeRequirement> projection;
    const auto valid = resolveCurrentRequirementProjection(source, projection);
    check(valid.succeeded() && projection.size() == 1u &&
              projection.front().gameId == "game:a" &&
              projection.front().revision == 13u,
          "valid local store projects the exact trusted requirement into the launcher input");

    plan::GameRuntimeRequirement sentinel;
    sentinel.gameId = "sentinel";
    sentinel.revision = 999u;
    projection = {sentinel};
    check(store.remove().succeeded(),
          "launcher projection fixture removes the requirement store");
    const auto missing = resolveCurrentRequirementProjection(source, projection);
    check(missing.code == RequirementSnapshotCode::InvalidStore && projection.empty(),
          "missing store clears launcher requirements instead of retaining a previous projection");

    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "{corrupt-runtime-authority";
    }
    projection = {sentinel};
    const auto corrupt = resolveCurrentRequirementProjection(source, projection);
    check(corrupt.code == RequirementSnapshotCode::InvalidStore && projection.empty(),
          "corrupt store clears launcher requirements and fails closed");
    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);
}

void testProtectedApprovalIsExactAndSeparateFromEvidenceStore() {
    FakeProvider provider;
    auto stored = document();
    stored.records.front().requirements.highRisk = true;
    auto result = evidence();
    result.scenario = compat::Scenario::ProtectedExperiment;
    result.protectedExperimental = true;
    const std::vector<compat::CompatibilityResult> results{result};

    auto snapshot = resolve(stored, catalogFor(), provider, results, {}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::ProtectedApprovalRequired,
          "protected evidence alone cannot self-approve a runtime experiment");

    auto staleApproval = approvalFor(stored.records.front());
    ++staleApproval.providerMetadataRevision;
    snapshot = resolve(stored, catalogFor(), provider, results, {staleApproval}, context());
    check(snapshot.requirements.empty() && snapshot.blockedGames.front().code ==
              RequirementResolveCode::ProtectedApprovalRequired,
          "approval for another provider revision is not reusable");

    const auto exactApproval = approvalFor(stored.records.front());
    snapshot = resolve(stored, catalogFor(), provider, results, {exactApproval}, context());
    check(snapshot.requirements.size() == 1u && snapshot.requirements.front().highRiskApproved,
          "exact current protected approval is projected into the trusted requirement only");

    RequirementSnapshotDiagnostic diagnostic;
    (void)resolve(stored, catalogFor(), provider, results,
                  {exactApproval, exactApproval}, context(), &diagnostic);
    check(diagnostic.code == RequirementSnapshotCode::DuplicateApproval,
          "duplicate protected approvals fail closed before snapshot publication");
}

void testDefaultProductionCompositionCarriesMaterializationAuthority() {
    const auto source = makeDefaultProductionTrustedRequirementSource();
    check(source != nullptr,
          "default production trusted requirement authority is always non-null");
#ifdef _WIN32
    check(source && source->trustedMaterializationDecisionSource() != nullptr,
          "Windows production trusted authority composes the fixed local materialization decision source");
    const auto instancesRoot =
        source ? source->trustedMaterializationInstancesRoot() : std::filesystem::path{};
    check(instancesRoot.is_absolute() && instancesRoot.filename() == L"instances",
          "Windows production trusted authority pins the product-owned materialization instances root");
#else
    check(source && !source->trustedMaterializationDecisionSource() &&
              source->trustedMaterializationInstancesRoot().empty(),
          "non-Windows unavailable production authority does not invent materialization authority");
#endif
}

void testDuplicateLocalResultAuthorityDoesNotPublishSnapshot() {
    FakeProvider provider;
    const auto first = evidence();
    const std::vector<compat::CompatibilityResult> duplicated{first, first};
    TrustedRequirementSnapshot sentinel;
    plan::GameRuntimeRequirement marker;
    marker.gameId = "sentinel";
    marker.revision = 1u;
    sentinel.requirements.push_back(marker);
    const auto before = sentinel;
    const auto bindings = providers(provider);
    const auto diagnostic = resolveTrustedGameRuntimeRequirements(
        document(), catalogFor(), bindings, duplicated, {}, context(), sentinel);
    check(diagnostic.code == RequirementSnapshotCode::DuplicateLocalEvidenceId &&
              sentinel.requirements == before.requirements &&
              sentinel.blockedGames == before.blockedGames,
          "duplicate result_id authority rejects transactionally without publishing partial state");
}

} // namespace

int main() {
    testStoreRoundTripAndTransactionalFailures();
    testExactLocalEvidenceProducesTrustedSnapshot();
    testTrustedRuntimeRejectsSeatScopeExpansion();
    testProviderCatalogAndMissingEvidenceFailClosedPerGame();
    testCommunityFreshnessEnvironmentAndCapabilityEvidenceFailClosed();
    testTrustedPlanGateRejectsTamperedOrStaleAuthority();
    testStoreBackedSourceReloadsBeforeEachAuthorityDecision();
    testLauncherProjectionUsesOnlyCurrentTrustedStore();
    testProtectedApprovalIsExactAndSeparateFromEvidenceStore();
    testDefaultProductionCompositionCarriesMaterializationAuthority();
    testDuplicateLocalResultAuthorityDoesNotPublishSnapshot();

    if (failures != 0) {
        std::cerr << failures << " game runtime requirement resolver test(s) failed\n";
        return 1;
    }
    std::cout << "game runtime requirement resolver tests passed\n";
    return 0;
}
