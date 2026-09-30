#include "hydra/profile_schema.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace hydra;
using namespace hydra::profile;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

PersistedSeatConfig seat(SeatId id) {
    PersistedSeatConfig value;
    value.seatId = id;
    value.name = id == 1u ? L"Seat 1 \uD50C\uB808\uC774\uC5B4" : L"Seat 2 \U0001F47E";
    value.displayIds = {L"DISPLAY-" + std::to_wstring(id)};
    value.primaryDisplayId = value.displayIds.front();
    value.keyboardIds = {L"HID\\KEYBOARD-" + std::to_wstring(id)};
    value.mouseIds = {L"HID\\MOUSE-" + std::to_wstring(id)};
    value.controllerIds = {L"DINPUT:{00000000-0000-0000-0000-00000000000" +
                           std::to_wstring(id) + L"}"};
    value.audioOutputEndpointId = L"{0.0.0.00000000}.AUDIO-" + std::to_wstring(id);
    value.active = true;
    return value;
}

SeatConfigDocument seats() {
    SeatConfigDocument document;
    document.managementSeatId = 1u;
    document.seats = {seat(1u), seat(2u)};
    return document;
}

PlayerProfileDocument players() {
    PlayerProfile first;
    first.playerId = "player.one";
    first.displayName = L"\uB9C8\uB9AC\uC624 \U0001F3AE";
    first.preferredLocale = "ko-KR";
    first.providerAccounts = {{"steam", "local-account-1"}};

    PlayerProfile second;
    second.playerId = "player.two";
    second.displayName = L"\uB8E8\uC774\uC9C0";
    second.preferredLocale = "en-US";
    second.providerAccounts = {{"steam", "local-account-2"}};

    PlayerProfileDocument document;
    document.players = {first, second};
    return document;
}

GameRecord game(std::string id, std::wstring title) {
    GameRecord value;
    value.gameId = std::move(id);
    value.providerId = "steam";
    value.providerAppId = "123456";
    value.title = std::move(title);
    value.installRoot = L"C:\\Games\\Hydra Test";
    value.executableCandidates = {L"C:\\Games\\Hydra Test\\game.exe"};
    value.localVersion = L"1.2.3-\uD14C\uC2A4\uD2B8";
    value.executableSha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    value.compatibility = CompatibilityReference{"compat.game.shared", "local", 3u};
    value.origin = GameOrigin::Discovered;
    return value;
}

GameRecordDocument games() {
    GameRecordDocument document;
    document.games = {game("game.shared", L"\uD14C\uC2A4\uD2B8 \U0001F680")};
    return document;
}

TwoPlayerSetupDocument setups() {
    TwoPlayerSetup setup;
    setup.setupId = "setup.shared.two";
    setup.gameId = "game.shared";
    setup.displayName = L"\uB450 \uBA85 \uC124\uC815";
    setup.compatibility = CompatibilityReference{"compat.setup.shared", "local", 2u};
    InstanceRecipe first;
    first.arguments = {L"--player", L"1", L"--data", L"C:\\HydraData\\P1"};
    first.workingDirectory = L"C:\\Games\\Hydra Test";
    first.dataRoot = L"C:\\HydraData\\P1";
    InstanceRecipe second;
    second.arguments = {L"--player", L"2", L"--data", L"C:\\HydraData\\P2"};
    second.workingDirectory = L"C:\\Games\\Hydra Test";
    second.dataRoot = L"C:\\HydraData\\P2";
    setup.instances = {first, second};

    TwoPlayerSetupDocument document;
    document.setups = {setup};
    return document;
}

RuntimeSessionSelection runtimeSelection() {
    RuntimeSessionSelection selection;
    selection.bindings = {
        {1u, "player.one", "game.shared", std::string("setup.shared.two"), 0u},
        {2u, "player.two", "game.shared", std::string("setup.shared.two"), 1u}};
    return selection;
}

void testRoundTripsAndConceptIsolation() {
    SchemaDiagnostic diagnostic;

    const auto seatInput = seats();
    const auto seatJson = encodeSeatConfigDocument(seatInput, &diagnostic);
    SeatConfigDocument seatOutput;
    check(diagnostic.succeeded() && !seatJson.empty() &&
              decodeSeatConfigDocument(seatJson, seatOutput).succeeded() &&
              seatOutput == seatInput,
          "SeatConfigDocument round-trips Unicode hardware-only Seat data");
    check(seatJson.find("target_hwnd") == std::string::npos &&
              seatJson.find("process_id") == std::string::npos &&
              seatJson.find("handle") == std::string::npos,
          "persisted Seat JSON has no runtime PID/HWND/handle representation");
    check(encodeSeatConfigDocument(seatInput) == seatJson,
          "SeatConfigDocument encoding is deterministic");

    const auto playerInput = players();
    const auto playerJson = encodePlayerProfileDocument(playerInput, &diagnostic);
    PlayerProfileDocument playerOutput;
    check(diagnostic.succeeded() &&
              decodePlayerProfileDocument(playerJson, playerOutput).succeeded() &&
              playerOutput == playerInput,
          "PlayerProfileDocument round-trips Unicode and opaque account references");
    check(playerJson.find("password") == std::string::npos &&
              playerJson.find("token") == std::string::npos &&
              playerJson.find("cookie") == std::string::npos,
          "Player schema has no provider credential fields");

    const auto gameInput = games();
    const auto gameJson = encodeGameRecordDocument(gameInput, &diagnostic);
    GameRecordDocument gameOutput;
    check(diagnostic.succeeded() &&
              decodeGameRecordDocument(gameJson, gameOutput).succeeded() &&
              gameOutput == gameInput,
          "GameRecordDocument round-trips provider/install metadata independently");

    const auto setupInput = setups();
    const auto setupJson = encodeTwoPlayerSetupDocument(setupInput, &diagnostic);
    TwoPlayerSetupDocument setupOutput;
    check(diagnostic.succeeded() &&
              decodeTwoPlayerSetupDocument(setupJson, setupOutput).succeeded() &&
              setupOutput == setupInput,
          "TwoPlayerSetupDocument round-trips exactly two typed instance recipes");
    check(setupJson.find("script") == std::string::npos &&
              setupJson.find("shell") == std::string::npos,
          "TwoPlayerSetup has no arbitrary script or shell field");

    const auto runtimeInput = runtimeSelection();
    const auto runtimeJson = encodeRuntimeSessionSelection(runtimeInput, &diagnostic);
    RuntimeSessionSelection runtimeOutput;
    check(diagnostic.succeeded() &&
              decodeRuntimeSessionSelection(runtimeJson, runtimeOutput).succeeded() &&
              runtimeOutput == runtimeInput,
          "temporary RuntimeSessionSelection round-trips Seat+Player+Game bindings");
    check(runtimeJson.find("pid") == std::string::npos &&
              runtimeJson.find("hwnd") == std::string::npos &&
              runtimeJson.find("handle") == std::string::npos,
          "runtime selection serializes logical references rather than OS object identity");
    check(validateRuntimeSessionSelection(runtimeInput, seatInput, playerInput,
                                          gameInput, setupInput).succeeded(),
          "two different Players can bind the same Game through separate setup instances");
}

void testRuntimeSeatConversionContainsOnlyStableSeatState() {
    SeatConfig runtime;
    runtime.seatId = 1u;
    runtime.name = L"Seat 1";
    runtime.displayIds = {L"DISPLAY-1"};
    runtime.primaryDisplayId = runtime.displayIds.front();

    PersistedSeatConfig persisted;
    check(makePersistedSeatConfig(runtime, persisted).succeeded(),
          "runtime Seat converts to stable persisted Seat state");
    const auto restored = makeRuntimeSeatConfig(persisted);
    check(restored.seatId == runtime.seatId &&
              restored.displayIds == runtime.displayIds,
          "persisted Seat restores stable hardware state without runtime window identity");
}

void testUnknownSensitiveAndRuntimeFieldsFailClosed() {
    SeatConfigDocument seatOutput;
    const std::string seatWithHwnd =
        "{\"schema_version\":1,\"management_seat_id\":1,\"seats\":[{"
        "\"seat_id\":1,\"name\":\"Seat 1\",\"active\":true,"
        "\"display_ids\":[],\"primary_display_id\":null,\"keyboard_ids\":[],"
        "\"mouse_ids\":[],\"controller_ids\":[],\"audio_output_endpoint_id\":null,"
        "\"audio_input_endpoint_id\":null,\"target_hwnd\":123}]}";
    check(decodeSeatConfigDocument(seatWithHwnd, seatOutput).result ==
              SchemaResult::UnknownField,
          "new persisted Seat schema rejects legacy target_hwnd field");

    for (const std::string_view secret : {"password", "token", "cookie", "refresh_token"}) {
        const std::string json =
            "{\"schema_version\":1,\"players\":[{\"player_id\":\"p1\","
            "\"display_name\":\"P1\",\"preferred_locale\":\"en-US\","
            "\"provider_accounts\":[{\"provider_id\":\"steam\","
            "\"account_ref\":\"local-1\",\"" + std::string(secret) +
            "\":\"secret\"}]}]}";
        PlayerProfileDocument output;
        check(decodePlayerProfileDocument(json, output).result == SchemaResult::UnknownField,
              "provider credential-like unknown field is rejected instead of retained");
    }

    const std::string scriptSetup =
        "{\"schema_version\":1,\"setups\":[{\"setup_id\":\"s1\","
        "\"game_id\":\"g1\",\"display_name\":\"S\",\"compatibility\":null,"
        "\"instances\":["
        "{\"arguments\":[],\"working_directory\":null,\"data_root\":null,"
        "\"script\":\"cmd.exe /c anything\"},"
        "{\"arguments\":[],\"working_directory\":null,\"data_root\":null}]}]}";
    TwoPlayerSetupDocument setupOutput;
    check(decodeTwoPlayerSetupDocument(scriptSetup, setupOutput).result ==
              SchemaResult::UnknownField,
          "TwoPlayerSetup decoder rejects arbitrary script field");
}

void testParserAndVersionFailuresAreTransactional() {
    PlayerProfileDocument original = players();
    PlayerProfileDocument output = original;
    const auto malformed = decodePlayerProfileDocument("{not-json", output);
    check(malformed.result == SchemaResult::ParseError && output == original,
          "malformed JSON leaves destination document unchanged");

    const auto duplicate = decodePlayerProfileDocument(
        "{\"schema_version\":1,\"schema_version\":1,\"players\":[]}", output);
    check(duplicate.result == SchemaResult::ParseError && output == original,
          "duplicate JSON object keys fail closed transactionally");

    const auto future = decodePlayerProfileDocument(
        "{\"schema_version\":2,\"players\":[]}", output);
    check(future.result == SchemaResult::UnsupportedVersion && output == original,
          "unknown future schema version fails closed without replacing valid state");

    std::string invalidUtf8 =
        "{\"schema_version\":1,\"players\":[{\"player_id\":\"p1\","
        "\"display_name\":\"";
    invalidUtf8.push_back(static_cast<char>(0xc0));
    invalidUtf8.push_back(static_cast<char>(0xaf));
    invalidUtf8 +=
        "\",\"preferred_locale\":\"en\",\"provider_accounts\":[]}]}";
    check(decodePlayerProfileDocument(invalidUtf8, output).result ==
              SchemaResult::InvalidValue,
          "overlong invalid UTF-8 sequence is rejected");
}

void testBounds() {
    auto seatDocument = seats();
    seatDocument.seats.push_back(seat(3u));
    check(validateSeatConfigDocument(seatDocument).result == SchemaResult::BoundsExceeded,
          "v1 persisted Seat schema rejects a third Seat");

    auto playerDocument = players();
    playerDocument.players[0].playerId.assign(kMaximumIdentifierBytes, 'a');
    check(validatePlayerProfileDocument(playerDocument).succeeded(),
          "identifier exactly at maximum bound is accepted");
    playerDocument.players[0].playerId.push_back('a');
    check(validatePlayerProfileDocument(playerDocument).result == SchemaResult::InvalidValue,
          "identifier beyond maximum bound is rejected");

    playerDocument = players();
    playerDocument.players[0].displayName.assign(kMaximumDisplayNameCodeUnits, L'x');
    check(validatePlayerProfileDocument(playerDocument).succeeded(),
          "display name exactly at maximum bound is accepted");
    playerDocument.players[0].displayName.push_back(L'x');
    check(validatePlayerProfileDocument(playerDocument).result == SchemaResult::BoundsExceeded,
          "display name beyond maximum bound is rejected");

    auto gameDocument = games();
    gameDocument.games[0].installRoot.assign(kMaximumPathCodeUnits, L'x');
    check(validateGameRecordDocument(gameDocument).succeeded(),
          "path exactly at maximum bound is accepted");
    gameDocument.games[0].installRoot.push_back(L'x');
    check(validateGameRecordDocument(gameDocument).result == SchemaResult::BoundsExceeded,
          "path beyond maximum bound is rejected");

    auto setupDocument = setups();
    setupDocument.setups[0].instances[0].arguments.assign(
        kMaximumArgumentsPerInstance, L"arg");
    check(validateTwoPlayerSetupDocument(setupDocument).succeeded(),
          "argument count exactly at maximum bound is accepted");
    setupDocument.setups[0].instances[0].arguments.push_back(L"overflow");
    check(validateTwoPlayerSetupDocument(setupDocument).result == SchemaResult::BoundsExceeded,
          "argument count beyond maximum bound is rejected");

    RuntimeSessionSelection runtime = runtimeSelection();
    runtime.bindings.push_back({3u, "player.three", "game.shared", std::nullopt, 0u});
    check(encodeRuntimeSessionSelection(runtime).empty(),
          "temporary runtime schema cannot encode a third binding");

    std::string oversized(kMaximumSchemaDocumentBytes + 1u, ' ');
    PlayerProfileDocument output;
    check(decodePlayerProfileDocument(oversized, output).result ==
              SchemaResult::DocumentTooLarge,
          "oversized JSON document is rejected before parsing");
}

void testDuplicateAndCrossReferenceValidation() {
    auto playerDocument = players();
    playerDocument.players.push_back(playerDocument.players.front());
    check(validatePlayerProfileDocument(playerDocument).result == SchemaResult::DuplicateId,
          "duplicate Player ID is rejected");

    auto gameDocument = games();
    gameDocument.games.push_back(gameDocument.games.front());
    check(validateGameRecordDocument(gameDocument).result == SchemaResult::DuplicateId,
          "duplicate Game ID is rejected");

    auto setupDocument = setups();
    setupDocument.setups[0].instances.pop_back();
    check(validateTwoPlayerSetupDocument(setupDocument).result == SchemaResult::InvalidValue,
          "TwoPlayerSetup requires exactly two instance recipes");

    const auto seatDocument = seats();
    const auto validPlayers = players();
    const auto validGames = games();
    const auto validSetups = setups();

    auto runtime = runtimeSelection();
    runtime.bindings[0].playerId = "missing.player";
    check(validateRuntimeSessionSelection(runtime, seatDocument, validPlayers,
                                          validGames, validSetups).result ==
              SchemaResult::CrossReferenceError,
          "runtime binding fails when Player reference is missing");

    runtime = runtimeSelection();
    runtime.bindings[0].gameId = "missing.game";
    check(validateRuntimeSessionSelection(runtime, seatDocument, validPlayers,
                                          validGames, validSetups).result ==
              SchemaResult::CrossReferenceError,
          "runtime binding fails when Game reference is missing");

    runtime = runtimeSelection();
    runtime.bindings[0].seatId = 2u;
    check(validateRuntimeSessionSelection(runtime, seatDocument, validPlayers,
                                          validGames, validSetups).result ==
              SchemaResult::DuplicateId,
          "runtime binding cannot assign one Seat twice");

    runtime = runtimeSelection();
    runtime.bindings[1].playerId = runtime.bindings[0].playerId;
    check(validateRuntimeSessionSelection(runtime, seatDocument, validPlayers,
                                          validGames, validSetups).result ==
              SchemaResult::DuplicateId,
          "runtime binding cannot assign one Player twice");

    runtime = runtimeSelection();
    runtime.bindings[1].instanceIndex = 0u;
    check(validateRuntimeSessionSelection(runtime, seatDocument, validPlayers,
                                          validGames, validSetups).result ==
              SchemaResult::DuplicateId,
          "same TwoPlayerSetup instance cannot be assigned twice");

    runtime = runtimeSelection();
    runtime.bindings[0].setupId.reset();
    runtime.bindings[0].instanceIndex = 1u;
    check(validateRuntimeSessionSelection(runtime, seatDocument, validPlayers,
                                          validGames, validSetups).result ==
              SchemaResult::InvalidValue,
          "instance index cannot silently survive without a setup reference");

    auto mismatchedSetups = validSetups;
    mismatchedSetups.setups[0].gameId = "different.game";
    runtime = runtimeSelection();
    check(validateRuntimeSessionSelection(runtime, seatDocument, validPlayers,
                                          validGames, mismatchedSetups).result ==
              SchemaResult::CrossReferenceError,
          "TwoPlayerSetup cannot be applied to a different Game");
}

void testDeviceAndGameValidation() {
    auto seatDocument = seats();
    seatDocument.seats[0].displayIds.push_back(L"display-1");
    check(validateSeatConfigDocument(seatDocument).result == SchemaResult::DuplicateId,
          "Seat device IDs reject case-insensitive duplicates");

    seatDocument = seats();
    seatDocument.seats[0].primaryDisplayId = L"NOT-OWNED";
    check(validateSeatConfigDocument(seatDocument).result == SchemaResult::InvalidValue,
          "primary display must belong to the Seat display list");

    auto gameDocument = games();
    gameDocument.games[0].executableSha256 = "not-a-sha256";
    check(validateGameRecordDocument(gameDocument).result == SchemaResult::InvalidValue,
          "Game hash field is typed as a SHA-256 digest rather than arbitrary text");

    gameDocument = games();
    gameDocument.games[0].executableCandidates.clear();
    check(validateGameRecordDocument(gameDocument).result == SchemaResult::BoundsExceeded,
          "Game record requires at least one executable candidate");

    gameDocument = games();
    gameDocument.games[0].compatibility->evidenceRevision = 0u;
    check(validateGameRecordDocument(gameDocument).result == SchemaResult::InvalidValue,
          "compatibility reference requires a nonzero evidence revision");

    auto setupDocument = setups();
    setupDocument.setups[0].compatibility->provenance = "bad provenance with spaces";
    check(validateTwoPlayerSetupDocument(setupDocument).result == SchemaResult::InvalidValue,
          "compatibility provenance is a bounded logical identifier rather than free text");
}

} // namespace

int main() {
    testRoundTripsAndConceptIsolation();
    testRuntimeSeatConversionContainsOnlyStableSeatState();
    testUnknownSensitiveAndRuntimeFieldsFailClosed();
    testParserAndVersionFailuresAreTransactional();
    testBounds();
    testDuplicateAndCrossReferenceValidation();
    testDeviceAndGameValidation();

    if (failures != 0) {
        std::cerr << failures << " profile schema test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "P6-SCHEMA-01 profile schema tests passed\n";
    return EXIT_SUCCESS;
}
