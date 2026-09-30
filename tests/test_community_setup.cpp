#include "hydra/community_setup.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::community;
using namespace hydra::provider;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class FakeProvider final : public LauncherProviderAdapter {
public:
    ProviderDescriptor descriptor() const noexcept override {
        return {"fake", ProviderAvailability::Available, 7u,
                {true, false, true, true, true}};
    }
    DiscoveryResponse discoverInstalledGames() noexcept override {
        return {ProviderResult::Success, 7u, {}, {}};
    }
    AccountReferenceResponse listAccountReferences() noexcept override {
        return {ProviderResult::UnsupportedOperation, 7u, {}, "not needed"};
    }
    LaunchResponse buildLaunchRequest(const LaunchSelection& selection) noexcept override {
        ProviderLaunchRequest request;
        request.providerId = "fake";
        request.gameId = selection.gameId;
        request.providerAppId = selection.providerAppId;
        request.metadataRevision = 7u;
        request.targetKind = LaunchTargetKind::ProviderUri;
        request.target = L"fake://run/community";
        request.arguments = selection.instanceArguments;
        request.launchCorrelationId = "community-launch";
        return {ProviderResult::Success, std::move(request), {}};
    }
    ProcessIdentificationResponse identifyProcesses(
        const ProcessIdentificationQuery&) noexcept override {
        return {ProviderResult::Success, 7u, {}, {}};
    }
};

profile::CompatibilityReference compatibility() {
    return {"compat-community", "local-evidence", 4u};
}

profile::GameRecord localGame() {
    profile::GameRecord value;
    value.gameId = "game:community";
    value.providerId = "fake";
    value.providerAppId = "500";
    value.title = L"Community Fixture";
    value.installRoot = L"D:\\Games\\Community";
    value.executableCandidates = {L"D:\\Games\\Community\\game.exe"};
    value.localVersion = L"1.0";
    value.compatibility = compatibility();
    value.origin = profile::GameOrigin::Discovered;
    return value;
}

profile::TwoPlayerSetup sourceSetup() {
    profile::TwoPlayerSetup value;
    value.setupId = "setup-community";
    value.gameId = "game:community";
    value.displayName = L"Community two player";
    value.compatibility = compatibility();
    value.instances = {
        {{L"--seat=1", L"--profile", L"alpha"},
         L"C:\\Source\\Game1", L"C:\\Source\\Data1"},
        {{L"--seat=2", L"--profile", L"beta"},
         L"C:\\Source\\Game2", L"C:\\Source\\Data2"},
    };
    return value;
}

std::vector<portable::PortableInstanceMaterialization> materializations() {
    return {
        {0u,
         {{"community-pre", setup::RecipeExecutionPhase::PreSpawn,
           setup::MutationScope::SeatWritableInstance,
           {{L"defaults/pre.ini", L"config/seat1.ini", 4096u}}},
          {"community-startup", setup::RecipeExecutionPhase::Startup,
           setup::MutationScope::SeatWritableInstance,
           {{L"defaults/startup.ini", L"state/startup.ini", 4096u}}}}},
        {1u,
         {{"community-window", setup::RecipeExecutionPhase::PostWindow,
           setup::MutationScope::SeatWritableInstance,
           {{L"defaults/window.ini", L"config/seat2.ini", 4096u}}},
          {"community-runtime", setup::RecipeExecutionPhase::Runtime,
           setup::MutationScope::SeatWritableInstance,
           {{L"defaults/runtime.ini", L"state/runtime.ini", 4096u}}}}},
    };
}

CommunitySetupEntry entry(bool protectedExperimental = false) {
    CommunitySetupEntry value;
    value.entryId = "community-setup-a";
    value.packageId = "community-seed";
    value.packageRevision = 3u;
    value.selector.gameId = "game:community";
    value.selector.providerId = "fake";
    value.selector.providerAppId = "500";
    value.selector.gameVersion = "1.0";
    value.protectedExperimental = protectedExperimental;
    value.knownLimitations = {"Requires two separate local data directories."};
    value.evidenceResultIds = {"result-community-a"};
    value.sourceId = "community-reviewed";
    value.licenseId = "CC0-1.0";
    value.authorAttribution = "Fixture Contributor";
    const auto exported = portable::exportSetup(
        sourceSetup(), localGame(), {value.sourceId, value.packageRevision, "fixture-exporter"},
        value.setupPackage);
    check(exported.succeeded(), "community setup fixture exports through the P6 portable boundary");
    return value;
}

CommunitySetupEntry entryWithMaterialization() {
    auto value = entry();
    const auto descriptors = materializations();
    const auto exported = portable::exportSetup(
        sourceSetup(), localGame(), {value.sourceId, value.packageRevision, "fixture-exporter"},
        descriptors, value.setupPackage);
    check(exported.succeeded(),
          "community semantic fixture exports typed materialization descriptors");
    return value;
}

std::vector<portable::PathBinding> localBindings() {
    return {
        {"WORKING_DIRECTORY_0", L"D:\\Games\\Community"},
        {"DATA_ROOT_0", L"D:\\HydraSeat\\Community1"},
        {"WORKING_DIRECTORY_1", L"D:\\Games\\Community"},
        {"DATA_ROOT_1", L"D:\\HydraSeat\\Community2"},
    };
}

profile::SeatConfigDocument seats() {
    profile::SeatConfigDocument document;
    document.managementSeatId = 1u;
    profile::PersistedSeatConfig first;
    first.seatId = 1u;
    first.name = L"Seat 1";
    first.displayIds = {L"display-1"};
    first.primaryDisplayId = L"display-1";
    first.keyboardIds = {L"keyboard-1"};
    first.mouseIds = {L"mouse-1"};
    first.audioOutputEndpointId = L"audio-1";
    profile::PersistedSeatConfig second;
    second.seatId = 2u;
    second.name = L"Seat 2";
    second.displayIds = {L"display-2"};
    second.primaryDisplayId = L"display-2";
    second.keyboardIds = {L"keyboard-2"};
    second.mouseIds = {L"mouse-2"};
    second.audioOutputEndpointId = L"audio-2";
    document.seats = {first, second};
    return document;
}

profile::PlayerProfileDocument players() {
    profile::PlayerProfileDocument document;
    document.players = {
        {"player-1", L"Player 1", "en-US", {}},
        {"player-2", L"Player 2", "en-US", {}},
    };
    return document;
}

plan::GameRuntimeRequirement requirement(bool protectedExperimental, bool approved) {
    plan::GameRuntimeRequirement value;
    value.gameId = "game:community";
    value.revision = 9u;
    value.validatedSeatCount = 2u;
    value.requirements.display = true;
    value.requirements.keyboard = true;
    value.requirements.mouse = true;
    value.requirements.audioOutput = true;
    value.requirements.windowOwnership = true;
    value.requirements.recovery = true;
    value.requirements.highRisk = protectedExperimental;
    value.capabilities = {};
    value.highRiskApproved = approved;
    value.compatibility = compatibility();
    return value;
}

plan::PlanCompileResult compileImported(const profile::TwoPlayerSetup& imported,
                                        bool protectedExperimental,
                                        bool approved,
                                        bool provideLocalRequirement = true) {
    FakeProvider fake;
    const std::vector<plan::ProviderAdapterBinding> providers{{"fake", &fake}};
    profile::GameRecordDocument games;
    games.games = {localGame()};
    profile::TwoPlayerSetupDocument setups;
    setups.setups = {imported};
    const auto selection = setup::makeRuntimeSelection(
        imported, 1u, "player-1", 2u, "player-2");
    std::vector<plan::GameRuntimeRequirement> requirements;
    if (provideLocalRequirement) {
        requirements.push_back(requirement(protectedExperimental, approved));
    }
    return plan::compileProviderAwareLaunchPlan(
        seats(), players(), games, setups, selection, providers, requirements);
}

void testValidatedCommunitySetupImportsAndCompilesLocally() {
    const auto communityEntry = entry();
    check(validateCommunitySetupEntry(communityEntry).succeeded(),
          "well-formed community setup entry validates as data-only metadata");

    profile::TwoPlayerSetup imported;
    check(importCommunitySetup(communityEntry, localGame(), localBindings(), imported).succeeded(),
          "community setup imports only after explicit local path remapping");
    check(imported.instances[0].dataRoot == L"D:\\HydraSeat\\Community1" &&
              imported.instances[1].dataRoot == L"D:\\HydraSeat\\Community2",
          "community source paths are replaced by local approved paths");
    check(compileImported(imported, false, false).succeeded(),
          "imported community setup compiles through the exact local P6 plan/preflight contract");
}

void testTypedCommunityImportPreservesSemanticsWithoutGrantingAuthority() {
    const auto communityEntry = entryWithMaterialization();
    check(validateCommunitySetupEntry(communityEntry).succeeded(),
          "community entry accepts only validated data-only v2 materialization descriptors");

    portable::ImportedSetup imported;
    check(importCommunitySetup(communityEntry, localGame(), localBindings(), imported).succeeded(),
          "typed community import preserves semantic-bearing setup package");
    check(imported.setup.instances[0].arguments == sourceSetup().instances[0].arguments &&
              imported.setup.instances[1].arguments == sourceSetup().instances[1].arguments &&
              imported.setup.compatibility == compatibility(),
          "community typed import preserves both argument arrays and compatibility reference");
    check(imported.instanceMaterializations == materializations(),
          "community typed import preserves PreSpawn/Startup/PostWindow/Runtime descriptors exactly");

    profile::TwoPlayerSetup legacyOutput;
    legacyOutput.setupId = "sentinel";
    const auto sentinel = legacyOutput;
    check(importCommunitySetup(communityEntry, localGame(), localBindings(), legacyOutput).code ==
              CommunitySetupCode::LocalImportFailed &&
              legacyOutput == sentinel,
          "setup-only community import refuses to silently discard v2 materialization semantics");

    const auto noAuthority = compileImported(imported.setup, false, false, false);
    check(!noAuthority.succeeded() &&
              std::any_of(noAuthority.issues.begin(), noAuthority.issues.end(),
                          [](const plan::PlanIssue& issue) {
                              return issue.code == plan::PlanIssueCode::MissingRequirement;
                          }),
          "community import alone cannot manufacture trusted local runtime requirement evidence");

    const auto locallyAuthorized = compileImported(imported.setup, false, false, true);
    check(locallyAuthorized.succeeded() && locallyAuthorized.plan.has_value(),
          "same imported declarative setup compiles only after local requirement authority is supplied");
    if (locallyAuthorized.plan) {
        check(std::all_of(locallyAuthorized.plan->seats.begin(), locallyAuthorized.plan->seats.end(),
                          [](const plan::SeatProviderLaunchPlan& seat) {
                              return seat.requirementRevision == 9u &&
                                     seat.launchRequest.metadataRevision == 7u;
                          }),
              "community package/evidence revision never replaces local requirement/provider revisions");
    }
}

void testCommunityPopularityCannotOverrideLocalProtectionGate() {
    const auto protectedEntry = entry(true);
    profile::TwoPlayerSetup imported;
    check(importCommunitySetup(protectedEntry, localGame(), localBindings(), imported).succeeded(),
          "Protected marker does not itself grant or deny data import");
    const auto blocked = compileImported(imported, true, false);
    check(!blocked.succeeded() && !blocked.issues.empty() &&
              blocked.issues.front().code == plan::PlanIssueCode::HighRiskApprovalRequired,
          "community setup still requires local explicit Protected/Experimental approval");
    check(compileImported(imported, true, true).succeeded(),
          "local explicit approval can admit the same typed setup without community bypass semantics");
}

void testMaliciousInstructionAndExternalResourceTextRejected() {
    auto malicious = entry();
    malicious.knownLimitations = {"Run powershell to disable anti-cheat before launch"};
    check(validateCommunitySetupEntry(malicious).code == CommunitySetupCode::InvalidText,
          "community limitation cannot carry executable/protection-bypass instructions");

    malicious = entry();
    malicious.knownLimitations = {"Download helper from https://example.invalid/helper.exe"};
    check(validateCommunitySetupEntry(malicious).code == CommunitySetupCode::InvalidText,
          "community setup cannot turn external URLs into implicit helper acquisition");

    malicious = entry();
    malicious.authorAttribution = "https://example.invalid/profile";
    check(validateCommunitySetupEntry(malicious).code == CommunitySetupCode::InvalidProvenance,
          "author attribution remains passive bounded text, not an active external resource reference");

    malicious = entryWithMaterialization();
    malicious.setupPackage.instanceMaterializations[0].steps[0].scope =
        setup::MutationScope::SharedInstallation;
    check(validateCommunitySetupEntry(malicious).code == CommunitySetupCode::InvalidPortableSetup,
          "community setup cannot upgrade declarative materialization into shared-install mutation authority");
}

void testSelectorProvenanceAndRemapFailuresAreTransactional() {
    profile::TwoPlayerSetup output;
    output.setupId = "sentinel";
    const auto sentinel = output;

    auto mismatch = entry();
    mismatch.selector.gameId = "game:other";
    check(validateCommunitySetupEntry(mismatch).code == CommunitySetupCode::SelectorMismatch &&
              output == sentinel,
          "selector/payload Game mismatch fails before local import");

    mismatch = entry();
    mismatch.setupPackage.provenance.sourceRevision = 99u;
    check(validateCommunitySetupEntry(mismatch).code == CommunitySetupCode::InvalidProvenance,
          "portable payload must retain exact package provenance revision");

    const auto valid = entry();
    auto bindings = localBindings();
    bindings.pop_back();
    check(importCommunitySetup(valid, localGame(), bindings, output).code ==
              CommunitySetupCode::LocalImportFailed &&
              output == sentinel,
          "missing local remap leaves prior local setup state unchanged");

    auto wrongGame = localGame();
    wrongGame.providerAppId = "999";
    check(importCommunitySetup(valid, wrongGame, localBindings(), output).code ==
              CommunitySetupCode::LocalGameMismatch &&
              output == sentinel,
          "community setup does not silently retarget a different local provider/app identity");
}

} // namespace

int main() {
    testValidatedCommunitySetupImportsAndCompilesLocally();
    testTypedCommunityImportPreservesSemanticsWithoutGrantingAuthority();
    testCommunityPopularityCannotOverrideLocalProtectionGate();
    testMaliciousInstructionAndExternalResourceTextRejected();
    testSelectorProvenanceAndRemapFailuresAreTransactional();
    if (failures != 0) {
        std::cerr << failures << " community setup test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Community setup validator tests passed.\n";
    return EXIT_SUCCESS;
}
