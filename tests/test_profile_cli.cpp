#include "hydra/profile_cli.hpp"

#include <iostream>
#include <string>

namespace {

using namespace hydra;
using namespace hydra::cli;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

profile::GameRecord game() {
    profile::GameRecord value;
    value.gameId = "game:cli";
    value.providerId = "steam";
    value.providerAppId = "123";
    value.title = L"CLI 게임 🎮";
    value.installRoot = L"C:\\Games\\Cli";
    value.executableCandidates = {L"C:\\Games\\Cli\\game.exe"};
    value.origin = profile::GameOrigin::Discovered;
    return value;
}

void testStableDocumentRenderers() {
    profile::GameRecordDocument games;
    games.games = {game()};
    std::string gameJson;
    check(renderGames(games, OutputFormat::Json, gameJson).succeeded(),
          "Game CLI JSON renderer succeeds");
    profile::GameRecordDocument decodedGames;
    check(profile::decodeGameRecordDocument(gameJson, decodedGames).succeeded() &&
              decodedGames == games,
          "Game CLI JSON round-trips through the stable Game schema");

    profile::PlayerProfileDocument players;
    players.players = {{"player-a", L"Player A", "ko-KR", {{"steam", "opaque-account-42"}}}};
    std::string playerJson;
    check(renderPlayers(players, OutputFormat::Json, playerJson).succeeded(),
          "Player CLI JSON renderer succeeds");
    check(playerJson.find("opaque-account-42") == std::string::npos &&
              playerJson.find("redacted") != std::string::npos,
          "Player JSON never exposes the provider account-reference value");
    profile::PlayerProfileDocument decodedPlayers;
    check(profile::decodePlayerProfileDocument(playerJson, decodedPlayers).succeeded(),
          "redacted Player JSON remains a valid stable Player schema document");

    profile::TwoPlayerSetup setup;
    setup.setupId = "setup-cli";
    setup.gameId = "game:cli";
    setup.displayName = L"CLI setup";
    setup.instances = {
        {{L"--one"}, L"C:\\Games\\Cli", L"C:\\Data\\One"},
        {{L"--two"}, L"C:\\Games\\Cli", L"C:\\Data\\Two"},
    };
    profile::TwoPlayerSetupDocument setups;
    setups.setups = {setup};
    std::string setupJson;
    check(renderSetups(setups, OutputFormat::Json, setupJson).succeeded(),
          "Setup CLI JSON renderer succeeds");
    profile::TwoPlayerSetupDocument decodedSetups;
    check(profile::decodeTwoPlayerSetupDocument(setupJson, decodedSetups).succeeded() &&
              decodedSetups == setups,
          "Setup CLI JSON round-trips through the stable Setup schema");
}

plan::ProviderAwareLaunchPlan compiledPlan() {
    plan::SeatProviderLaunchPlan seat;
    seat.seatId = 1u;
    seat.playerId = "player-a";
    seat.gameId = "game:cli";
    seat.setupId = "setup-cli";
    seat.instanceIndex = 0u;
    seat.requirementRevision = 7u;
    seat.hardwareFingerprint = 88u;
    seat.requirements.highRisk = true;
    seat.launchRequest.providerId = "steam";
    seat.launchRequest.gameId = "game:cli";
    seat.launchRequest.providerAppId = "123";
    seat.launchRequest.accountRef = "opaque-account-42";
    seat.launchRequest.metadataRevision = 9u;
    seat.launchRequest.targetKind = provider::LaunchTargetKind::ProviderUri;
    seat.launchRequest.target = L"steam://run/123";
    seat.launchRequest.arguments = {L"--fixture=한글"};
    seat.launchRequest.workingDirectory = L"C:\\Games\\Cli";
    seat.launchRequest.launchCorrelationId = "cli-correlation";

    plan::ProviderAwareLaunchPlan plan;
    plan.fingerprint = 123456789u;
    plan.seats = {seat};
    return plan;
}

void testPlanSnapshotRedactionAndRoundTrip() {
    PlanSnapshot snapshot;
    check(makePlanSnapshot(compiledPlan(), snapshot).succeeded(),
          "compiled plan converts to a CLI diagnostic snapshot");
    check(snapshot.seats.size() == 1u && snapshot.seats.front().accountReferenceSelected,
          "snapshot records account-reference presence only");

    std::string encoded;
    check(encodePlanSnapshot(snapshot, encoded).succeeded(),
          "plan snapshot stable envelope encodes");
    check(encoded.find("opaque-account-42") == std::string::npos,
          "stable plan snapshot never serializes the account-reference value");

    PlanSnapshot decoded;
    check(decodePlanSnapshot(encoded, decoded).succeeded() && decoded == snapshot,
          "plan snapshot stable envelope round-trips deterministically");

    std::string human;
    std::string json;
    check(renderPlan(decoded, OutputFormat::Human, human).succeeded() &&
              renderPlan(decoded, OutputFormat::Json, json).succeeded(),
          "same plan snapshot renders in human and JSON forms");
    check(human.find("<redacted-selected>") != std::string::npos &&
              human.find("opaque-account-42") == std::string::npos &&
              json.find("opaque-account-42") == std::string::npos &&
              json.find("account_reference_selected\":true") != std::string::npos,
          "human/JSON plan diagnostics apply identical account redaction semantics");
    check(json.find("한글") != std::string::npos,
          "plan JSON preserves valid Unicode diagnostic arguments");
}

void testMalformedPlanSnapshotFailsTransactionally() {
    PlanSnapshot snapshot;
    makePlanSnapshot(compiledPlan(), snapshot);
    std::string encoded;
    encodePlanSnapshot(snapshot, encoded);

    PlanSnapshot output;
    output.fingerprint = 77u;
    const auto sentinel = output;
    auto trailing = encoded + "TRAIL";
    check(decodePlanSnapshot(trailing, output).result == CliResult::ParseError &&
              output == sentinel,
          "malformed plan snapshot leaves previous output unchanged");

    auto unsupported = encoded;
    const auto version = unsupported.find("VERSION 1\n");
    if (version != std::string::npos) unsupported.replace(version, 10u, "VERSION 2\n");
    check(decodePlanSnapshot(unsupported, output).result == CliResult::UnsupportedVersion &&
              output == sentinel,
          "unsupported plan snapshot version fails closed transactionally");
}

void testHumanListsRemainDiagnosticAndRedacted() {
    profile::PlayerProfileDocument players;
    players.players = {{"player-a", L"Player A", "ko-KR", {{"steam", "opaque-account-42"}}}};
    std::string human;
    check(renderPlayers(players, OutputFormat::Human, human).succeeded() &&
              human.find("provider=steam") != std::string::npos &&
              human.find("opaque-account-42") == std::string::npos,
          "human Player listing exposes provider identity but not account-reference value");

    profile::GameRecordDocument games;
    games.games = {game()};
    check(renderGames(games, OutputFormat::Human, human).succeeded() &&
              human.find("game:cli") != std::string::npos,
          "human Game listing is sufficient for issue diagnostics");
}

} // namespace

int main() {
    testStableDocumentRenderers();
    testPlanSnapshotRedactionAndRoundTrip();
    testMalformedPlanSnapshotFailsTransactionally();
    testHumanListsRemainDiagnosticAndRedacted();
    if (failures != 0) {
        std::cerr << failures << " profile CLI test(s) failed\n";
        return 1;
    }
    std::cout << "profile CLI tests passed\n";
    return 0;
}
