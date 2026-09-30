#include "hydra/compatibility_share_model.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::community;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void enableSharing(CompatibilityShareModel& model,
                   std::size_t retainedLocalResults = kDefaultRetainedCompatibilityResults) {
    CompatibilityPrivacySettings settings;
    settings.communitySharingEnabled = true;
    settings.retainedLocalResults = retainedLocalResults;
    check(model.setPrivacySettings(settings).succeeded(),
          "privacy settings explicitly enable community sharing");
}

compat::CompatibilityResult resultFixture(std::string resultId,
                                          bool protectedExperimental = false) {
    compat::CompatibilityResult value;
    value.resultId = std::move(resultId);
    value.timestampClass = compat::TimestampClass::MonthBucket;
    value.timestampBucket = "2026-08";
    value.gameId = "game:share";
    value.providerId = "custom";
    value.providerAppId = "share-app";
    value.gameVersion = "1.0";
    value.hydraSeatVersion = "0.1.0";
    value.hydraSeatBuild = "share-build";
    value.windowsBuildClass = "win10-2009";
    value.architecture = "x64";
    value.scenario = protectedExperimental ? compat::Scenario::ProtectedExperiment
                                           : compat::Scenario::DifferentGames;
    value.protectedExperimental = protectedExperimental;
    value.launch = compat::EvidenceStatus::Pass;
    value.inputIsolation = compat::EvidenceStatus::Pass;
    value.cleanExit = compat::EvidenceStatus::Pass;
    value.origin = compat::ResultOrigin::ControlledProcess;
    value.provenanceId = "share-fixture";
    value.provenanceRevision = 5u;
    return value;
}

class FakeTransport final : public CommunitySubmissionTransport {
public:
    TransportAvailability availabilityValue{TransportAvailability::Available};
    SubmissionReceipt response{SubmissionTransportResult::Accepted, "receipt-share", true};
    std::size_t calls{0u};
    std::vector<SubmissionEnvelope> seen;

    TransportAvailability availability() const noexcept override { return availabilityValue; }
    SubmissionReceipt submit(const SubmissionEnvelope& envelope) noexcept override {
        ++calls;
        try {
            seen.push_back(envelope);
        } catch (...) {
            return {SubmissionTransportResult::RetryableFailure, std::nullopt, false};
        }
        return response;
    }
};

void testLocalPreviewDeclineFlowKeepsTechnicalResult() {
    CompatibilityShareModel model;
    check(model.state() == CompatibilityShareState::TestNotRun && model.active() == nullptr,
          "share model starts in TestNotRun without synthetic evidence");

    const auto local = resultFixture("share-1", true);
    check(model.recordLocalResult(local).succeeded() &&
              model.state() == CompatibilityShareState::LocalResultAvailable,
          "valid local test creates LocalResultAvailable state");

    CompatibilitySharePreview preview;
    check(model.preparePreview(preview).succeeded() &&
              model.state() == CompatibilityShareState::PreviewReady &&
              preview.protectedExperimental,
          "preview is generated locally and retains Protected / Experimental marker");
    check(model.active()->result == local,
          "preparing a sharing preview does not change technical result truth");

    check(model.declineSharing().succeeded() &&
              model.state() == CompatibilityShareState::UserDeclined &&
              model.active()->result == local,
          "declining optional sharing preserves the local result intact");

    CompatibilitySharePreview secondPreview;
    check(model.preparePreview(secondPreview).succeeded() &&
              secondPreview.exactRedactedJson == preview.exactRedactedJson &&
              secondPreview.payloadIdentity == preview.payloadIdentity &&
              secondPreview.generation != preview.generation,
          "re-preview is deterministic but receives a fresh consent generation");
    enableSharing(model);
    check(model.beginSubmission(preview).code == ShareModelCode::StaleApproval &&
              model.state() == CompatibilityShareState::PreviewReady,
          "a byte-identical older preview cannot be reused as stale approval");
}

void testOfflineFailureAndExplicitRetryRemainLocalFirst() {
    CompatibilityShareModel model;
    enableSharing(model);
    const auto local = resultFixture("share-offline");
    model.recordLocalResult(local);
    CompatibilitySharePreview preview;
    model.preparePreview(preview);
    check(model.beginSubmission(preview).succeeded() &&
              model.state() == CompatibilityShareState::SubmitPending,
          "explicit exact-preview consent moves the record to SubmitPending");

    FakeTransport transport;
    transport.availabilityValue = TransportAvailability::Offline;
    SubmissionReceipt receipt{SubmissionTransportResult::Accepted, "sentinel", true};
    const auto failed = model.completeSubmission(transport, receipt);
    check(failed.code == ShareModelCode::SubmissionFailed &&
              failed.submissionCode == SubmissionCode::TransportOffline &&
              model.state() == CompatibilityShareState::SubmitFailed && transport.calls == 0u,
          "offline community transport becomes SubmitFailed without transport invocation");
    check(model.active()->result == local && receipt.receiptId == "sentinel",
          "offline failure preserves both local technical result and caller receipt sentinel");

    check(model.beginSubmission(preview).succeeded(),
          "retry requires another explicit transition from SubmitFailed");
    transport.availabilityValue = TransportAvailability::Available;
    check(model.completeSubmission(transport, receipt).succeeded() &&
              model.state() == CompatibilityShareState::SubmitSucceeded &&
              transport.calls == 1u && receipt.receiptId == "receipt-share",
          "later explicit retry may succeed without rerunning the local compatibility test");
    check(transport.seen.size() == 1u &&
              transport.seen[0].exactRedactedJson == preview.exactRedactedJson &&
              transport.seen[0].idempotencyKey == preview.payloadIdentity,
          "transport consumes the exact approved bytes and deterministic payload identity");
    check(model.active()->result == local,
          "successful upload state still does not mutate technical result truth");
}

void testMismatchedPreviewCannotBeginSubmission() {
    CompatibilityShareModel model;
    enableSharing(model);
    model.recordLocalResult(resultFixture("share-mismatch"));
    CompatibilitySharePreview preview;
    model.preparePreview(preview);
    auto changed = preview;
    changed.exactRedactedJson += " ";
    const auto rejected = model.beginSubmission(changed);
    check(rejected.code == ShareModelCode::PayloadIdentityMismatch &&
              model.state() == CompatibilityShareState::PreviewReady,
          "changed preview bytes cannot be converted into user consent");
    changed = preview;
    changed.payloadIdentity += "x";
    check(model.beginSubmission(changed).code == ShareModelCode::PayloadIdentityMismatch &&
              model.state() == CompatibilityShareState::PreviewReady,
          "changed preview identity cannot authorize the exact bytes");
    changed = preview;
    ++changed.canonicalizationVersion;
    check(model.beginSubmission(changed).code == ShareModelCode::PayloadIdentityMismatch &&
              model.state() == CompatibilityShareState::PreviewReady,
          "changed canonicalization version cannot authorize community submission");

    FakeTransport transport;
    SubmissionReceipt receipt;
    check(model.completeSubmission(transport, receipt).code == ShareModelCode::InvalidState &&
              transport.calls == 0u,
          "transport cannot be invoked without the explicit SubmitPending state");

    auto settings = model.privacySettings();
    --settings.retainedLocalResults;
    check(model.setPrivacySettings(settings).succeeded() &&
              model.beginSubmission(preview).code == ShareModelCode::StaleApproval &&
              model.active()->result.resultId == "share-mismatch",
          "changed retention metadata invalidates community consent as stale without changing technical truth");
}

void testNewLocalResultSupersedesOldRecordWithoutDeletingHistory() {
    CompatibilityShareModel model;
    const auto oldResult = resultFixture("share-old");
    const auto newResult = resultFixture("share-new", true);
    model.recordLocalResult(oldResult);
    CompatibilitySharePreview preview;
    model.preparePreview(preview);
    model.declineSharing();

    check(model.recordLocalResult(newResult).succeeded() && model.history().size() == 2u &&
              model.state() == CompatibilityShareState::LocalResultAvailable,
          "new local test creates a new active share record");
    check(model.history()[0].state == CompatibilityShareState::Superseded &&
              model.history()[0].result == oldResult &&
              model.history()[0].supersededByResultId == "share-new",
          "older local result remains inspectable and explicitly marked Superseded");
    check(model.active()->result == newResult && model.active()->protectedExperimental(),
          "new active result preserves its own Protected / Experimental semantics");

    CompatibilitySharePreview newPreview;
    check(model.preparePreview(newPreview).succeeded() && newPreview.generation != preview.generation,
          "new local result receives a distinct preview consent generation");
    enableSharing(model);
    check(model.beginSubmission(preview).code == ShareModelCode::StaleApproval,
          "an older result's approval cannot authorize a newer result submission");
    check(model.beginSubmission(newPreview).succeeded(),
          "only the new result's exact preview can authorize its submission");
    FakeTransport transport;
    SubmissionReceipt receipt;
    check(model.completeSubmission(transport, receipt).succeeded() && transport.seen.size() == 1u &&
              transport.seen[0].exactRedactedJson == newPreview.exactRedactedJson &&
              transport.seen[0].exactRedactedJson.find("share-old") == std::string::npos,
          "Seat/user-local data from a superseded result cannot join the newer submission");
}

void testDuplicateRuntimeResultIdIsRejectedTransactionally() {
    CompatibilityShareModel model;
    const auto first = resultFixture("share-duplicate");
    check(model.recordLocalResult(first).succeeded(),
          "duplicate runtime fixture records its first result");
    CompatibilitySharePreview preview;
    check(model.preparePreview(preview).succeeded() &&
              model.state() == CompatibilityShareState::PreviewReady,
          "duplicate runtime fixture has observable active state before rejection");

    auto duplicate = first;
    duplicate.timestampBucket = "2026-08-30T10:00Z";
    const auto rejected = model.recordLocalResult(duplicate);
    check(rejected.code == ShareModelCode::InvalidResult &&
              model.history().size() == 1u &&
              model.state() == CompatibilityShareState::PreviewReady &&
              model.active() != nullptr && model.active()->result == first &&
              !model.active()->supersededByResultId.has_value(),
          "duplicate runtime result IDs fail closed without mutating active evidence");
}

void testSharingIsOptInAndDisableStopsPendingTransport() {
    CompatibilityShareModel model;
    check(!model.privacySettings().communitySharingEnabled &&
              model.privacySettings().retainedLocalResults == kDefaultRetainedCompatibilityResults,
          "community sharing is disabled by default with bounded local retention");

    model.recordLocalResult(resultFixture("share-opt-in"));
    CompatibilitySharePreview preview;
    check(model.preparePreview(preview).succeeded(),
          "exact redacted preview remains available locally while network sharing is disabled");

    FakeTransport transport;
    SubmissionReceipt receipt;
    check(model.beginSubmission(preview).code == ShareModelCode::SharingDisabled &&
              model.state() == CompatibilityShareState::PreviewReady && transport.calls == 0u,
          "preview approval cannot open the network boundary while sharing is disabled");

    enableSharing(model);
    check(model.beginSubmission(preview).succeeded() &&
              model.state() == CompatibilityShareState::SubmitPending,
          "explicit privacy setting plus exact preview approval is required before submission");

    auto settings = model.privacySettings();
    settings.communitySharingEnabled = false;
    check(model.setPrivacySettings(settings).succeeded() &&
              model.state() == CompatibilityShareState::PreviewReady,
          "disabling sharing cancels a not-yet-sent pending transition");
    check(model.completeSubmission(transport, receipt).code == ShareModelCode::SharingDisabled &&
              transport.calls == 0u,
          "disabling sharing before transport guarantees no network call");
}

void testRetentionSettingsAreBoundedAndRotateOldHistory() {
    CompatibilityShareModel model;
    auto invalid = model.privacySettings();
    invalid.retainedLocalResults = 0u;
    check(model.setPrivacySettings(invalid).code == ShareModelCode::InvalidPrivacySettings &&
              model.privacySettings().retainedLocalResults == kDefaultRetainedCompatibilityResults,
          "zero-result retention is rejected instead of becoming implicit data deletion");
    invalid.retainedLocalResults = kMaximumRetainedCompatibilityResults + 1u;
    check(model.setPrivacySettings(invalid).code == ShareModelCode::InvalidPrivacySettings,
          "retention above the public bound is rejected");

    CompatibilityPrivacySettings settings;
    settings.retainedLocalResults = 2u;
    check(model.setPrivacySettings(settings).succeeded(),
          "bounded local retention can be configured independently of network sharing");
    model.recordLocalResult(resultFixture("retain-1"));
    model.recordLocalResult(resultFixture("retain-2"));
    model.recordLocalResult(resultFixture("retain-3"));
    check(model.history().size() == 2u &&
              model.history()[0].result.resultId == "retain-2" &&
              model.history()[1].result.resultId == "retain-3" &&
              model.active() != nullptr && model.active()->result.resultId == "retain-3",
          "retention rotates the oldest local result and preserves the current result");

    settings.retainedLocalResults = 1u;
    check(model.setPrivacySettings(settings).succeeded() && model.history().size() == 1u &&
              model.active() != nullptr && model.active()->result.resultId == "retain-3",
          "lowering retention immediately prunes only older history");
}

void testLocalExportDeleteAndClearAreExplicit() {
    CompatibilityShareModel model;
    CompatibilityPrivacySettings settings;
    settings.retainedLocalResults = 7u;
    check(model.setPrivacySettings(settings).succeeded(),
          "local export fixture uses explicit retention metadata while sharing stays disabled");
    const auto first = resultFixture("privacy-export-1", true);
    check(model.recordLocalResult(first).succeeded(), "local result is available for privacy controls");

    std::string exported = "sentinel";
    check(model.exportActiveLocalResult(exported).code == ShareModelCode::PreviewRequired &&
              exported == "sentinel",
          "local export cannot bypass exact preview approval");

    CompatibilityLocalExportPreview preview;
    check(model.prepareActiveLocalExport(preview).succeeded() &&
              preview.canonicalizationVersion == kCompatibilityCanonicalizationVersion &&
              preview.resultId == first.resultId && !preview.exactRedactedJson.empty() &&
              preview.payloadIdentity.starts_with("compat-v1-") && preview.generation != 0u,
          "local export preview freezes canonical bytes, version, identity, result, and generation");
    auto changed = preview;
    changed.exactRedactedJson[changed.exactRedactedJson.size() / 2u] ^= 1;
    check(model.approveLocalExport(changed).code == ShareModelCode::PayloadIdentityMismatch,
          "one-byte local export preview mutation is rejected");
    changed = preview;
    ++changed.canonicalizationVersion;
    check(model.approveLocalExport(changed).code == ShareModelCode::PayloadIdentityMismatch,
          "approval cannot cross a canonicalization-version change");
    check(model.approveLocalExport(preview).succeeded() &&
              model.exportActiveLocalResult(exported).succeeded() &&
              exported == preview.exactRedactedJson,
          "approved local export returns the exact frozen preview bytes without reserialization");
    compat::CompatibilityResult decoded;
    check(compat::decodeCompatibilityResultJson(exported, decoded).succeeded() && decoded == first,
          "approved local export remains the canonical bounded compatibility JSON");

    CompatibilityLocalExportPreview repeated;
    check(model.prepareActiveLocalExport(repeated).succeeded() &&
              repeated.exactRedactedJson == preview.exactRedactedJson &&
              repeated.payloadIdentity == preview.payloadIdentity &&
              repeated.generation != preview.generation,
          "canonical local export bytes and identity are stable while consent generation is one-use");
    check(model.approveLocalExport(preview).code == ShareModelCode::StaleApproval,
          "byte-identical old local export approval cannot be replayed");
    check(model.declineLocalExport().succeeded(),
          "declining the current local export preview is an explicit non-authorizing action");
    exported = "sentinel";
    check(model.exportActiveLocalResult(exported).code == ShareModelCode::PreviewRequired &&
              exported == "sentinel",
          "declining a preview cannot authorize a later export");

    check(model.prepareActiveLocalExport(repeated).succeeded() &&
              model.approveLocalExport(repeated).succeeded(),
          "fresh export approval can be prepared before a retention change");
    settings.retainedLocalResults = 6u;
    check(model.setPrivacySettings(settings).succeeded(),
          "retention metadata can be changed independently of technical truth");
    exported = "sentinel";
    check(model.exportActiveLocalResult(exported).code == ShareModelCode::PreviewRequired &&
              model.active() != nullptr && model.active()->result == first,
          "changed retention metadata invalidates export consent without changing technical result");

    check(model.deleteLocalResult("missing-result").code == ShareModelCode::ResultNotFound &&
              model.history().size() == 1u,
          "deleting an unknown result fails without changing local evidence");

    const auto second = resultFixture("privacy-export-2");
    model.recordLocalResult(second);
    check(model.prepareLocalExport("privacy-export-1", repeated).succeeded() &&
              model.approveLocalExport(repeated).succeeded(),
          "a superseded result may be explicitly previewed for local export");
    check(model.exportLocalResult("privacy-export-2", exported).code == ShareModelCode::StaleApproval,
          "approval for one result cannot authorize another result with different bytes");
    check(model.deleteLocalResult("privacy-export-1").succeeded() &&
              model.history().size() == 1u && model.active() != nullptr &&
              model.active()->result.resultId == "privacy-export-2",
          "deleting superseded history preserves the active local result");
    exported = "sentinel";
    check(model.exportLocalResult("privacy-export-2", exported).code == ShareModelCode::PreviewRequired,
          "deleting selected local authority revokes its export approval without promoting it");
    check(model.deleteLocalResult("privacy-export-2").succeeded() &&
              model.active() == nullptr && model.state() == CompatibilityShareState::TestNotRun,
          "deleting the active result never silently promotes older evidence");

    model.recordLocalResult(resultFixture("privacy-clear-1"));
    model.recordLocalResult(resultFixture("privacy-clear-2"));
    const auto settingsBeforeClear = model.privacySettings();
    check(model.clearLocalResults().succeeded() && model.history().empty() &&
              model.active() == nullptr && model.state() == CompatibilityShareState::TestNotRun &&
              model.privacySettings() == settingsBeforeClear,
          "clear removes only local compatibility-result authority and preserves privacy preferences");
}

void testPrivacySettingsPersistenceIsStrictAndTransactional() {
    CompatibilityShareModel model;
    std::string defaultJson;
    check(model.exportPrivacySettingsJson(defaultJson).succeeded() &&
              defaultJson ==
                  "{\"schemaVersion\":1,\"communitySharingEnabled\":false,\"retainedLocalResults\":32}",
          "default privacy settings export is canonical, versioned, and sharing-off");

    CompatibilityPrivacySettings settings;
    settings.communitySharingEnabled = true;
    settings.retainedLocalResults = 7u;
    check(model.setPrivacySettings(settings).succeeded(),
          "non-default privacy settings are accepted before persistence");
    std::string persisted;
    check(model.exportPrivacySettingsJson(persisted).succeeded() &&
              persisted ==
                  "{\"schemaVersion\":1,\"communitySharingEnabled\":true,\"retainedLocalResults\":7}",
          "privacy settings encode deterministically without unrelated local state");

    CompatibilityShareModel restored;
    check(restored.loadPrivacySettingsJson(
              "{ \"retainedLocalResults\" : 7, \"communitySharingEnabled\" : true, \"schemaVersion\" : 1 }")
              .succeeded() &&
              restored.privacySettings() == settings,
          "privacy settings restore accepts field reordering and JSON whitespace");

    const auto beforeInvalid = restored.privacySettings();
    const std::vector<std::string> invalidDocuments = {
        "{}",
        "{\"schemaVersion\":2,\"communitySharingEnabled\":true,\"retainedLocalResults\":7}",
        "{\"schemaVersion\":1,\"communitySharingEnabled\":true,\"retainedLocalResults\":0}",
        "{\"schemaVersion\":1,\"communitySharingEnabled\":true,\"retainedLocalResults\":65}",
        "{\"schemaVersion\":1,\"communitySharingEnabled\":\"true\",\"retainedLocalResults\":7}",
        "{\"schemaVersion\":1,\"communitySharingEnabled\":true,\"retainedLocalResults\":07}",
        "{\"schemaVersion\":1,\"communitySharingEnabled\":true,\"retainedLocalResults\":7,\"unknown\":1}",
        "{\"schemaVersion\":1,\"schemaVersion\":1,\"communitySharingEnabled\":true,\"retainedLocalResults\":7}",
        "{\"schemaVersion\":1,\"communitySharingEnabled\":true,\"retainedLocalResults\":7} trailing",
    };
    for (const auto& document : invalidDocuments) {
        check(restored.loadPrivacySettingsJson(document).code == ShareModelCode::InvalidPrivacySettings &&
                  restored.privacySettings() == beforeInvalid,
              "malformed/future/unknown privacy settings fail without mutating the previous settings");
    }

    std::string oversized(kMaximumCompatibilityPrivacySettingsBytes + 1u, 'x');
    check(restored.loadPrivacySettingsJson(oversized).code == ShareModelCode::InvalidPrivacySettings &&
              restored.privacySettings() == beforeInvalid,
          "oversized privacy settings fail closed before parsing");
}

void testLocalHistoryPersistenceIsBoundedTransactionalAndPrivacySafe() {
    CompatibilityShareModel model;
    enableSharing(model, 3u);
    const auto first = resultFixture("history-1");
    const auto second = resultFixture("history-2", true);
    const auto third = resultFixture("history-3");
    model.recordLocalResult(first);
    model.recordLocalResult(second);
    model.recordLocalResult(third);

    CompatibilitySharePreview preview;
    check(model.preparePreview(preview).succeeded() &&
              model.beginSubmission(preview).succeeded() &&
              model.state() == CompatibilityShareState::SubmitPending,
          "history fixture includes pending network state before persistence");

    std::string stored;
    check(model.exportLocalHistoryJsonl(stored).succeeded() &&
              stored.starts_with("{\"schemaVersion\":1,\"kind\":\"compatibility-local-history\"}\n") &&
              stored.find("SubmitPending") == std::string::npos &&
              stored.find("receipt") == std::string::npos,
          "local history stores only canonical technical results, not sharing transport state");

    CompatibilityShareModel restored;
    CompatibilityPrivacySettings restoreSettings;
    restoreSettings.communitySharingEnabled = false;
    restoreSettings.retainedLocalResults = 2u;
    restored.setPrivacySettings(restoreSettings);
    check(restored.loadLocalHistoryJsonl(stored).succeeded() && restored.history().size() == 2u &&
              restored.history()[0].result.resultId == "history-2" &&
              restored.history()[0].state == CompatibilityShareState::Superseded &&
              restored.history()[1].result.resultId == "history-3" &&
              restored.history()[1].state == CompatibilityShareState::LocalResultAvailable &&
              restored.active() != nullptr && restored.active()->result.resultId == "history-3" &&
              !restored.privacySettings().communitySharingEnabled,
          "history restore honors current retention, preserves order, and never restores network consent");
    check(restored.history()[0].protectedExperimental(),
          "Protected / Experimental technical truth survives local history persistence");

    std::string beforeInvalid;
    check(restored.exportLocalHistoryJsonl(beforeInvalid).succeeded(),
          "restored local history has a canonical transactional baseline");
    const std::vector<std::string> invalidHistory = {
        "",
        "{\"schemaVersion\":2,\"kind\":\"compatibility-local-history\"}",
        "{\"schemaVersion\":1,\"kind\":\"other\"}",
        "{\"schemaVersion\":1,\"kind\":\"compatibility-local-history\"}\n{}",
        "{\"schemaVersion\":1,\"kind\":\"compatibility-local-history\"}\n",
    };
    for (const auto& invalid : invalidHistory) {
        check(restored.loadLocalHistoryJsonl(invalid).code == ShareModelCode::InvalidLocalHistory,
              "malformed/future local history is rejected");
        std::string afterInvalid;
        check(restored.exportLocalHistoryJsonl(afterInvalid).succeeded() && afterInvalid == beforeInvalid,
              "invalid local history never partially mutates existing technical evidence");
    }

    std::string encodedThird;
    check(compat::encodeCompatibilityResultJson(third, encodedThird).succeeded(),
          "duplicate-history fixture result encodes canonically");
    const std::string duplicateHistory =
        std::string("{\"schemaVersion\":1,\"kind\":\"compatibility-local-history\"}\n") +
        encodedThird + "\n" + encodedThird;
    check(restored.loadLocalHistoryJsonl(duplicateHistory).code == ShareModelCode::InvalidLocalHistory,
          "duplicate result IDs cannot enter the local history store");

    std::string tooMany = "{\"schemaVersion\":1,\"kind\":\"compatibility-local-history\"}";
    for (std::size_t index = 0u; index <= kMaximumRetainedCompatibilityResults; ++index) {
        auto value = resultFixture("history-many-" + std::to_string(index));
        std::string encoded;
        check(compat::encodeCompatibilityResultJson(value, encoded).succeeded(),
              "many-history fixture result encodes");
        tooMany += "\n" + encoded;
    }
    check(restored.loadLocalHistoryJsonl(tooMany).code == ShareModelCode::LocalHistoryTooLarge,
          "local history rejects more than the maximum retained result count");
}

void testInvalidResultNeverCreatesHistory() {
    CompatibilityShareModel model;
    auto invalid = resultFixture("share-invalid");
    invalid.redaction.personalPathsExcluded = false;
    check(model.recordLocalResult(invalid).code == ShareModelCode::InvalidResult &&
              model.history().empty() && model.state() == CompatibilityShareState::TestNotRun,
          "privacy-invalid local result cannot become shareable history");

    invalid = resultFixture("share-machine-name");
    invalid.windowsBuildClass = "DESKTOP-ALICE";
    check(model.recordLocalResult(invalid).code == ShareModelCode::InvalidResult &&
              model.history().empty(),
          "machine names cannot masquerade as a public Windows build class in shareable history");

    invalid = resultFixture("share-architecture");
    invalid.architecture = "alice-workstation";
    check(model.recordLocalResult(invalid).code == ShareModelCode::InvalidResult &&
              model.history().empty(),
          "arbitrary device/user-local architecture labels cannot enter shareable history");
}

} // namespace

int main() {
    testLocalPreviewDeclineFlowKeepsTechnicalResult();
    testOfflineFailureAndExplicitRetryRemainLocalFirst();
    testMismatchedPreviewCannotBeginSubmission();
    testNewLocalResultSupersedesOldRecordWithoutDeletingHistory();
    testDuplicateRuntimeResultIdIsRejectedTransactionally();
    testSharingIsOptInAndDisableStopsPendingTransport();
    testRetentionSettingsAreBoundedAndRotateOldHistory();
    testLocalExportDeleteAndClearAreExplicit();
    testPrivacySettingsPersistenceIsStrictAndTransactional();
    testLocalHistoryPersistenceIsBoundedTransactionalAndPrivacySafe();
    testInvalidResultNeverCreatesHistory();
    if (failures != 0) {
        std::cerr << failures << " compatibility share model test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Compatibility share model tests passed.\n";
    return EXIT_SUCCESS;
}
