#include "hydra/plan_preflight.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::plan;
using namespace hydra::preflight;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

PlanCompileResult successfulPlan() {
    SeatProviderLaunchPlan seat;
    seat.seatId = 2u;
    seat.playerId = "player-2";
    seat.gameId = "game:a";
    seat.setupId = "setup-a";
    seat.instanceIndex = 1u;
    seat.requirementRevision = 4u;
    seat.hardwareFingerprint = 22u;
    seat.requirements.controller = true;
    seat.requirements.audioOutput = true;
    seat.requirements.highRisk = true;
    seat.launchRequest.providerId = "fake";
    seat.launchRequest.gameId = "game:a";
    seat.launchRequest.metadataRevision = 9u;
    seat.launchRequest.targetKind = provider::LaunchTargetKind::ProviderUri;
    seat.launchRequest.target = L"fake://launch/a";
    seat.launchRequest.launchCorrelationId = "correlation";

    ProviderAwareLaunchPlan plan;
    plan.fingerprint = 12345u;
    plan.seats = {seat};
    PlanCompileResult result;
    result.plan = plan;
    return result;
}

void testSuccessfulSummaryShowsRequirementsAndApprovedChanges() {
    const std::vector<PlannedMutation> mutations{
        {"display-2", 2u, MutationKind::DisplayPlacement, false, false},
        {"config-2", 2u, MutationKind::WriteConfig, true, true},
    };
    const auto first = buildSummary(successfulPlan(), mutations);
    const auto second = buildSummary(successfulPlan(),
        std::vector<PlannedMutation>{mutations.rbegin(), mutations.rend()});
    check(first.canActivate && first.planFingerprint == 12345u,
          "successful plan with approved typed mutations may activate");
    check(first == second,
          "mutation input order does not change the human/expert preflight summary");

    bool controller = false;
    bool protectedWarning = false;
    bool mutation = false;
    for (const auto& message : first.messages) {
        controller = controller || message.code == "requires.controller";
        protectedWarning = protectedWarning || message.code == "risk.protected";
        mutation = mutation || message.code == "mutation.WriteConfig";
    }
    check(controller && protectedWarning && mutation,
          "requirements, Protected risk, and planned mutation are visible");
}

void testBlockingIssueHasNormalAndExpertViews() {
    PlanCompileResult failed;
    failed.issues.push_back({PlanIssueCode::MissingController, 2u,
                             "selected game requires a controller"});
    const auto summary = buildSummary(failed);
    check(!summary.canActivate && summary.messages.size() == 1u,
          "blocking compiler result stays blocking");
    check(summary.messages[0].severity == Severity::Blocking &&
              summary.messages[0].userMessage.find("controller") != std::string::npos &&
              summary.messages[0].expertDetail.find("MissingController") != std::string::npos,
          "blocking requirement has normal-user text and deterministic expert detail");
}

void testApprovalAndMutationIdentityFailClosed() {
    const std::vector<PlannedMutation> unapproved{
        {"write-a", 1u, MutationKind::WriteConfig, true, false},
    };
    auto summary = buildSummary(successfulPlan(), unapproved);
    check(!summary.canActivate && summary.messages.front().severity == Severity::Blocking,
          "unapproved mutation prevents activation");

    const std::vector<PlannedMutation> duplicate{
        {"same", 1u, MutationKind::CreateDirectory, false, false},
        {"same", 2u, MutationKind::AudioRoute, false, false},
    };
    summary = buildSummary(successfulPlan(), duplicate);
    check(!summary.canActivate,
          "duplicate mutation identity fails closed instead of hiding one change");
}

void testTypedMutationPreviewCannotCarrySecretPayload() {
    PlannedMutation mutation{"opaque-id", 1u, MutationKind::DeviceRoute, true, true};
    const auto summary = buildSummary(successfulPlan(), std::vector<PlannedMutation>{mutation});
    bool sawOnlyTypedDetail = false;
    for (const auto& message : summary.messages) {
        if (message.code == "mutation.DeviceRoute") {
            sawOnlyTypedDetail = message.expertDetail.find("opaque-id") != std::string::npos &&
                                 message.expertDetail.find("password") == std::string::npos &&
                                 message.expertDetail.find("token") == std::string::npos;
        }
    }
    check(sawOnlyTypedDetail,
          "mutation preview exposes kind/id/approval only, not arbitrary payload fields");
}

} // namespace

int main() {
    testSuccessfulSummaryShowsRequirementsAndApprovedChanges();
    testBlockingIssueHasNormalAndExpertViews();
    testApprovalAndMutationIdentityFailClosed();
    testTypedMutationPreviewCannotCarrySecretPayload();
    if (failures != 0) {
        std::cerr << failures << " plan preflight test(s) failed\n";
        return 1;
    }
    std::cout << "plan preflight tests passed\n";
    return 0;
}
