#include "hydra/two_player_setup_editor.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace hydra::setup {
namespace {

SetupDiagnostic fail(SetupIssueCode code, std::string message) {
    return {code, std::move(message)};
}

bool isAsciiAlpha(wchar_t value) noexcept {
    return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z');
}

bool isAbsoluteWindowsPath(std::wstring_view path) noexcept {
    if (path.size() >= 3u && isAsciiAlpha(path[0]) && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    return path.size() >= 3u &&
           ((path[0] == L'\\' && path[1] == L'\\') ||
            (path[0] == L'/' && path[1] == L'/'));
}

SetupDiagnostic validateRecipePaths(const profile::TwoPlayerSetup& setup) {
    for (std::size_t index = 0; index < setup.instances.size(); ++index) {
        const auto& recipe = setup.instances[index];
        if (recipe.workingDirectory && !isAbsoluteWindowsPath(*recipe.workingDirectory)) {
            return fail(SetupIssueCode::InvalidPath,
                        "instance " + std::to_string(index) +
                            " working directory must be an absolute Windows path");
        }
        if (recipe.dataRoot && !isAbsoluteWindowsPath(*recipe.dataRoot)) {
            return fail(SetupIssueCode::InvalidPath,
                        "instance " + std::to_string(index) +
                            " data root must be an absolute Windows path");
        }
    }
    if (setup.instances.size() == 2u && setup.instances[0].dataRoot &&
        setup.instances[1].dataRoot &&
        *setup.instances[0].dataRoot == *setup.instances[1].dataRoot) {
        return fail(SetupIssueCode::SharedDataRoot,
                    "two instances cannot claim the same explicit data root");
    }
    return {};
}

} // namespace

SetupDiagnostic validateSetup(const profile::TwoPlayerSetup& setup,
                              const profile::GameRecord& game) {
    profile::GameRecordDocument gameDocument;
    gameDocument.games = {game};
    const auto gameDiagnostic = profile::validateGameRecordDocument(gameDocument);
    if (!gameDiagnostic.succeeded()) {
        return fail(SetupIssueCode::InvalidGame,
                    "game record is invalid: " + gameDiagnostic.message);
    }
    if (setup.gameId != game.gameId) {
        return fail(SetupIssueCode::InvalidSetup,
                    "two-player setup does not reference the selected Game");
    }
    if (setup.compatibility && setup.compatibility != game.compatibility) {
        return fail(SetupIssueCode::InvalidSetup,
                    "two-player setup compatibility reference is stale");
    }

    profile::TwoPlayerSetupDocument setupDocument;
    setupDocument.setups = {setup};
    const auto setupDiagnostic = profile::validateTwoPlayerSetupDocument(setupDocument);
    if (!setupDiagnostic.succeeded()) {
        return fail(SetupIssueCode::InvalidSetup,
                    "two-player setup schema is invalid: " + setupDiagnostic.message);
    }
    return validateRecipePaths(setup);
}

SetupDiagnostic generateCandidate(const GenerateSetupInput& input,
                                  GeneratedSetupCandidate& output) {
    if (input.game == nullptr) {
        return fail(SetupIssueCode::InvalidGame,
                    "automatic setup generation requires one selected Game");
    }

    GeneratedSetupCandidate candidate;
    candidate.setup.setupId = input.setupId;
    candidate.setup.gameId = input.game->gameId;
    candidate.setup.displayName = input.displayName;
    candidate.setup.compatibility = input.game->compatibility;
    candidate.setup.instances.assign(input.instances.begin(), input.instances.end());

    const auto diagnostic = validateSetup(candidate.setup, *input.game);
    if (!diagnostic.succeeded()) return diagnostic;

    for (std::size_t index = 0; index < candidate.setup.instances.size(); ++index) {
        const auto& recipe = candidate.setup.instances[index];
        if (recipe.dataRoot) {
            MutationIntent intent;
            intent.mutationId = candidate.setup.setupId + "-instance-" +
                                std::to_string(index) + "-data-root";
            intent.instanceIndex = static_cast<std::uint32_t>(index);
            intent.kind = MutationIntentKind::EnsureDataRoot;
            intent.phase = RecipeExecutionPhase::PreSpawn;
            intent.scope = MutationScope::SeatWritableInstance;
            intent.targetPath = *recipe.dataRoot;
            candidate.intendedMutations.push_back(std::move(intent));
        }
    }

    output = std::move(candidate);
    return {};
}

SetupEditor::SetupEditor(profile::TwoPlayerSetup committed)
    : committed_(std::move(committed)), draft_(committed_) {}

SetupDiagnostic SetupEditor::setInstance(std::uint32_t index,
                                         profile::InstanceRecipe recipe) {
    if (index >= 2u || draft_.instances.size() != 2u) {
        return fail(SetupIssueCode::InvalidInstanceIndex,
                    "two-player setup exposes exactly two typed instances");
    }
    draft_.instances[index] = std::move(recipe);
    return {};
}

SetupDiagnostic SetupEditor::validateDraft(const profile::GameRecord& game) const {
    return validateSetup(draft_, game);
}

SetupDiagnostic SetupEditor::save(const profile::GameRecord& game) {
    const auto diagnostic = validateDraft(game);
    if (!diagnostic.succeeded()) return diagnostic;
    committed_ = draft_;
    return {};
}

profile::RuntimeSessionSelection makeRuntimeSelection(
    const profile::TwoPlayerSetup& setup,
    SeatId firstSeat,
    std::string firstPlayer,
    SeatId secondSeat,
    std::string secondPlayer) {
    profile::RuntimeSessionSelection selection;
    selection.bindings = {
        {firstSeat, std::move(firstPlayer), setup.gameId, setup.setupId, 0u},
        {secondSeat, std::move(secondPlayer), setup.gameId, setup.setupId, 1u},
    };
    std::sort(selection.bindings.begin(), selection.bindings.end(),
              [](const auto& left, const auto& right) {
                  return left.seatId < right.seatId;
              });
    return selection;
}

std::string_view setupIssueCodeName(SetupIssueCode code) noexcept {
    switch (code) {
    case SetupIssueCode::Success: return "Success";
    case SetupIssueCode::InvalidGame: return "InvalidGame";
    case SetupIssueCode::InvalidSetup: return "InvalidSetup";
    case SetupIssueCode::InvalidPath: return "InvalidPath";
    case SetupIssueCode::SharedDataRoot: return "SharedDataRoot";
    case SetupIssueCode::InvalidInstanceIndex: return "InvalidInstanceIndex";
    case SetupIssueCode::NoDraft: return "NoDraft";
    }
    return "Unknown";
}

std::string_view mutationIntentKindName(MutationIntentKind kind) noexcept {
    switch (kind) {
    case MutationIntentKind::EnsureWorkingDirectory: return "EnsureWorkingDirectory";
    case MutationIntentKind::EnsureDataRoot: return "EnsureDataRoot";
    }
    return "Unknown";
}

std::string_view recipeExecutionPhaseName(RecipeExecutionPhase phase) noexcept {
    switch (phase) {
    case RecipeExecutionPhase::PreSpawn: return "PreSpawn";
    case RecipeExecutionPhase::Startup: return "Startup";
    case RecipeExecutionPhase::PostWindow: return "PostWindow";
    case RecipeExecutionPhase::Runtime: return "Runtime";
    }
    return "Unknown";
}

std::string_view mutationScopeName(MutationScope scope) noexcept {
    switch (scope) {
    case MutationScope::SeatWritableInstance: return "SeatWritableInstance";
    case MutationScope::SharedInstallation: return "SharedInstallation";
    }
    return "Unknown";
}

} // namespace hydra::setup
