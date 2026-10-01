#pragma once

#include "hydra/profile_schema.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hydra::setup {

// Compatibility/setup mutations are never allowed to execute at an implicit
// "whenever convenient" point. These four bounded phases are the public timing
// contract consumed by the runtime materialization layer.
enum class RecipeExecutionPhase : std::uint8_t {
    PreSpawn = 0,
    Startup = 1,
    PostWindow = 2,
    Runtime = 3,
};

enum class MutationScope : std::uint8_t {
    SeatWritableInstance = 0,
    SharedInstallation = 1,
};

enum class SetupIssueCode : std::uint8_t {
    Success = 0,
    InvalidGame = 1,
    InvalidSetup = 2,
    InvalidPath = 3,
    SharedDataRoot = 4,
    InvalidInstanceIndex = 5,
    NoDraft = 6,
};

struct SetupDiagnostic {
    SetupIssueCode code{SetupIssueCode::Success};
    std::string message;

    bool succeeded() const noexcept { return code == SetupIssueCode::Success; }
};

enum class MutationIntentKind : std::uint8_t {
    EnsureWorkingDirectory = 0,
    EnsureDataRoot = 1,
};

struct MutationIntent {
    std::string mutationId;
    std::uint32_t instanceIndex{0};
    MutationIntentKind kind{MutationIntentKind::EnsureDataRoot};
    RecipeExecutionPhase phase{RecipeExecutionPhase::PreSpawn};
    MutationScope scope{MutationScope::SeatWritableInstance};
    std::wstring targetPath;

    bool operator==(const MutationIntent&) const = default;
};

struct GeneratedSetupCandidate {
    profile::TwoPlayerSetup setup;
    std::vector<MutationIntent> intendedMutations;

    bool operator==(const GeneratedSetupCandidate&) const = default;
};

struct GenerateSetupInput {
    const profile::GameRecord* game{nullptr};
    std::string setupId;
    std::wstring displayName;
    std::array<profile::InstanceRecipe, 2> instances;
};

// Pure generation from already-discovered/read-only Game metadata. The function
// describes directory intents but never creates directories or edits provider/game files.
SetupDiagnostic generateCandidate(const GenerateSetupInput& input,
                                  GeneratedSetupCandidate& output);

SetupDiagnostic validateSetup(const profile::TwoPlayerSetup& setup,
                              const profile::GameRecord& game);

class SetupEditor final {
public:
    explicit SetupEditor(profile::TwoPlayerSetup committed);

    const profile::TwoPlayerSetup& committed() const noexcept { return committed_; }
    const profile::TwoPlayerSetup& draft() const noexcept { return draft_; }

    void resetDraft() { draft_ = committed_; }
    void setDisplayName(std::wstring value) { draft_.displayName = std::move(value); }
    void setCompatibility(std::optional<profile::CompatibilityReference> value) {
        draft_.compatibility = std::move(value);
    }
    SetupDiagnostic setInstance(std::uint32_t index, profile::InstanceRecipe recipe);
    SetupDiagnostic validateDraft(const profile::GameRecord& game) const;
    // Transactional save: committed state is replaced only after complete validation.
    SetupDiagnostic save(const profile::GameRecord& game);

private:
    profile::TwoPlayerSetup committed_;
    profile::TwoPlayerSetup draft_;
};

profile::RuntimeSessionSelection makeRuntimeSelection(
    const profile::TwoPlayerSetup& setup,
    SeatId firstSeat,
    std::string firstPlayer,
    SeatId secondSeat,
    std::string secondPlayer);

std::string_view setupIssueCodeName(SetupIssueCode code) noexcept;
std::string_view mutationIntentKindName(MutationIntentKind kind) noexcept;
std::string_view recipeExecutionPhaseName(RecipeExecutionPhase phase) noexcept;
std::string_view mutationScopeName(MutationScope scope) noexcept;

} // namespace hydra::setup
