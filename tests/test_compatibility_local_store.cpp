#include "hydra/compatibility_local_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using namespace hydra;
using namespace hydra::community;
using namespace hydra::materialization;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

compat::CompatibilityResult resultFixture(std::string resultId,
                                           bool protectedExperimental = false) {
    compat::CompatibilityResult value;
    value.resultId = std::move(resultId);
    value.timestampClass = compat::TimestampClass::MonthBucket;
    value.timestampBucket = "2026-08";
    value.gameId = "game:local-store";
    value.providerId = "custom";
    value.providerAppId = "local-store-app";
    value.gameVersion = "1.0";
    value.hydraSeatVersion = "0.1.0";
    value.hydraSeatBuild = "local-store-build";
    value.windowsBuildClass = "win11-25h2";
    value.architecture = "x64";
    value.scenario = protectedExperimental ? compat::Scenario::ProtectedExperiment
                                           : compat::Scenario::DifferentGames;
    value.protectedExperimental = protectedExperimental;
    value.launch = compat::EvidenceStatus::Pass;
    value.inputIsolation = compat::EvidenceStatus::Pass;
    value.cleanExit = compat::EvidenceStatus::Pass;
    value.rollback = compat::EvidenceStatus::Pass;
    value.origin = compat::ResultOrigin::ControlledProcess;
    value.provenanceId = "local-store-fixture";
    value.provenanceRevision = 1u;
    return value;
}

std::filesystem::path makeRoot() {
    auto root = std::filesystem::temp_directory_path() / "hydraseat-local-store-tests";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    check(!error, "temporary local-store test root can be created");
    return root;
}

LocalMaterializationDecision materializationDecisionFixture(
    LocalMaterializationDecisionOrigin origin =
        LocalMaterializationDecisionOrigin::LocalApproved) {
    LocalMaterializationDecision decision;
    decision.decisionId = "local-materialization-1";
    decision.revision = 7u;
    decision.origin = origin;
    decision.setupId = "setup-local-materialization";
    decision.instanceIndex = 0u;
    decision.gameId = "game:local-materialization";
    decision.providerId = "custom";
    decision.providerAppId = "local-materialization-app";
    decision.providerMetadataRevision = 11u;
    decision.requirementRevision = 13u;
    decision.compatibility = profile::CompatibilityReference{
        "compat-local-materialization", "local-evidence", 3u};

    CompatibilityRecipeStep step;
    step.stepId = "copy-seat-config";
    step.phase = setup::RecipeExecutionPhase::PreSpawn;
    step.scope = setup::MutationScope::SeatWritableInstance;
    step.files.push_back({L"config/default.ini", L"config/player.ini", 4096u});
    decision.steps.push_back(std::move(step));
    return decision;
}

MaterializationDecisionQuery materializationQueryFixture() {
    const auto decision = materializationDecisionFixture();
    MaterializationDecisionQuery query;
    query.setupId = decision.setupId;
    query.instanceIndex = decision.instanceIndex;
    query.gameId = decision.gameId;
    query.providerId = decision.providerId;
    query.providerAppId = decision.providerAppId;
    query.providerMetadataRevision = decision.providerMetadataRevision;
    query.requirementRevision = decision.requirementRevision;
    query.compatibility = decision.compatibility;
    return query;
}

void testMissingStoreIsNormalAndNonMutating() {
    const auto root = makeRoot();
    CompatibilityLocalStore store(root / "missing.jsonl");
    CompatibilityShareModel model;
    model.recordLocalResult(resultFixture("existing"));

    const auto result = store.load(model);
    check(result.code == CompatibilityLocalStoreCode::Missing && result.succeeded() && !result.found(),
          "missing local store is a normal first-run condition");
    check(model.history().size() == 1u && model.active() != nullptr &&
              model.active()->result.resultId == "existing",
          "missing local store does not mutate existing in-memory evidence");

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void testAtomicSaveLoadRetentionAndConsentReset() {
    const auto root = makeRoot();
    const auto path = root / "nested" / "compatibility-results.jsonl";
    CompatibilityLocalStore store(path);

    CompatibilityShareModel source;
    CompatibilityPrivacySettings sourceSettings;
    sourceSettings.communitySharingEnabled = true;
    sourceSettings.retainedLocalResults = 3u;
    source.setPrivacySettings(sourceSettings);
    source.recordLocalResult(resultFixture("store-1"));
    source.recordLocalResult(resultFixture("store-2", true));
    source.recordLocalResult(resultFixture("store-3"));
    CompatibilitySharePreview preview;
    source.preparePreview(preview);
    source.beginSubmission(preview);

    const auto saved = store.save(source);
    check(saved.succeeded() && saved.code == CompatibilityLocalStoreCode::Success,
          "local technical history saves through the bounded atomic store");
    check(std::filesystem::exists(path) && !std::filesystem::exists(path.wstring() + L".tmp"),
          "successful store publication leaves no staging file");

    CompatibilityShareModel restored;
    CompatibilityPrivacySettings restoredSettings;
    restoredSettings.communitySharingEnabled = false;
    restoredSettings.retainedLocalResults = 2u;
    restored.setPrivacySettings(restoredSettings);
    const auto loaded = store.load(restored);
    check(loaded.succeeded() && loaded.found() && restored.history().size() == 2u,
          "local store loads and applies the current retention limit");
    check(restored.history()[0].result.resultId == "store-2" &&
              restored.history()[0].protectedExperimental() &&
              restored.history()[1].result.resultId == "store-3",
          "local store preserves newest technical evidence and Protected semantics");
    check(restored.state() == CompatibilityShareState::LocalResultAvailable &&
              !restored.privacySettings().communitySharingEnabled,
          "store restore never restores pending submission or network consent");

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void testCorruptAndOversizedStoresAreTransactional() {
    const auto root = makeRoot();
    const auto path = root / "compatibility-results.jsonl";
    CompatibilityLocalStore store(path);
    CompatibilityShareModel model;
    model.recordLocalResult(resultFixture("baseline"));

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "{\"schemaVersion\":2,\"kind\":\"compatibility-local-history\"}";
    }
    const auto corrupt = store.load(model);
    check(corrupt.code == CompatibilityLocalStoreCode::InvalidHistory &&
              model.history().size() == 1u && model.active() != nullptr &&
              model.active()->result.resultId == "baseline",
          "corrupt/future store is rejected without replacing previous evidence");

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        std::string oversized(kMaximumCompatibilityLocalHistoryBytes + 1u, 'x');
        output.write(oversized.data(), static_cast<std::streamsize>(oversized.size()));
    }
    const auto oversized = store.load(model);
    check(oversized.code == CompatibilityLocalStoreCode::TooLarge &&
              model.active() != nullptr && model.active()->result.resultId == "baseline",
          "oversized store fails before parsing and preserves previous evidence");

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void testEmptyHistoryCanBeSavedAndExplicitlyRemoved() {
    const auto root = makeRoot();
    const auto path = root / "compatibility-results.jsonl";
    CompatibilityLocalStore store(path);
    CompatibilityShareModel empty;
    check(store.save(empty).succeeded(), "empty technical history saves as a versioned empty store");

    CompatibilityShareModel restored;
    restored.recordLocalResult(resultFixture("will-clear"));
    check(store.load(restored).succeeded() && restored.history().empty() && restored.active() == nullptr,
          "valid empty store intentionally restores an empty local history");

    check(store.remove().succeeded() && !std::filesystem::exists(path),
          "explicit local-store remove deletes the persisted evidence file");
    const auto missing = store.remove();
    check(missing.code == CompatibilityLocalStoreCode::Missing && missing.succeeded(),
          "repeated local-store remove is idempotent for an already missing file");

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void testStaleTemporaryPayloadIsCleanedAndNeverRetained() {
    const auto root = makeRoot();
    const auto path = root / "compatibility-results.jsonl";
    const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
    CompatibilityLocalStore store(path);
    CompatibilityShareModel model;
    model.recordLocalResult(resultFixture("cleanup-result"));

    {
        std::ofstream stale(temporary, std::ios::binary | std::ios::trunc);
        stale << "sensitive-stale-payload";
    }
    check(store.save(model).succeeded() && std::filesystem::exists(path) &&
              !std::filesystem::exists(temporary),
          "save removes stale temporary payload before staging and leaves none after publish");

    check(store.remove().succeeded(), "cleanup fixture removes the durable store");
    {
        std::ofstream orphan(temporary, std::ios::binary | std::ios::trunc);
        orphan << "orphan-sensitive-payload";
    }
    check(store.remove().succeeded() && !std::filesystem::exists(path) &&
              !std::filesystem::exists(temporary),
          "explicit removal also purges an orphan staging payload when the durable store is absent");

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void testInvalidStorePathsFailClosed() {
    CompatibilityLocalStore emptyPath({});
    CompatibilityShareModel model;
    check(emptyPath.load(model).code == CompatibilityLocalStoreCode::ReadFailed,
          "empty store path cannot be read");
    check(emptyPath.save(model).code == CompatibilityLocalStoreCode::WriteFailed,
          "empty store path cannot be written");
    check(emptyPath.remove().code == CompatibilityLocalStoreCode::RemoveFailed,
          "empty store path cannot be removed");
}

void testLocalMaterializationDecisionStoreAndTrustBoundary() {
    const auto root = makeRoot();
    const auto path = root / "materialization-decisions.json";
    LocalMaterializationDecisionStore store(path);

    LocalMaterializationDecisionDocument document;
    document.decisions.push_back(materializationDecisionFixture());

    std::string encoded;
    const auto encodedResult =
        encodeLocalMaterializationDecisionDocumentJson(document, encoded);
    check(encodedResult.succeeded() && !encoded.empty(),
          "local materialization decisions encode as bounded strict JSON");

    LocalMaterializationDecisionDocument decoded;
    const auto decodedResult =
        decodeLocalMaterializationDecisionDocumentJson(encoded, decoded);
    check(decodedResult.succeeded() && decoded == document,
          "local materialization decision JSON round-trips exact typed semantics");

    check(store.save(document).succeeded() && std::filesystem::exists(path) &&
              !std::filesystem::exists(path.wstring() + L".tmp"),
          "local materialization decision store publishes atomically without staging residue");

    StoreBackedTrustedMaterializationDecisionSource source(path);
    auto query = materializationQueryFixture();
    LocalMaterializationDecision resolved;
    const auto trusted = source.resolveCurrent(query, resolved);
    check(trusted.code == TrustedMaterializationDecisionCode::Success &&
              trusted.found() && resolved == document.decisions.front(),
          "fresh exact locally approved decision becomes typed materialization authority");

    auto stale = query;
    ++stale.requirementRevision;
    LocalMaterializationDecision staleOutput;
    const auto staleResult = source.resolveCurrent(stale, staleOutput);
    check(staleResult.code == TrustedMaterializationDecisionCode::IdentityMismatch &&
              !staleResult.succeeded(),
          "stale requirement identity cannot reuse a local materialization decision");

    auto missing = query;
    missing.setupId = "another-setup";
    LocalMaterializationDecision missingOutput;
    const auto missingResult = source.resolveCurrent(missing, missingOutput);
    check(missingResult.code == TrustedMaterializationDecisionCode::NotRequired &&
              missingResult.succeeded() && !missingResult.found(),
          "setup instances with no local materialization decision remain unchanged");

    document.decisions.front() = materializationDecisionFixture(
        LocalMaterializationDecisionOrigin::ImportedCommunity);
    check(store.save(document).succeeded(),
          "community-origin descriptor can be persisted only as explicitly untrusted data");
    LocalMaterializationDecision communityOutput;
    const auto community = source.resolveCurrent(query, communityOutput);
    check(community.code == TrustedMaterializationDecisionCode::UntrustedOrigin &&
              !community.succeeded() && !community.found(),
          "community/imported materialization bytes never become runtime mutation authority");

    auto unsafeDocument = document;
    unsafeDocument.decisions.front() = materializationDecisionFixture();
    unsafeDocument.decisions.front().steps.front().scope =
        setup::MutationScope::SharedInstallation;
    const auto unsafe = validateLocalMaterializationDecisionDocument(unsafeDocument);
    check(unsafe.code == MaterializationDecisionStoreCode::InvalidDocument &&
              !unsafe.succeeded(),
          "local materialization authority cannot authorize shared-installation mutation");

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void testMaterializationDefaultPathContract() {
    std::string error;
    const auto storePath = defaultLocalMaterializationDecisionStorePath(&error);
#ifdef _WIN32
    check(storePath.has_value() && storePath->filename() == L"materialization-decisions.json" &&
              error.empty(),
          "Windows local materialization authority uses a fixed product-owned file");
    error.clear();
    const auto instanceRoot = defaultInstanceMaterializationRoot(&error);
    check(instanceRoot.has_value() && instanceRoot->filename() == L"instances" && error.empty(),
          "Windows materialization destinations use the fixed product-owned instances root");
#else
    check(!storePath.has_value() && !error.empty(),
          "non-Windows builds do not invent a production materialization authority path");
    error.clear();
    const auto instanceRoot = defaultInstanceMaterializationRoot(&error);
    check(!instanceRoot.has_value() && !error.empty(),
          "non-Windows builds do not invent a production materialization instance root");
#endif
}

void testDefaultPathContract() {
    std::string error;
    const auto path = defaultCompatibilityLocalStorePath(&error);
#ifdef _WIN32
    check(path.has_value() && path->filename() == L"compatibility-results.jsonl" && error.empty(),
          "Windows default local-store path resolves to the fixed per-user file name");
#else
    check(!path.has_value() && !error.empty(),
          "non-Windows builds do not invent a production default store path");
#endif
}

} // namespace

int main() {
    testMissingStoreIsNormalAndNonMutating();
    testAtomicSaveLoadRetentionAndConsentReset();
    testCorruptAndOversizedStoresAreTransactional();
    testEmptyHistoryCanBeSavedAndExplicitlyRemoved();
    testStaleTemporaryPayloadIsCleanedAndNeverRetained();
    testInvalidStorePathsFailClosed();
    testLocalMaterializationDecisionStoreAndTrustBoundary();
    testMaterializationDefaultPathContract();
    testDefaultPathContract();
    if (failures != 0) {
        std::cerr << failures << " compatibility local store test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Compatibility local store tests passed.\n";
    return EXIT_SUCCESS;
}
