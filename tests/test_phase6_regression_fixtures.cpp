#include "hydra/game_catalog.hpp"
#include "hydra/plan_preflight.hpp"
#include "hydra/provider_launch_plan.hpp"
#include "hydra/setup_package.hpp"
#include "hydra/two_player_setup_editor.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::catalog;
using namespace hydra::plan;
using namespace hydra::portable;
using namespace hydra::provider;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class FixtureProvider final : public LauncherProviderAdapter {
public:
    explicit FixtureProvider(std::string providerId, std::uint64_t revision = 5u)
        : descriptorValue{std::move(providerId), ProviderAvailability::Available, revision,
                          {true, true, true, true, true}} {}

    ProviderDescriptor descriptorValue;

    ProviderDescriptor descriptor() const noexcept override { return descriptorValue; }
    DiscoveryResponse discoverInstalledGames() noexcept override {
        return {ProviderResult::Success, descriptorValue.metadataRevision, {}, {}};
    }
    AccountReferenceResponse listAccountReferences() noexcept override {
        return {ProviderResult::Success, descriptorValue.metadataRevision, {}, {}};
    }
    LaunchResponse buildLaunchRequest(const LaunchSelection& selection) noexcept override {
        ProviderLaunchRequest request;
        request.providerId = descriptorValue.providerId;
        request.gameId = selection.gameId;
        request.providerAppId = selection.providerAppId;
        request.accountRef = selection.accountRef;
        request.metadataRevision = descriptorValue.metadataRevision;
        request.targetKind = LaunchTargetKind::ProviderUri;
        request.target = L"fixture://run/";
        if (selection.providerAppId) {
            for (const char ch : *selection.providerAppId) {
                request.target.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
            }
        }
        request.arguments = selection.instanceArguments;
        request.launchCorrelationId = descriptorValue.providerId + "-" + selection.gameId;
        return {ProviderResult::Success, std::move(request), {}};
    }
    ProcessIdentificationResponse identifyProcesses(
        const ProcessIdentificationQuery&) noexcept override {
        return {ProviderResult::Success, descriptorValue.metadataRevision, {}, {}};
    }
};

profile::CompatibilityReference compatibility(std::string id, std::uint32_t revision = 1u) {
    return {std::move(id), "phase6-regression", revision};
}

GameCatalogCandidate candidate(std::string providerId,
                               std::string appId,
                               std::wstring title,
                               std::wstring executable,
                               CatalogStaleness staleness = CatalogStaleness::Current) {
    GameCatalogCandidate value;
    value.providerId = std::move(providerId);
    value.providerAppId = std::move(appId);
    value.title = std::move(title);
    value.installRoot = L"C:\\Fixture\\Games";
    value.executableCandidates = {std::move(executable)};
    value.localVersion = L"1.0";
    value.compatibility = compatibility("compat-main");
    value.origin = profile::GameOrigin::Discovered;
    value.staleness = staleness;
    return value;
}

profile::SeatConfigDocument seatDocument() {
    profile::SeatConfigDocument document;
    document.managementSeatId = 1u;
    profile::PersistedSeatConfig first;
    first.seatId = 1u;
    first.name = L"Seat 1";
    first.displayIds = {L"display-1"};
    first.primaryDisplayId = L"display-1";
    first.keyboardIds = {L"keyboard-1"};
    first.mouseIds = {L"mouse-1"};
    first.controllerIds = {L"controller-1"};
    first.audioOutputEndpointId = L"audio-1";
    profile::PersistedSeatConfig second;
    second.seatId = 2u;
    second.name = L"Seat 2";
    second.displayIds = {L"display-2"};
    second.primaryDisplayId = L"display-2";
    second.keyboardIds = {L"keyboard-2"};
    second.mouseIds = {L"mouse-2"};
    second.controllerIds = {L"controller-2"};
    second.audioOutputEndpointId = L"audio-2";
    document.seats = {first, second};
    return document;
}

profile::PlayerProfileDocument playerDocument() {
    profile::PlayerProfileDocument document;
    document.players = {
        {"alice", L"Alice", "ko-KR", {{"provider-a", "local-a"}}},
        {"bob", L"Bob", "en-US", {{"provider-b", "local-b"}}},
    };
    return document;
}

GameRuntimeRequirement requirement(const profile::GameRecord& game,
                                   bool protectedPath = false,
                                   bool approved = false) {
    GameRuntimeRequirement value;
    value.gameId = game.gameId;
    value.revision = 9u;
    value.validatedSeatCount = 1u;
    value.requirements.display = true;
    value.requirements.keyboard = true;
    value.requirements.mouse = true;
    value.requirements.audioOutput = true;
    value.requirements.highRisk = protectedPath;
    value.capabilities = {};
    value.highRiskApproved = approved;
    value.compatibility = game.compatibility;
    return value;
}

void testCatalogCorpusIsDeterministicAcrossProvidersAndMoves() {
    std::vector<GameCatalogCandidate> source{
        candidate("provider-a", "100", L"게임 Alpha 🎮", L"C:\\Fixture\\Games\\Alpha\\game.exe"),
        candidate("provider-b", "200", L"Beta", L"D:\\Moved\\Beta\\beta.exe"),
        candidate("provider-a", "100", L"게임 Alpha 🎮", L"C:/Fixture/Games/Alpha/./game.exe"),
        candidate("provider-b", "300", L"Removed", L"D:\\Old\\removed.exe",
                  CatalogStaleness::Stale),
    };
    LocalGameCatalog first;
    check(buildLocalGameCatalog(source, first).succeeded(),
          "multi-provider/Unicode/moved/stale fixture catalog builds");
    std::reverse(source.begin(), source.end());
    LocalGameCatalog second;
    check(buildLocalGameCatalog(source, second).succeeded() && second == first,
          "catalog fixture result is independent of candidate input order");
    check(first.entries.size() == 3u,
          "duplicate provider identity reconciles while distinct stale/uninstalled fixture remains explicit");

    bool unicode = false;
    bool stale = false;
    for (const auto& entry : first.entries) {
        unicode = unicode || entry.game.title == L"게임 Alpha 🎮";
        stale = stale || entry.staleness == CatalogStaleness::Stale;
    }
    check(unicode && stale,
          "Unicode presentation and stale provider state survive regression reconciliation");
}

void testPlayerSeatMovesAndProviderFailuresRemainExplicit() {
    FixtureProvider providerA("provider-a");
    FixtureProvider providerB("provider-b");
    const std::vector<ProviderAdapterBinding> providers{
        {"provider-a", &providerA}, {"provider-b", &providerB}};

    auto games = profile::GameRecordDocument{};
    auto firstGame = candidate("provider-a", "100", L"Alpha", L"C:\\Alpha\\game.exe");
    auto secondGame = candidate("provider-b", "200", L"Beta", L"C:\\Beta\\game.exe");
    LocalGameCatalog catalog;
    std::vector<GameCatalogCandidate> candidates{firstGame, secondGame};
    check(buildLocalGameCatalog(candidates, catalog).succeeded() && catalog.entries.size() == 2u,
          "two-provider plan fixture catalog builds");
    for (const auto& entry : catalog.entries) games.games.push_back(entry.game);

    const auto gameA = std::find_if(games.games.begin(), games.games.end(), [](const auto& game) {
        return game.providerId == "provider-a";
    });
    const auto gameB = std::find_if(games.games.begin(), games.games.end(), [](const auto& game) {
        return game.providerId == "provider-b";
    });
    check(gameA != games.games.end() && gameB != games.games.end(),
          "both provider game records are discoverable in fixture");
    if (gameA == games.games.end() || gameB == games.games.end()) return;

    auto players = playerDocument();
    const profile::TwoPlayerSetupDocument setups;
    const std::vector<GameRuntimeRequirement> requirements{
        requirement(*gameA), requirement(*gameB)};

    profile::RuntimeSessionSelection initial;
    initial.bindings = {
        {1u, "alice", gameA->gameId, std::nullopt, 0u},
        {2u, "bob", gameB->gameId, std::nullopt, 0u},
    };
    const auto firstPlan = compileProviderAwareLaunchPlan(
        seatDocument(), players, games, setups, initial, providers, requirements);
    check(firstPlan.succeeded(), "baseline two-provider plan compiles");

    profile::RuntimeSessionSelection moved;
    moved.bindings = {
        {1u, "bob", gameB->gameId, std::nullopt, 0u},
        {2u, "alice", gameA->gameId, std::nullopt, 0u},
    };
    const auto movedPlan = compileProviderAwareLaunchPlan(
        seatDocument(), players, games, setups, moved, providers, requirements);
    check(movedPlan.succeeded() && movedPlan.plan->fingerprint != firstPlan.plan->fingerprint,
          "Player/Game move between Seats is explicit material plan state");

    providerB.descriptorValue.availability = ProviderAvailability::Offline;
    const auto offline = compileProviderAwareLaunchPlan(
        seatDocument(), players, games, setups, initial, providers, requirements);
    check(!offline.succeeded() &&
              offline.issues.front().code == PlanIssueCode::ProviderUnavailable,
          "offline provider fixture fails closed without fallback");

    const std::vector<ProviderAdapterBinding> missingProvider{{"provider-a", &providerA}};
    const auto missing = compileProviderAwareLaunchPlan(
        seatDocument(), players, games, setups, initial, missingProvider, requirements);
    check(!missing.succeeded() && missing.issues.front().code == PlanIssueCode::MissingProvider,
          "missing provider fixture fails closed without provider substitution");
}

void testSameGameAutomaticManualPortableRegression() {
    const auto catalogCandidate = candidate(
        "provider-a", "500", L"Co-op 한글", L"C:\\Games\\Coop\\coop.exe");
    LocalGameCatalog catalog;
    const std::vector<GameCatalogCandidate> candidates{catalogCandidate};
    check(buildLocalGameCatalog(candidates, catalog).succeeded() && catalog.entries.size() == 1u,
          "same-game catalog fixture builds");
    if (catalog.entries.empty()) return;
    const auto& game = catalog.entries.front().game;

    std::array<profile::InstanceRecipe, 2> recipes{
        profile::InstanceRecipe{{L"--instance=1"}, L"C:\\Games\\Coop",
                                L"C:\\FixtureUser\\Coop1"},
        profile::InstanceRecipe{{L"--instance=2"}, L"C:\\Games\\Coop",
                                L"C:\\FixtureUser\\Coop2"},
    };
    setup::GeneratedSetupCandidate automatic;
    check(setup::generateCandidate({&game, "setup-reg", L"Regression", recipes}, automatic)
              .succeeded(),
          "automatic same-game setup fixture validates");

    setup::SetupEditor manual(automatic.setup);
    manual.setDisplayName(L"Regression");
    check(manual.save(game).succeeded() && manual.committed() == automatic.setup,
          "manual same-game setup path stays contract-equivalent to automatic path");

    SetupPackage package;
    check(exportSetup(automatic.setup, game,
                      {"phase6-corpus", 1u, "regression-suite"}, package).succeeded(),
          "same-game setup exports to portable fixture");
    std::string encoded;
    check(encodePackage(package, encoded).succeeded(), "portable fixture encodes");
    SetupPackage decoded;
    check(decodePackage(encoded, decoded).succeeded() && decoded == package,
          "portable fixture decode is deterministic");

    const std::vector<PathBinding> bindings{
        {"WORKING_DIRECTORY_0", L"D:\\Games\\Coop"},
        {"DATA_ROOT_0", L"D:\\HydraSeat\\Coop1"},
        {"WORKING_DIRECTORY_1", L"D:\\Games\\Coop"},
        {"DATA_ROOT_1", L"D:\\HydraSeat\\Coop2"},
    };
    profile::TwoPlayerSetup imported;
    check(importSetup(decoded, game, bindings, imported).succeeded(),
          "portable same-game fixture imports after explicit machine remap");
    check(imported.instances[0].arguments == automatic.setup.instances[0].arguments &&
              imported.instances[1].arguments == automatic.setup.instances[1].arguments,
          "portable remap changes local paths without changing launch arguments");

    auto malformed = encoded + "TRAILING";
    SetupPackage sentinel;
    sentinel.provenance.sourceId = "sentinel";
    const auto before = sentinel;
    check(decodePackage(malformed, sentinel).result == PackageResult::InvalidPackage &&
              sentinel == before,
          "malformed imported package remains transactionally rejected in corpus");
}

void testProtectionRequirementRequiresExplicitApproval() {
    FixtureProvider providerA("provider-a");
    const std::vector<ProviderAdapterBinding> providers{{"provider-a", &providerA}};
    LocalGameCatalog catalog;
    const std::vector<GameCatalogCandidate> candidates{
        candidate("provider-a", "900", L"Protected Fixture", L"C:\\Protected\\game.exe")};
    buildLocalGameCatalog(candidates, catalog);
    if (catalog.entries.empty()) {
        check(false, "protected fixture catalog should build");
        return;
    }
    profile::GameRecordDocument games;
    games.games = {catalog.entries.front().game};
    profile::PlayerProfileDocument players;
    players.players = {{"alice", L"Alice", "ko-KR", {{"provider-a", "local-a"}}}};
    profile::RuntimeSessionSelection selection;
    selection.bindings = {{1u, "alice", games.games.front().gameId, std::nullopt, 0u}};
    const profile::TwoPlayerSetupDocument setups;

    auto protectedRequirement = requirement(games.games.front(), true, false);
    std::vector<GameRuntimeRequirement> requirements{protectedRequirement};
    const auto blocked = compileProviderAwareLaunchPlan(
        seatDocument(), players, games, setups, selection, providers, requirements);
    check(!blocked.succeeded() &&
              blocked.issues.front().code == PlanIssueCode::HighRiskApprovalRequired,
          "Protected / Experimental fixture requires explicit approval");
    const auto blockedSummary = preflight::buildSummary(blocked);
    check(!blockedSummary.canActivate && !blockedSummary.messages.empty(),
          "Protected path produces blocking normal-user preflight evidence");

    requirements.front().highRiskApproved = true;
    const auto approved = compileProviderAwareLaunchPlan(
        seatDocument(), players, games, setups, selection, providers, requirements);
    check(approved.succeeded(), "explicit protected-path approval allows plan compilation");
    const auto approvedSummary = preflight::buildSummary(approved);
    bool warned = false;
    for (const auto& message : approvedSummary.messages) {
        warned = warned || message.code == "risk.protected";
    }
    check(approvedSummary.canActivate && warned,
          "approved Protected path remains visibly warned rather than silently normalized");
}

} // namespace

int main() {
    testCatalogCorpusIsDeterministicAcrossProvidersAndMoves();
    testPlayerSeatMovesAndProviderFailuresRemainExplicit();
    testSameGameAutomaticManualPortableRegression();
    testProtectionRequirementRequiresExplicitApproval();
    if (failures != 0) {
        std::cerr << failures << " Phase 6 regression fixture test(s) failed\n";
        return 1;
    }
    std::cout << "Phase 6 regression fixture tests passed\n";
    return 0;
}
