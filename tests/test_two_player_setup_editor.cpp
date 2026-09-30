#include "hydra/two_player_setup_editor.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::setup;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

profile::CompatibilityReference compatibility() {
    return {"compat-a", "fixture", 4u};
}

profile::GameRecord game() {
    profile::GameRecord value;
    value.gameId = "game:a";
    value.providerId = "fake";
    value.providerAppId = "100";
    value.title = L"Fixture Game";
    value.installRoot = L"C:\\Games\\A";
    value.executableCandidates = {L"C:\\Games\\A\\game.exe"};
    value.compatibility = compatibility();
    value.origin = profile::GameOrigin::Discovered;
    return value;
}

std::array<profile::InstanceRecipe, 2> recipes() {
    return {
        profile::InstanceRecipe{{L"--seat=1"}, L"C:\\Games\\A", L"C:\\HydraSeat\\A1"},
        profile::InstanceRecipe{{L"--seat=2"}, L"C:\\Games\\A", L"C:\\HydraSeat\\A2"},
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
    profile::PersistedSeatConfig second;
    second.seatId = 2u;
    second.name = L"Seat 2";
    second.displayIds = {L"display-2"};
    second.primaryDisplayId = L"display-2";
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

void testAutomaticAndManualPathsConverge() {
    const auto selectedGame = game();
    GeneratedSetupCandidate generated;
    const GenerateSetupInput input{&selectedGame, "setup-a", L"Two Player", recipes()};
    check(generateCandidate(input, generated).succeeded(),
          "automatic setup generation succeeds from read-only Game metadata");
    check(generated.intendedMutations.size() == 2u,
          "automatic generation previews both isolated data-root mutations");
    check(generated.intendedMutations.size() == 2u &&
              generated.intendedMutations[0].phase == RecipeExecutionPhase::PreSpawn &&
              generated.intendedMutations[1].phase == RecipeExecutionPhase::PreSpawn &&
              generated.intendedMutations[0].scope == MutationScope::SeatWritableInstance &&
              generated.intendedMutations[1].scope == MutationScope::SeatWritableInstance,
          "automatic setup mutations declare explicit PreSpawn Seat-writable timing/scope");
    check(recipeExecutionPhaseName(RecipeExecutionPhase::PreSpawn) == "PreSpawn" &&
              mutationScopeName(MutationScope::SeatWritableInstance) == "SeatWritableInstance",
          "recipe timing/scope names are stable for diagnostics and integration");

    profile::TwoPlayerSetup manualSeed;
    manualSeed.setupId = "setup-a";
    manualSeed.gameId = "game:a";
    manualSeed.displayName = L"Draft";
    manualSeed.compatibility = compatibility();
    manualSeed.instances = {
        {{L"--old=1"}, L"C:\\Games\\A", L"C:\\HydraSeat\\Old1"},
        {{L"--old=2"}, L"C:\\Games\\A", L"C:\\HydraSeat\\Old2"},
    };
    SetupEditor editor(manualSeed);
    editor.setDisplayName(L"Two Player");
    check(editor.setInstance(0u, recipes()[0]).succeeded() &&
              editor.setInstance(1u, recipes()[1]).succeeded(),
          "manual editor accepts typed instance recipes");
    check(editor.save(selectedGame).succeeded(),
          "manual editor saves a fully valid draft transactionally");
    check(editor.committed() == generated.setup,
          "automatic and guided-manual paths converge on the same setup contract");

    const auto automaticSelection = makeRuntimeSelection(
        generated.setup, 1u, "player-1", 2u, "player-2");
    const auto manualSelection = makeRuntimeSelection(
        editor.committed(), 1u, "player-1", 2u, "player-2");
    check(automaticSelection == manualSelection,
          "automatic and manual setups compile to identical runtime bindings");

    const profile::GameRecordDocument gameDocument{profile::kProfileSchemaVersion,
                                                    {selectedGame}};
    const profile::TwoPlayerSetupDocument setupDocument{profile::kProfileSchemaVersion,
                                                        {generated.setup}};
    const auto validation = profile::validateRuntimeSessionSelection(
        automaticSelection, seats(), players(), gameDocument, setupDocument);
    check(validation.succeeded(),
          "converged runtime binding contract passes cross-document schema validation");
}

void testInvalidManualSavePreservesCommittedState() {
    const auto selectedGame = game();
    GeneratedSetupCandidate generated;
    check(generateCandidate({&selectedGame, "setup-a", L"Two Player", recipes()}, generated)
              .succeeded(),
          "fixture setup generated");

    SetupEditor editor(generated.setup);
    const auto before = editor.committed();
    auto unsafe = recipes()[1];
    unsafe.dataRoot = L"C:\\HydraSeat\\A1";
    check(editor.setInstance(1u, unsafe).succeeded(), "unsafe draft remains editable");
    const auto result = editor.save(selectedGame);
    check(result.code == SetupIssueCode::SharedDataRoot,
          "shared explicit data root is rejected as unsafe");
    check(editor.committed() == before,
          "failed Save preserves the previous valid committed setup");

    editor.resetDraft();
    auto relative = recipes()[0];
    relative.dataRoot = L"relative\\data";
    editor.setInstance(0u, relative);
    check(editor.validateDraft(selectedGame).code == SetupIssueCode::InvalidPath,
          "relative mutation path is rejected continuously before Save");
}

void testGeneratorFailureIsTransactionalAndBounded() {
    const auto selectedGame = game();
    GeneratedSetupCandidate output;
    output.setup.setupId = "sentinel";
    const auto sentinel = output;

    auto invalidRecipes = recipes();
    invalidRecipes[1].dataRoot = invalidRecipes[0].dataRoot;
    const auto result = generateCandidate(
        {&selectedGame, "setup-a", L"Two Player", invalidRecipes}, output);
    check(result.code == SetupIssueCode::SharedDataRoot && output == sentinel,
          "invalid automatic candidate leaves previous output unchanged");

    GeneratedSetupCandidate missingGame = sentinel;
    check(generateCandidate({nullptr, "setup-a", L"Two Player", recipes()}, missingGame).code ==
              SetupIssueCode::InvalidGame &&
              missingGame == sentinel,
          "missing Game fails closed without replacing output");
}

void testInvalidInstanceIndexIsNotScriptEscapeHatch() {
    const auto selectedGame = game();
    GeneratedSetupCandidate generated;
    generateCandidate({&selectedGame, "setup-a", L"Two Player", recipes()}, generated);
    SetupEditor editor(generated.setup);
    check(editor.setInstance(2u, recipes()[0]).code == SetupIssueCode::InvalidInstanceIndex,
          "editor exposes exactly two typed instances and no arbitrary extra execution slot");
}

} // namespace

int main() {
    testAutomaticAndManualPathsConverge();
    testInvalidManualSavePreservesCommittedState();
    testGeneratorFailureIsTransactionalAndBounded();
    testInvalidInstanceIndexIsNotScriptEscapeHatch();
    if (failures != 0) {
        std::cerr << failures << " two-player setup editor test(s) failed\n";
        return 1;
    }
    std::cout << "two-player setup editor tests passed\n";
    return 0;
}
