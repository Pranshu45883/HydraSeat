#pragma once

#include "hydra/instance_materialization.hpp"
#include "hydra/two_seat_launch.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace hydra::production {

enum class CompatibilityActivationCode : std::uint8_t {
    Ok = 0,
    AlreadySatisfied,
    InvalidIdentity,
    WrongSeat,
    WrongGame,
    WrongProvider,
    StaleSetupDecision,
    StaleSession,
    StaleSeatGameGeneration,
    StaleActivationPlan,
    StaleProviderRevision,
    StaleRequirementRevision,
    StaleProviderPlan,
    StaleSourceIdentity,
    StaleInstanceIdentity,
    StaleRecipe,
    MissingLifecycleBoundary,
    WrongPhaseOrder,
    MaterializationFailure,
    AlreadyFinalized,
    RecoveryRequired,
};

struct CompatibilityActivationIdentity {
    SeatId seatId{0};
    std::string setupId;
    std::string localDecisionId;
    std::uint64_t localDecisionRevision{0};
    std::string sessionId;
    std::uint64_t sessionGeneration{0};
    std::uint64_t seatGameGeneration{0};
    std::uint64_t activationFingerprint{0};
    std::uint32_t instanceIndex{0};
    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::uint64_t providerMetadataRevision{0};
    std::uint64_t requirementRevision{0};
    std::uint64_t providerPlanFingerprint{0};
    std::uint64_t sourceIdentityFingerprint{0};
    std::uint64_t instanceIdentityFingerprint{0};
    std::uint64_t recipeFingerprint{0};

    bool operator==(const CompatibilityActivationIdentity&) const = default;
};

struct CompatibilityActivationDiagnostic {
    CompatibilityActivationCode code{CompatibilityActivationCode::Ok};
    materialization::RecipeResult recipeCode{materialization::RecipeResult::Success};
    std::string message;

    bool succeeded() const noexcept {
        return code == CompatibilityActivationCode::Ok ||
               code == CompatibilityActivationCode::AlreadySatisfied;
    }
};

// Reproduces the session-bound identity hash used by the trusted materialization
// compiler. This is intentionally narrow: it authenticates the Seat/session/game/
// provider tuple of an already-compiled plan and does not reimplement recipe
// compilation or provider trust validation.
std::uint64_t compatibilityInstanceIdentityFingerprint(
    const CompatibilityActivationIdentity& identity) noexcept;

// Bind an already-compiled materialization plan to the freshly validated
// provider-aware launch plan used by production activation. Provider/game/Seat/
// revision/fingerprint fields come from current runtime authority, while the
// source/instance/recipe fingerprints remain pinned to the compiled materialization.
// A stale materialization plan will therefore fail validateIdentity() before PreSpawn.
CompatibilityActivationDiagnostic bindCompatibilityActivationIdentity(
    const materialization::InstanceMaterializationPlan& materializationPlan,
    const plan::ProviderAwareLaunchPlan& currentProviderPlan,
    std::string sessionId,
    CompatibilityActivationIdentity& output);

// Convenience snapshot for callers that are already at the trusted compile
// boundary. Production activation should prefer bindCompatibilityActivationIdentity()
// against the freshly revalidated provider plan instead of rebuilding authority
// solely from the materialization plan being consumed.
CompatibilityActivationIdentity makeCompatibilityActivationIdentity(
    const materialization::InstanceMaterializationPlan& plan,
    std::string sessionId);

class ProductionCompatibilityActivation final
    : public launch::ISeatActivationLifecycleHook {
public:
    ProductionCompatibilityActivation(
        materialization::InstanceMaterializationPlan plan,
        CompatibilityActivationIdentity identity,
        materialization::TransactionCheckpointHook checkpointHook = {});

    bool prepare(const launch::SeatActivationPlan& plan,
                 const runtime::SeatGameBinding& binding,
                 std::string& error) override;
    bool preSpawn(std::string& error) override;
    bool startup(std::string& error) override;
    bool postWindow(std::string& error) override;
    bool runtime(std::string& error) override;
    bool rollback(std::string& error) noexcept override;
    bool verifySafe(std::string& error) noexcept override;
    bool recoveryRequired() const noexcept override { return recoveryRequired_; }

    const CompatibilityActivationIdentity& identity() const noexcept { return identity_; }
    const materialization::InstanceMaterializationPlan& plan() const noexcept { return plan_; }
    const std::filesystem::path& instanceRoot() const noexcept { return plan_.instanceRoot; }
    const CompatibilityActivationDiagnostic& diagnostic() const noexcept { return diagnostic_; }
    bool finalized() const noexcept { return finalized_; }
    bool rolledBack() const noexcept { return rolledBack_; }

private:
    bool advance(setup::RecipeExecutionPhase phase, std::string& error);
    CompatibilityActivationDiagnostic validateIdentity() const;
    bool captureBaseline(std::string& error);
    bool verifyExpectedSafeState(std::string& error) const noexcept;
    bool auxiliaryPathsAbsent(std::string& error) const noexcept;
    void setDiagnostic(CompatibilityActivationCode code,
                       materialization::RecipeResult recipeCode,
                       std::string message);

    materialization::InstanceMaterializationPlan plan_;
    CompatibilityActivationIdentity identity_;
    materialization::RecipeExecutionSession session_;
    CompatibilityActivationDiagnostic diagnostic_;
    std::optional<materialization::InstanceState> baselineState_;
    std::uint64_t launchPlanFingerprint_{0};
    std::string boundGameId_;
    int completedPhase_{-1};
    bool prepared_{false};
    bool finalized_{false};
    bool aborted_{false};
    bool rolledBack_{false};
    bool cleanupAfterFinalize_{false};
    bool recoveryRequired_{false};
};

std::unique_ptr<launch::ISeatActivationLifecycleHook>
makeProductionCompatibilityActivation(
    materialization::InstanceMaterializationPlan plan,
    CompatibilityActivationIdentity identity,
    materialization::TransactionCheckpointHook checkpointHook = {});

std::string_view compatibilityActivationCodeName(
    CompatibilityActivationCode code) noexcept;

} // namespace hydra::production
