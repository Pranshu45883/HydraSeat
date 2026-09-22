#pragma once

#include "hydra/game_runtime_requirement_resolver.hpp"
#include "hydra/two_player_setup_editor.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::materialization {

inline constexpr std::uint32_t kCompatibilityRecipeSchemaVersion = 1u;
inline constexpr std::size_t kMaximumCompatibilityRecipeSteps = 16u;
inline constexpr std::size_t kMaximumMutableFilesPerRecipe = 64u;
inline constexpr std::uint64_t kMaximumSingleMutableFileBytes = 16ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaximumMutableBytesPerInstance = 64ull * 1024ull * 1024ull;
inline constexpr std::size_t kMaximumMaterializationSessionIdBytes = 128u;

// A recipe may copy only explicitly named regular files from the directory that
// contains the exact trusted executable. Relative paths are deliberately narrow:
// no shell/script payload, wildcard, directory tree, absolute path, or ".." escape
// is representable by a valid production recipe.
struct MutableFileSpec {
    std::wstring sourceRelativePath;
    std::wstring destinationRelativePath;
    std::uint64_t maximumBytes{kMaximumSingleMutableFileBytes};

    bool operator==(const MutableFileSpec&) const = default;
};

struct CompatibilityRecipeStep {
    std::string stepId;
    setup::RecipeExecutionPhase phase{setup::RecipeExecutionPhase::PreSpawn};
    setup::MutationScope scope{setup::MutationScope::SeatWritableInstance};
    std::vector<MutableFileSpec> files;

    bool operator==(const CompatibilityRecipeStep&) const = default;
};

// Runtime recipe authority is exact and local. Community popularity/history is
// intentionally absent. The recipe still cannot execute until the supplied
// provider plan passes the existing TrustedRequirementSnapshot runtime gate.
struct CompatibilityRecipe {
    std::uint32_t schemaVersion{kCompatibilityRecipeSchemaVersion};
    SeatId seatId{0};
    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::uint64_t providerMetadataRevision{0};
    std::uint64_t requirementRevision{0};
    std::optional<profile::CompatibilityReference> compatibility;
    std::vector<CompatibilityRecipeStep> steps;

    bool operator==(const CompatibilityRecipe&) const = default;
};

struct MaterializationContext {
    // Product-owned root. Final per-instance destinations are derived by the
    // compiler and cannot be selected by recipe/package content.
    std::filesystem::path instancesRoot;
    // Bounded opaque runtime-session identity; used only to derive a deterministic
    // Seat/session instance destination. It is never interpreted as a path.
    std::string sessionId;

    bool operator==(const MaterializationContext&) const = default;
};

struct PlannedMutableFile {
    std::filesystem::path sourcePath;
    std::filesystem::path destinationRelativePath;
    std::uint64_t maximumBytes{0};

    bool operator==(const PlannedMutableFile&) const = default;
};

struct PlannedCompatibilityStep {
    std::string stepId;
    setup::RecipeExecutionPhase phase{setup::RecipeExecutionPhase::PreSpawn};
    std::vector<PlannedMutableFile> files;

    bool operator==(const PlannedCompatibilityStep&) const = default;
};

struct InstanceMaterializationPlan {
    std::uint32_t schemaVersion{kCompatibilityRecipeSchemaVersion};
    SeatId seatId{0};
    std::uint32_t instanceIndex{0};
    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::uint64_t providerMetadataRevision{0};
    std::uint64_t requirementRevision{0};
    std::optional<profile::CompatibilityReference> compatibility;
    // Runtime-only authority stamped after the pure recipe compiler succeeds.
    // Portable/community bytes cannot supply these fields.
    std::string setupId;
    std::string localDecisionId;
    std::uint64_t localDecisionRevision{0};
    std::string sessionId;
    std::uint64_t sessionGeneration{0};
    std::uint64_t seatGameGeneration{0};
    std::uint64_t activationFingerprint{0};
    std::uint64_t providerPlanFingerprint{0};
    std::uint64_t sourceIdentityFingerprint{0};
    std::uint64_t instanceIdentityFingerprint{0};
    std::uint64_t recipeFingerprint{0};
    std::filesystem::path sourceRoot;
    std::filesystem::path instanceRoot;
    std::filesystem::path stagingRoot;
    std::filesystem::path rollbackRoot;
    std::filesystem::path previousPhaseRoot;
    std::vector<PlannedCompatibilityStep> steps;

    bool operator==(const InstanceMaterializationPlan&) const = default;
};

enum class RecipeResult : std::uint8_t {
    Success = 0,
    AlreadyCurrent,
    InvalidRecipe,
    UnsupportedPhase,
    WrongPhaseOrder,
    WrongGameIdentity,
    WrongProviderIdentity,
    StaleProviderRevision,
    StaleRequirementRevision,
    UntrustedProviderPlan,
    InvalidPath,
    BoundsExceeded,
    ConflictingMutation,
    SharedInstallationMutationDenied,
    UnsupportedSourceLayout,
    SourceUnavailable,
    ReparsePointRejected,
    StaleInstance,
    UnsafeInstance,
    StagingFailed,
    CommitFailed,
    RollbackFailed,
    CleanupFailed,
    AlreadyFinalized,
};

struct RecipeDiagnostic {
    RecipeResult code{RecipeResult::Success};
    std::string message;

    bool succeeded() const noexcept {
        return code == RecipeResult::Success || code == RecipeResult::AlreadyCurrent;
    }
};

// Pure compiler. No filesystem mutation occurs here. It first invokes the same
// exact trusted requirement gate used by production host plan installation, then
// canonicalizes phases/steps/files and derives one Seat/session-owned destination.
RecipeDiagnostic compileInstanceMaterializationPlan(
    const CompatibilityRecipe& recipe,
    const plan::ProviderAwareLaunchPlan& providerPlan,
    const requirement::TrustedRequirementSnapshot& trustedSnapshot,
    const MaterializationContext& context,
    InstanceMaterializationPlan& output);

enum class InstanceState : std::uint8_t {
    Missing = 0,
    Current,
    Partial,
    Stale,
    Unsafe,
};

RecipeDiagnostic inspectInstanceMaterialization(
    const InstanceMaterializationPlan& plan,
    InstanceState& state);

// Crash/interruption recovery touches only deterministic HydraSeat-owned paths.
// It removes owned staging debris and restores a retained pre-recipe instance
// when a commit was interrupted after the previous instance was moved aside.
RecipeDiagnostic recoverInterruptedMaterialization(
    const InstanceMaterializationPlan& plan);

// Idempotent cleanup. Existing foreign/unmarked paths are never deleted.
RecipeDiagnostic cleanupInstanceMaterialization(
    const InstanceMaterializationPlan& plan);

enum class TransactionCheckpoint : std::uint8_t {
    StagingValidated = 0,
    PreviousInstanceMoved = 1,
};

// Optional veto-only hook for deterministic failure injection/diagnostics. It can
// force a transaction to fail but cannot widen paths, phases, recipe authority, or
// filesystem permissions. Production callers normally omit it.
using TransactionCheckpointHook =
    std::function<bool(TransactionCheckpoint checkpoint, std::string& reason)>;

class RecipeExecutionSession final {
public:
    explicit RecipeExecutionSession(
        InstanceMaterializationPlan plan,
        TransactionCheckpointHook checkpointHook = {});

    // Phases must be called exactly in order: PreSpawn -> Startup -> PostWindow -> Runtime.
    // A failure reverses every earlier phase applied by this session.
    RecipeDiagnostic executePhase(setup::RecipeExecutionPhase phase);
    RecipeDiagnostic rollback();

    const InstanceMaterializationPlan& plan() const noexcept { return plan_; }
    const std::filesystem::path& instanceRoot() const noexcept { return plan_.instanceRoot; }
    bool finalized() const noexcept { return finalized_; }
    bool failed() const noexcept { return failed_; }

private:
    RecipeDiagnostic ensureRecovered();
    RecipeDiagnostic materializeThrough(setup::RecipeExecutionPhase phase);
    RecipeDiagnostic reverseApplied();

    InstanceMaterializationPlan plan_;
    TransactionCheckpointHook checkpointHook_;
    int completedPhase_{-1};
    bool recovered_{false};
    bool originalExisted_{false};
    bool originalBackedUp_{false};
    bool reusedCurrent_{false};
    bool finalized_{false};
    bool failed_{false};
};

std::string_view recipeResultName(RecipeResult result) noexcept;
std::string_view instanceStateName(InstanceState state) noexcept;

} // namespace hydra::materialization
