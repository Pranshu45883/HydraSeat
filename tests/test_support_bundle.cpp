#include "hydra/support_bundle.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace hydra;
using namespace hydra::support;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

metrics::SessionMetricsReport metricsFixture() {
    metrics::SessionMetricsReport report;
    report.planFingerprint = 42u;
    report.origin = metrics::EvidenceOrigin::ControlledProcess;
    report.isolationVerdict = metrics::EvidenceVerdict::Pass;
    report.sessionVerdict = metrics::EvidenceVerdict::Pass;
    report.receiverEvidenceComplete = true;
    report.lossFreeEvidence = true;
    report.seats = {
        {1u, 100u, 50u, 20u, true, true, true, true,
         metrics::CapabilityOutcome::Success, metrics::CapabilityOutcome::NotRequired},
        {2u, 110u, 55u, 18u, true, true, true, true,
         metrics::CapabilityOutcome::Success, metrics::CapabilityOutcome::NotRequired},
    };
    report.finalState = metrics::SessionFinalState::ReturnedToWindows;
    report.maximumLaunchDurationMicros = 110u;
    report.maximumStopDurationMicros = 55u;
    return report;
}

compat::CompatibilityResult compatibilityFixture() {
    compat::CompatibilityResult value;
    value.resultId = "support-result";
    value.timestampClass = compat::TimestampClass::MonthBucket;
    value.timestampBucket = "2026-08";
    value.gameId = "game:support";
    value.providerId = "custom";
    value.providerAppId = "support-app";
    value.gameVersion = "1.0";
    value.hydraSeatVersion = "0.1.0";
    value.hydraSeatBuild = "e80d1fb";
    value.windowsBuildClass = "win10-2009";
    value.architecture = "x64";
    value.scenario = compat::Scenario::DifferentGames;
    value.launch = compat::EvidenceStatus::Pass;
    value.inputIsolation = compat::EvidenceStatus::Pass;
    value.cleanExit = compat::EvidenceStatus::Pass;
    value.origin = compat::ResultOrigin::ControlledProcess;
    value.provenanceId = "support-fixture";
    value.provenanceRevision = 1u;
    return value;
}

recovery::CrashJournalState journalFixture() {
    recovery::CrashJournalState state;
    state.runtimeGeneration = 5u;
    state.phase = recovery::CrashJournalPhase::Clean;
    state.finalResult = recovery::CrashJournalFinalResult::Clean;
    state.records = {
        {1u, recovery::CrashJournalRecordKind::ActivationStarted, 0u, 5u},
        {2u, recovery::CrashJournalRecordKind::CleanStop, 0u, 5u},
    };
    return state;
}

SupportBundleInput input() {
    static const auto metrics = metricsFixture();
    static const auto journal = journalFixture();
    static const auto compatibility = compatibilityFixture();
    SupportBundleInput value;
    value.hydraSeatVersion = "0.1.0";
    value.hydraSeatBuild = "e80d1fb";
    value.windowsBuildClass = "win10-2009";
    value.architecture = "x64";
    value.sessionMetrics = &metrics;
    value.crashJournal = &journal;
    value.compatibility = &compatibility;
    value.events = {{"runtime.activation-complete", 1u, 5u},
                    {"runtime.clean-stop", 2u, 5u}};
    return value;
}

void testBundleIsDeterministicPreviewableAndPrivacyBounded() {
    SupportBundle first;
    SupportBundle second;
    auto firstInput = input();
    auto secondInput = input();
    std::reverse(secondInput.events.begin(), secondInput.events.end());
    check(buildSupportBundle(firstInput, first).succeeded() &&
              buildSupportBundle(secondInput, second).succeeded() && first == second,
          "support event input order canonicalizes into one deterministic bundle");

    std::string json;
    check(encodeSupportBundleJson(first, json).succeeded(),
          "privacy-safe support bundle encodes");
    check(json.find("credentials_excluded\":true") != std::string::npos &&
              json.find("player_names_excluded\":true") != std::string::npos &&
              json.find("personal_paths_excluded\":true") != std::string::npos &&
              json.find("raw_typed_text_excluded\":true") != std::string::npos,
          "bundle JSON declares mandatory redaction dimensions");
    check(json.find("C:\\\\Users") == std::string::npos &&
              json.find("password") == std::string::npos &&
              json.find("opaque-account") == std::string::npos,
          "bundle contains no sampled personal path/credential/account values");

    const auto preview = buildSupportBundlePreview(first);
    check(preview.find("Session metrics: included") != std::string::npos &&
              preview.find("Recovery summary: included") != std::string::npos &&
              preview.find("Redaction:") != std::string::npos,
          "human preview states exactly which evidence classes and redaction are included");
}

void testJournalIsReducedToNonIdentifyingSummary() {
    SupportBundle bundle;
    check(buildSupportBundle(input(), bundle).succeeded() && bundle.recovery.has_value(),
          "recovery state contributes a bounded summary");
    check(bundle.recovery->recordCount == 2u && bundle.recovery->snapshotCount == 0u &&
              bundle.recovery->runtimeGeneration == 5u,
          "support bundle retains phase/generation/counts only from crash journal");
    std::string json;
    encodeSupportBundleJson(bundle, json);
    check(json.find("session_id") == std::string::npos &&
              json.find("plan_hash") == std::string::npos &&
              json.find("journal_hash") == std::string::npos,
          "support bundle excludes raw journal session/hash identities");
}

void testSensitiveOrMalformedFieldsFailTransactionally() {
    SupportBundle output;
    output.hydraSeatVersion = "sentinel";
    const auto sentinel = output;

    auto invalid = input();
    invalid.hydraSeatBuild = "C:\\Users\\Alice\\build";
    check(buildSupportBundle(invalid, output).code == SupportCode::InvalidEnvironment &&
              output == sentinel,
          "personal absolute path cannot enter public support environment fields");

    invalid = input();
    invalid.windowsBuildClass = "DESKTOP-ALICE";
    check(buildSupportBundle(invalid, output).code == SupportCode::InvalidEnvironment &&
              output == sentinel,
          "machine names cannot masquerade as a public Windows build class");

    invalid = input();
    invalid.events.push_back({"bad event with spaces", 1u, 5u});
    check(buildSupportBundle(invalid, output).code == SupportCode::InvalidEvent &&
              output == sentinel,
          "support events accept stable codes only, not arbitrary diagnostic text");
}

void testRedactionContractCannotBeWeakenedBeforeExport() {
    SupportBundle bundle;
    buildSupportBundle(input(), bundle);
    bundle.playerNamesExcluded = false;
    std::string output = "sentinel";
    check(encodeSupportBundleJson(bundle, output).code == SupportCode::RedactionRequired &&
              output == "sentinel",
          "export refuses a support bundle whose mandatory redaction contract was weakened");
}

void testExactPreviewMustBeApprovedBeforeExport() {
    SupportBundle bundle;
    check(buildSupportBundle(input(), bundle).succeeded(), "support bundle fixture builds");

    SupportExportSession session;
    std::string exported = "sentinel";
    check(session.exportApproved(exported).code == SupportCode::PreviewRequired &&
              exported == "sentinel",
          "support bundle cannot export before preview and approval");

    SupportExportPreview preview;
    check(session.prepare(bundle, preview).succeeded() && session.hasPreview() &&
              !session.approved() && !preview.exactJson.empty() &&
              !preview.payloadIdentity.empty() && preview.generation != 0u &&
              preview.humanSummary.find("Redaction:") != std::string::npos,
          "support bundle produces human, exact JSON, and exact payload identity together");

    auto changedPreview = preview;
    changedPreview.exactJson.push_back(' ');
    check(session.approve(changedPreview).code == SupportCode::PayloadIdentityMismatch &&
              !session.approved(),
          "approval is bound to the exact preview bytes rather than a recomputed bundle");
    changedPreview = preview;
    changedPreview.payloadIdentity.push_back('x');
    check(session.approve(changedPreview).code == SupportCode::PayloadIdentityMismatch &&
              !session.approved(),
          "approval rejects a mismatched deterministic payload identity");
    changedPreview = preview;
    ++changedPreview.canonicalizationVersion;
    check(session.approve(changedPreview).code == SupportCode::PayloadIdentityMismatch &&
              !session.approved(),
          "approval rejects a changed support canonicalization version");
    check(session.approve(preview).succeeded() && session.approved(),
          "exact preview identity can be explicitly approved");
    bundle.events.push_back({"runtime.after-approval", 1u, 6u});
    check(session.exportApproved(exported).succeeded() && exported == preview.exactJson &&
              exported.find("runtime.after-approval") == std::string::npos,
          "approved export returns frozen preview bytes even if source bundle changes afterward");
    bundle.events.pop_back();

    check(session.prepare(bundle, changedPreview).succeeded() &&
              changedPreview.exactJson == preview.exactJson &&
              changedPreview.payloadIdentity == preview.payloadIdentity &&
              changedPreview.generation != preview.generation,
          "canonical support payload is deterministic while consent generation changes");
    check(session.approve(preview).code == SupportCode::StaleApproval && !session.approved(),
          "old approval cannot be replayed after a byte-identical re-preview");

    auto invalidBundle = bundle;
    invalidBundle.playerNamesExcluded = false;
    check(session.prepare(invalidBundle, changedPreview).code == SupportCode::RedactionRequired &&
              !session.hasPreview() && !session.approved(),
          "failed preparation of a new bundle revokes any previous export approval");
    exported = "sentinel";
    check(session.exportApproved(exported).code == SupportCode::PreviewRequired &&
              exported == "sentinel",
          "stale previously-approved support payload cannot export after a failed re-prepare");
}

} // namespace

int main() {
    testBundleIsDeterministicPreviewableAndPrivacyBounded();
    testJournalIsReducedToNonIdentifyingSummary();
    testSensitiveOrMalformedFieldsFailTransactionally();
    testRedactionContractCannotBeWeakenedBeforeExport();
    testExactPreviewMustBeApprovedBeforeExport();
    if (failures != 0) {
        std::cerr << failures << " support bundle test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Support bundle tests passed.\n";
    return EXIT_SUCCESS;
}
