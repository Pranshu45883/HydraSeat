#include "hydra/acceptance_campaign.hpp"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

using namespace hydra::acceptance;

void usage() {
    std::cout
        << "HydraSeat v1 acceptance campaign control\n\n"
        << "  new <file> <campaign> <commit> <artifact-sha> <artifact-name> "
           "<release-revision> <architecture> <profile-sha> <install-state-sha> "
           "<windows-build> <topology-sha> <scenario> <unix>\n"
        << "  start <file> <stage> <unix>\n"
        << "  attach <file> <evidence-id> <stage> <origin> <created-unix> "
           "<content-sha> <evidence-artifact-name> <test-name> "
           "<automated-pass:true|false> <note> <unix>\n"
        << "  verdict <file> <stage> <Pass|Fail> <note> <unix>\n"
        << "  recover <file> <unix>\n"
        << "  summary <file> <unix>\n";
}

std::optional<std::uint64_t> number(std::string_view text) {
    std::uint64_t value = 0u;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
               ? std::optional(value) : std::nullopt;
}

std::optional<CampaignStage> stage(std::string_view text) {
    for (std::uint8_t raw = 0u; raw < kCampaignStageCount; ++raw) {
        const auto value = static_cast<CampaignStage>(raw);
        if (campaignStageName(value) == text) return value;
    }
    return std::nullopt;
}

std::optional<EvidenceOrigin> origin(std::string_view text) {
    for (std::uint8_t raw = 0u; raw <= static_cast<std::uint8_t>(EvidenceOrigin::Physical); ++raw) {
        const auto value = static_cast<EvidenceOrigin>(raw);
        if (evidenceOriginName(value) == text) return value;
    }
    return std::nullopt;
}

int report(const CampaignDiagnostic& diagnostic) {
    if (diagnostic.succeeded()) return EXIT_SUCCESS;
    std::cerr << campaignCodeName(diagnostic.code) << ": " << diagnostic.message << '\n';
    return EXIT_FAILURE;
}

void summary(const AcceptanceCampaign& campaign) {
    std::cout << "campaign=" << campaign.identity.campaignId
              << " commit=" << campaign.identity.rcCommitSha
              << " artifact=" << campaign.identity.releaseArtifactSha256 << '\n';
    for (const auto& record : campaign.stages) {
        std::cout << campaignStageName(record.stage) << '=' << stageStateName(record.state)
                  << " attempt=" << record.attempt
                  << " evidence=" << record.evidence.size();
        if (!record.diagnostic.empty()) std::cout << " diagnostic=\"" << record.diagnostic << '"';
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return EXIT_FAILURE; }
    const std::string_view command(argv[1]);
    if (command == "new") {
        if (argc != 15) { usage(); return EXIT_FAILURE; }
        const auto revision = number(argv[7]);
        const auto now = number(argv[14]);
        if (!revision || !now) { usage(); return EXIT_FAILURE; }
        CampaignIdentity identity{argv[3], argv[4], argv[5], argv[6], *revision,
                                  argv[8], argv[9], argv[10], argv[11], argv[12],
                                  {1u, 2u}, argv[13]};
        CampaignDiagnostic diagnostic;
        const auto campaign = makeCampaign(identity, *now, &diagnostic);
        if (!diagnostic.succeeded()) return report(diagnostic);
        return report(CampaignStore(argv[2]).write(campaign));
    }
    if (command == "start") {
        if (argc != 5) { usage(); return EXIT_FAILURE; }
        const auto selected = stage(argv[3]);
        const auto now = number(argv[4]);
        if (!selected || !now) { usage(); return EXIT_FAILURE; }
        CampaignStore store(argv[2]);
        AcceptanceCampaign campaign;
        auto diagnostic = store.load(campaign, *now);
        if (diagnostic.succeeded()) diagnostic = startStage(campaign, *selected, *now);
        if (diagnostic.succeeded()) diagnostic = store.write(campaign);
        return report(diagnostic);
    }
    if (command == "attach") {
        if (argc != 13) { usage(); return EXIT_FAILURE; }
        const auto selected = stage(argv[4]);
        const auto selectedOrigin = origin(argv[5]);
        const auto created = number(argv[6]);
        const auto now = number(argv[12]);
        const std::string_view passed(argv[10]);
        if (!selected || !selectedOrigin || !created || !now ||
            (passed != "true" && passed != "false")) { usage(); return EXIT_FAILURE; }
        CampaignStore store(argv[2]);
        AcceptanceCampaign campaign;
        auto diagnostic = store.load(campaign, *now);
        if (!diagnostic.succeeded()) return report(diagnostic);
        ChildEvidence evidence;
        evidence.evidenceId = argv[3];
        evidence.stage = *selected;
        evidence.origin = *selectedOrigin;
        evidence.createdUnixSeconds = *created;
        evidence.contentSha256 = argv[7];
        evidence.evidenceArtifactName = argv[8];
        evidence.testName = argv[9];
        evidence.rcCommitSha = campaign.identity.rcCommitSha;
        evidence.releaseArtifactSha256 = campaign.identity.releaseArtifactSha256;
        evidence.releaseRevision = campaign.identity.releaseRevision;
        evidence.architecture = campaign.identity.architecture;
        evidence.profileSha256 = campaign.identity.profileSha256;
        evidence.installStateSha256 = campaign.identity.installStateSha256;
        evidence.scenarioIdentity = campaign.identity.scenarioIdentity;
        evidence.automatedPassed = passed == "true";
        evidence.note = argv[11];
        diagnostic = attachEvidence(campaign, evidence, *now);
        if (diagnostic.succeeded()) diagnostic = store.write(campaign);
        return report(diagnostic);
    }
    if (command == "verdict") {
        if (argc != 7) { usage(); return EXIT_FAILURE; }
        const auto selected = stage(argv[3]);
        const auto now = number(argv[6]);
        const std::string_view verdictText(argv[4]);
        if (!selected || !now || (verdictText != "Pass" && verdictText != "Fail")) {
            usage(); return EXIT_FAILURE;
        }
        CampaignStore store(argv[2]);
        AcceptanceCampaign campaign;
        auto diagnostic = store.load(campaign, *now);
        if (diagnostic.succeeded()) {
            diagnostic = recordManualVerdict(
                campaign, *selected,
                verdictText == "Pass" ? HumanVerdict::Pass : HumanVerdict::Fail,
                argv[5], *now);
        }
        if (diagnostic.succeeded()) diagnostic = store.write(campaign);
        return report(diagnostic);
    }
    if (command == "recover") {
        if (argc != 4) { usage(); return EXIT_FAILURE; }
        const auto now = number(argv[3]);
        if (!now) { usage(); return EXIT_FAILURE; }
        CampaignStore store(argv[2]);
        AcceptanceCampaign campaign;
        auto diagnostic = store.load(campaign, *now);
        if (diagnostic.succeeded()) diagnostic = recoverInterrupted(campaign, *now);
        if (diagnostic.succeeded()) diagnostic = store.write(campaign);
        return report(diagnostic);
    }
    if (command == "summary") {
        if (argc != 4) { usage(); return EXIT_FAILURE; }
        const auto now = number(argv[3]);
        if (!now) { usage(); return EXIT_FAILURE; }
        AcceptanceCampaign campaign;
        const auto diagnostic = CampaignStore(argv[2]).load(campaign, *now);
        if (!diagnostic.succeeded()) return report(diagnostic);
        summary(campaign);
        return EXIT_SUCCESS;
    }
    usage();
    return EXIT_FAILURE;
}
