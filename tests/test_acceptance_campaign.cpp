#include "hydra/acceptance_campaign.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using namespace hydra::acceptance;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

CampaignIdentity identity() {
    return {"campaign-001", std::string(40u, 'a'), std::string(64u, 'b'),
            "HydraSeat-x64.zip", 7u, "x64", std::string(64u, 'c'),
            std::string(64u, 'd'), "10.0.26100", std::string(64u, 'e'),
            {1u, 2u}, "rc-two-seat-001", "campaign-001"};
}

ChildEvidence evidence(const AcceptanceCampaign& campaign, CampaignStage stage,
                       std::uint64_t now, EvidenceOrigin origin = EvidenceOrigin::Physical,
                       std::string id = "evidence-001") {
    ChildEvidence value;
    value.evidenceId = std::move(id);
    value.stage = stage;
    value.origin = origin;
    value.createdUnixSeconds = now;
    value.contentSha256 = std::string(64u, 'f');
    value.evidenceArtifactName = value.evidenceId + ".json";
    value.testName = "test-" + value.evidenceId;
    value.rcCommitSha = campaign.identity.rcCommitSha;
    value.releaseArtifactSha256 = campaign.identity.releaseArtifactSha256;
    value.releaseRevision = campaign.identity.releaseRevision;
    value.architecture = campaign.identity.architecture;
    value.profileSha256 = campaign.identity.profileSha256;
    value.installStateSha256 = campaign.identity.installStateSha256;
    value.scenarioIdentity = campaign.identity.scenarioIdentity;
    value.automatedPassed = true;
    value.campaignSchemaVersion = campaign.schemaVersion;
    value.campaignId = campaign.identity.campaignId;
    value.sessionRunId = campaign.identity.sessionRunId;
    value.releaseArtifactName = campaign.identity.releaseArtifactName;
    value.windowsBuild = campaign.identity.windowsBuild;
    value.topologyFingerprintSha256 = campaign.identity.topologyFingerprintSha256;
    value.evidenceClass = evidenceClassForStage(stage);
    return value;
}

bool passStage(AcceptanceCampaign& campaign, CampaignStage stage, std::uint64_t& now) {
    if (!startStage(campaign, stage, ++now).succeeded()) return false;
    const auto origin = stageRequiresPhysicalEvidence(stage)
                            ? EvidenceOrigin::Physical
                            : EvidenceOrigin::ControlledProcess;
    auto child = evidence(campaign, stage, ++now, origin,
                          "stage-" + std::to_string(static_cast<unsigned>(stage)));
    if (!attachEvidence(campaign, child, now).succeeded()) return false;
    if (stageRequiresManualReview(stage)) {
        if (!recordManualVerdict(campaign, stage, HumanVerdict::Pass,
                                 "tester reviewed exact evidence", ++now).succeeded()) {
            return false;
        }
    }
    return true;
}

void testIdentityAndExactlyTwoSeats() {
    CampaignDiagnostic diagnostic;
    auto campaign = makeCampaign(identity(), 1000u, &diagnostic);
    check(diagnostic.succeeded() && campaign.stages.size() == kCampaignStageCount,
          "valid exact two-Seat campaign initializes every stage");

    auto invalid = identity();
    invalid.seatIds = {1u, 3u};
    (void)makeCampaign(invalid, 1000u, &diagnostic);
    check(diagnostic.code == CampaignCode::InvalidIdentity,
          "third Seat campaign identity is rejected");

    invalid = identity();
    invalid.seatIds = {1u, 1u};
    (void)makeCampaign(invalid, 1000u, &diagnostic);
    check(diagnostic.code == CampaignCode::InvalidIdentity,
          "duplicate Seat identity is rejected");
}

void testPhysicalEvidenceCannotBeInvented() {
    std::uint64_t now = 2000u;
    auto campaign = makeCampaign(identity(), now);
    check(passStage(campaign, CampaignStage::Preflight, now),
          "preflight passes from controlled exact evidence");
    check(startStage(campaign, CampaignStage::Phase3Physical, ++now).succeeded(),
          "physical stage starts after preflight");

    auto synthetic = evidence(campaign, CampaignStage::Phase3Physical, now,
                              EvidenceOrigin::Synthetic, "synthetic-physical");
    check(attachEvidence(campaign, synthetic, now).code ==
              CampaignCode::PhysicalEvidenceRequired,
          "synthetic evidence cannot become physical validation");
    auto controlledAsPhysical = evidence(campaign, CampaignStage::Phase3Physical, now,
                                         EvidenceOrigin::ControlledProcess,
                                         "controlled-labelled-physical");
    controlledAsPhysical.evidenceClass = EvidenceClass::Physical;
    check(attachEvidence(campaign, controlledAsPhysical, now).code ==
              CampaignCode::PhysicalEvidenceRequired,
          "controlled evidence labelled as Physical is rejected by independent origin binding");
    auto wrongClass = evidence(campaign, CampaignStage::Phase3Physical, now,
                               EvidenceOrigin::Physical, "physical-wrong-class");
    wrongClass.evidenceClass = EvidenceClass::CleanMachineInstall;
    check(attachEvidence(campaign, wrongClass, now).code == CampaignCode::EvidenceClassMismatch,
          "physical origin cannot substitute a clean-machine evidence class for a physical stage");

    auto preauthorized = evidence(campaign, CampaignStage::Phase3Physical, now,
                                  EvidenceOrigin::Physical, "preauthorized-physical");
    preauthorized.humanVerdict = HumanVerdict::Pass;
    check(attachEvidence(campaign, preauthorized, now).code ==
              CampaignCode::ManualVerdictRequired,
          "physical evidence cannot carry a pre-authorized human verdict");

    auto physical = evidence(campaign, CampaignStage::Phase3Physical, now,
                             EvidenceOrigin::Physical, "real-physical");
    check(attachEvidence(campaign, physical, now).succeeded(),
          "physical evidence enters manual review rather than auto-pass");
    check(campaign.stages[1].state == StageState::AwaitingManualReview,
          "physical automated evidence still awaits a person");
    check(recordManualVerdict(campaign, CampaignStage::Phase3Physical,
                              HumanVerdict::Pending, "review", ++now).code ==
              CampaignCode::ManualVerdictRequired,
          "pending is not accepted as a human verdict");
    check(recordManualVerdict(campaign, CampaignStage::Phase3Physical,
                              HumanVerdict::Pass, "two physical input sets reviewed", ++now)
              .succeeded() && campaign.stages[1].state == StageState::Passed &&
              campaign.stages[1].evidence.size() == 1u &&
              campaign.stages[1].evidence[0].humanVerdict == HumanVerdict::Pass,
          "explicit bounded manual pass completes physical stage and binds the evidence verdict");

    auto forgedPending = campaign;
    forgedPending.stages[1].evidence[0].humanVerdict = HumanVerdict::Pending;
    check(validateCampaign(forgedPending, now).code == CampaignCode::ManualVerdictRequired,
          "manual/physical PENDING evidence cannot be persisted as a validated stage");
}

void testStaleWrongRcDuplicateAndFailure() {
    std::uint64_t now = 1000000u;
    auto campaign = makeCampaign(identity(), now);
    check(startStage(campaign, CampaignStage::Preflight, now).succeeded(),
          "preflight starts");

    auto stale = evidence(campaign, CampaignStage::Preflight,
                          now - kMaximumEvidenceAgeSeconds - 1u,
                          EvidenceOrigin::ControlledProcess, "stale");
    check(attachEvidence(campaign, stale, now).code == CampaignCode::EvidenceStale,
          "stale evidence is rejected");

    auto wrongRc = evidence(campaign, CampaignStage::Preflight, now,
                            EvidenceOrigin::ControlledProcess, "wrong-rc");
    wrongRc.rcCommitSha = std::string(40u, 'f');
    check(attachEvidence(campaign, wrongRc, now).code ==
              CampaignCode::EvidenceIdentityMismatch,
          "evidence from a different RC is rejected");

    auto wrongArtifact = evidence(campaign, CampaignStage::Preflight, now,
                                  EvidenceOrigin::ControlledProcess, "wrong-artifact");
    wrongArtifact.releaseArtifactSha256 = std::string(64u, 'a');
    check(attachEvidence(campaign, wrongArtifact, now).code ==
              CampaignCode::EvidenceIdentityMismatch,
          "evidence from the wrong release artifact hash is rejected");

    auto wrongBuild = evidence(campaign, CampaignStage::Preflight, now,
                               EvidenceOrigin::ControlledProcess, "wrong-build");
    wrongBuild.releaseRevision += 1u;
    check(attachEvidence(campaign, wrongBuild, now).code ==
              CampaignCode::EvidenceIdentityMismatch,
          "evidence from the wrong release revision is rejected");
    wrongBuild = evidence(campaign, CampaignStage::Preflight, now,
                          EvidenceOrigin::ControlledProcess, "wrong-arch");
    wrongBuild.architecture = "arm64";
    check(attachEvidence(campaign, wrongBuild, now).code ==
              CampaignCode::EvidenceIdentityMismatch,
          "evidence from the wrong architecture is rejected");
    wrongBuild = evidence(campaign, CampaignStage::Preflight, now,
                          EvidenceOrigin::ControlledProcess, "wrong-profile");
    wrongBuild.profileSha256 = std::string(64u, 'a');
    check(attachEvidence(campaign, wrongBuild, now).code ==
              CampaignCode::EvidenceIdentityMismatch,
          "evidence from the wrong profile is rejected");
    wrongBuild = evidence(campaign, CampaignStage::Preflight, now,
                          EvidenceOrigin::ControlledProcess, "wrong-install-state");
    wrongBuild.installStateSha256 = std::string(64u, 'a');
    check(attachEvidence(campaign, wrongBuild, now).code ==
              CampaignCode::EvidenceIdentityMismatch,
          "evidence from the wrong installed-build state is rejected");
    wrongBuild = evidence(campaign, CampaignStage::Preflight, now,
                          EvidenceOrigin::ControlledProcess, "wrong-session");
    wrongBuild.sessionRunId = "foreign-session";
    check(attachEvidence(campaign, wrongBuild, now).code ==
              CampaignCode::EvidenceIdentityMismatch,
          "evidence from a different campaign session/run is rejected");
    wrongBuild = evidence(campaign, CampaignStage::Preflight, now,
                          EvidenceOrigin::ControlledProcess, "wrong-windows-build");
    wrongBuild.windowsBuild = "10.0.19045";
    check(attachEvidence(campaign, wrongBuild, now).code ==
              CampaignCode::EvidenceIdentityMismatch,
          "evidence from a different Windows build is rejected");
    wrongBuild = evidence(campaign, CampaignStage::Preflight, now,
                          EvidenceOrigin::ControlledProcess, "wrong-topology");
    wrongBuild.topologyFingerprintSha256 = std::string(64u, 'a');
    check(attachEvidence(campaign, wrongBuild, now).code ==
              CampaignCode::EvidenceIdentityMismatch,
          "evidence from different topology/input-artifact identity is rejected");

    auto failed = evidence(campaign, CampaignStage::Preflight, now,
                           EvidenceOrigin::ControlledProcess, "preflight-failed");
    failed.automatedPassed = false;
    check(attachEvidence(campaign, failed, now).succeeded() &&
              campaign.stages[0].state == StageState::Failed,
          "failed automated evidence cannot be manually promoted");

    check(startStage(campaign, CampaignStage::Preflight, ++now).succeeded(),
          "failed stage may retry explicitly");
    auto duplicate = evidence(campaign, CampaignStage::Preflight, now,
                              EvidenceOrigin::ControlledProcess, "preflight-failed");
    check(attachEvidence(campaign, duplicate, now).succeeded(),
          "retry replaces prior attempt evidence instead of retaining stale attempt authority");
    check(attachEvidence(campaign, duplicate, now).code == CampaignCode::InvalidTransition,
          "terminal stage refuses additional duplicate evidence");
}

void testMissingDuplicateAndConflictingEvidenceFailClosed() {
    std::uint64_t now = 2000000u;
    auto campaign = makeCampaign(identity(), now);
    check(passStage(campaign, CampaignStage::Preflight, now),
          "evidence integrity fixture has a valid passed preflight");

    auto missing = campaign;
    missing.stages[0].evidence.clear();
    check(validateCampaign(missing, now).code == CampaignCode::ManualVerdictRequired,
          "passed stage with missing required evidence is rejected");

    auto duplicate = campaign;
    auto conflicting = duplicate.stages[0].evidence[0];
    conflicting.evidenceId = "conflicting-id";
    conflicting.contentSha256 = std::string(64u, 'a');
    duplicate.stages[0].evidence.push_back(conflicting);
    check(validateCampaign(duplicate, now).code == CampaignCode::EvidenceDuplicate,
          "duplicate/conflicting evidence artifact or test identity is rejected");

    auto wrongStage = campaign;
    wrongStage.stages[0].evidence[0].stage = CampaignStage::Offline;
    check(validateCampaign(wrongStage, now).code == CampaignCode::InvalidCampaign,
          "evidence attached under the wrong required stage is rejected");

    auto mixedClass = campaign;
    mixedClass.stages[0].evidence[0].evidenceClass = EvidenceClass::SigningDeployment;
    check(validateCampaign(mixedClass, now).code == CampaignCode::EvidenceClassMismatch,
          "mixed campaign evidence cannot silently substitute signing/deployment for controlled preflight");
}

void testInterruptedRunBecomesRecoveryRequired() {
    std::uint64_t now = 3000u;
    auto campaign = makeCampaign(identity(), now);
    check(startStage(campaign, CampaignStage::Preflight, ++now).succeeded(),
          "stage begins before interruption");
    check(recoverInterrupted(campaign, ++now).succeeded() &&
              campaign.stages[0].state == StageState::RecoveryRequired,
          "running stage resumes as RecoveryRequired, never success");
    check(startStage(campaign, CampaignStage::Phase3Physical, ++now).code ==
              CampaignCode::InvalidTransition,
          "dependent stage stays blocked until recovery retry passes");
}

void testStrictRoundTripAndTransactionalDecode() {
    std::uint64_t now = 4000u;
    auto campaign = makeCampaign(identity(), now);
    check(passStage(campaign, CampaignStage::Preflight, now),
          "round-trip fixture preflight passes");
    const auto json = encodeCampaignJson(campaign);
    AcceptanceCampaign decoded;
    check(decodeCampaignJson(json, decoded, now).succeeded() && decoded == campaign,
          "canonical campaign JSON round trips");

    const auto previous = decoded;
    const auto duplicateKey = std::string("{\"schema_version\":2,\"schema_version\":2}");
    check(decodeCampaignJson(duplicateKey, decoded, now).code == CampaignCode::DecodeFailed &&
              decoded == previous,
          "duplicate-key decode failure preserves caller output");

    const std::string oversized(kMaximumCampaignBytes + 1u, 'x');
    check(decodeCampaignJson(oversized, decoded, now).code ==
              CampaignCode::DocumentTooLarge && decoded == previous,
          "oversized input is rejected transactionally");
}

void testOutOfOrderTerminalReceiptRepresentation() {
    std::uint64_t now = 6000u;
    auto campaign = makeCampaign(identity(), now);
    auto offline = evidence(campaign, CampaignStage::Offline, ++now,
                            EvidenceOrigin::ControlledProcess, "imported-offline");
    auto& record = campaign.stages[static_cast<std::size_t>(CampaignStage::Offline)];
    record.state = StageState::Passed;
    record.attempt = 1u;
    record.startedUnixSeconds = offline.createdUnixSeconds;
    record.completedUnixSeconds = offline.createdUnixSeconds;
    record.evidence = {offline};
    check(validateCampaign(campaign, now).succeeded(),
          "already-produced exact terminal evidence can be represented out of stage execution order");
    check(startStage(campaign, CampaignStage::FaultRecovery, ++now).code ==
              CampaignCode::InvalidTransition,
          "live stage execution still requires every earlier prerequisite to pass");
}

void testStoreRecoversVerifiedBackup() {
    std::uint64_t now = 5000u;
    const auto root = std::filesystem::temp_directory_path() /
        ("hydraseat-acceptance-test-" + std::to_string(now));
    std::error_code ignored;
    std::filesystem::create_directories(root, ignored);
    const auto path = root / "campaign.json";
    const auto staged = std::filesystem::path(path.string() + ".new");
    CampaignStore store(path);

    {
        std::ofstream stale(staged, std::ios::binary | std::ios::trunc);
        stale << "stale-sensitive-campaign";
    }
    auto first = makeCampaign(identity(), now);
    check(store.write(first).succeeded() && !std::filesystem::exists(staged),
          "initial transactional campaign write purges stale staging bytes");
    auto second = first;
    second.updatedUnixSeconds = ++now;
    check(store.write(second).succeeded(), "second write rotates a verified backup");

    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "partial";
    }
    AcceptanceCampaign recovered;
    check(store.load(recovered, now).succeeded() && recovered == first,
          "corrupt current document falls back to strict prior campaign backup");
}

} // namespace

int main() {
    testIdentityAndExactlyTwoSeats();
    testPhysicalEvidenceCannotBeInvented();
    testStaleWrongRcDuplicateAndFailure();
    testMissingDuplicateAndConflictingEvidenceFailClosed();
    testInterruptedRunBecomesRecoveryRequired();
    testStrictRoundTripAndTransactionalDecode();
    testOutOfOrderTerminalReceiptRepresentation();
    testStoreRecoversVerifiedBackup();
    if (failures != 0) {
        std::cerr << failures << " acceptance campaign test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Acceptance campaign tests passed.\n";
    return EXIT_SUCCESS;
}
