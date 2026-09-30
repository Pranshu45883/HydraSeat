#include "hydra/production_compatibility_activation.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

namespace hydra::production {
namespace {

class IdentityHash final {
public:
    void byte(std::uint8_t value) noexcept {
        value_ ^= static_cast<std::uint64_t>(value);
        value_ *= 1099511628211ull;
    }

    void boolean(bool value) noexcept { byte(value ? 1u : 0u); }

    void u32(std::uint32_t value) noexcept {
        for (unsigned int shift = 0u; shift < 32u; shift += 8u) {
            byte(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }

    void u64(std::uint64_t value) noexcept {
        for (unsigned int shift = 0u; shift < 64u; shift += 8u) {
            byte(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }

    void text(std::string_view value) noexcept {
        u64(static_cast<std::uint64_t>(value.size()));
        for (const char ch : value) {
            byte(static_cast<std::uint8_t>(static_cast<unsigned char>(ch)));
        }
    }

    std::uint64_t value() const noexcept { return value_ == 0u ? 1u : value_; }

private:
    std::uint64_t value_{1469598103934665603ull};
};

bool validSessionId(std::string_view value) noexcept {
    if (value.empty() ||
        value.size() > materialization::kMaximumMaterializationSessionIdBytes) {
        return false;
    }
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        const bool alphaNumeric = (ch >= 'a' && ch <= 'z') ||
                                  (ch >= 'A' && ch <= 'Z') ||
                                  (ch >= '0' && ch <= '9');
        if (!(alphaNumeric || ch == '-' || ch == '_' || ch == '.')) return false;
    }
    return true;
}

int phaseOrdinal(setup::RecipeExecutionPhase phase) noexcept {
    switch (phase) {
        case setup::RecipeExecutionPhase::PreSpawn: return 0;
        case setup::RecipeExecutionPhase::Startup: return 1;
        case setup::RecipeExecutionPhase::PostWindow: return 2;
        case setup::RecipeExecutionPhase::Runtime: return 3;
    }
    return -1;
}

bool containsExactlyOnce(std::span<const launch::ResourceKind> resources,
                         launch::ResourceKind target,
                         std::size_t& index) noexcept {
    std::size_t count = 0u;
    for (std::size_t current = 0u; current < resources.size(); ++current) {
        if (resources[current] != target) continue;
        index = current;
        ++count;
    }
    return count == 1u;
}

CompatibilityActivationDiagnostic failure(
    CompatibilityActivationCode code,
    std::string message,
    materialization::RecipeResult recipeCode = materialization::RecipeResult::Success) {
    return {code, recipeCode, std::move(message)};
}

} // namespace

std::uint64_t compatibilityInstanceIdentityFingerprint(
    const CompatibilityActivationIdentity& identity) noexcept {
    IdentityHash hash;
    hash.text(identity.sessionId);
    hash.u32(identity.seatId);
    hash.u32(identity.instanceIndex);
    hash.text(identity.gameId);
    hash.text(identity.providerId);
    hash.boolean(identity.providerAppId.has_value());
    if (identity.providerAppId) hash.text(*identity.providerAppId);
    return hash.value();
}

CompatibilityActivationDiagnostic bindCompatibilityActivationIdentity(
    const materialization::InstanceMaterializationPlan& materializationPlan,
    const plan::ProviderAwareLaunchPlan& currentProviderPlan,
    std::string sessionId,
    CompatibilityActivationIdentity& output) {
    output = {};
    if (!validSessionId(sessionId) ||
        currentProviderPlan.schemaVersion != plan::kProviderLaunchPlanSchemaVersion ||
        currentProviderPlan.fingerprint == 0u ||
        plan::recomputeProviderAwareLaunchPlanFingerprint(currentProviderPlan) !=
            currentProviderPlan.fingerprint) {
        return failure(CompatibilityActivationCode::InvalidIdentity,
                       "fresh provider-aware activation authority is malformed or not canonical");
    }

    const plan::SeatProviderLaunchPlan* currentSeat = nullptr;
    for (const auto& seat : currentProviderPlan.seats) {
        if (seat.seatId != materializationPlan.seatId) continue;
        if (currentSeat != nullptr) {
            return failure(CompatibilityActivationCode::WrongSeat,
                           "fresh provider-aware plan contains duplicate Seat authority");
        }
        currentSeat = &seat;
    }
    if (currentSeat == nullptr) {
        return failure(CompatibilityActivationCode::WrongSeat,
                       "fresh provider-aware plan does not contain the materialization Seat");
    }
    if (currentSeat->gameId.empty() || currentSeat->launchRequest.providerId.empty() ||
        currentSeat->launchRequest.gameId != currentSeat->gameId ||
        currentSeat->launchRequest.metadataRevision == 0u ||
        currentSeat->requirementRevision == 0u) {
        return failure(CompatibilityActivationCode::InvalidIdentity,
                       "fresh provider-aware Seat authority is incomplete");
    }
    if (!currentSeat->setupId || materializationPlan.setupId.empty() ||
        *currentSeat->setupId != materializationPlan.setupId ||
        materializationPlan.localDecisionId.empty() ||
        materializationPlan.localDecisionRevision == 0u) {
        return failure(CompatibilityActivationCode::StaleSetupDecision,
                       "compiled materialization is not bound to the current local setup decision");
    }
    if (materializationPlan.sessionId != sessionId ||
        materializationPlan.sessionGeneration == 0u) {
        return failure(CompatibilityActivationCode::StaleSession,
                       "compiled materialization is not bound to the current Host session generation");
    }
    if (materializationPlan.seatGameGeneration == 0u) {
        return failure(CompatibilityActivationCode::StaleSeatGameGeneration,
                       "compiled materialization has no Seat-game generation authority");
    }
    if (materializationPlan.activationFingerprint == 0u) {
        return failure(CompatibilityActivationCode::StaleActivationPlan,
                       "compiled materialization has no immutable activation fingerprint");
    }

    output.seatId = currentSeat->seatId;
    output.setupId = materializationPlan.setupId;
    output.localDecisionId = materializationPlan.localDecisionId;
    output.localDecisionRevision = materializationPlan.localDecisionRevision;
    output.sessionId = std::move(sessionId);
    output.sessionGeneration = materializationPlan.sessionGeneration;
    output.seatGameGeneration = materializationPlan.seatGameGeneration;
    output.activationFingerprint = materializationPlan.activationFingerprint;
    output.instanceIndex = currentSeat->instanceIndex;
    output.gameId = currentSeat->gameId;
    output.providerId = currentSeat->launchRequest.providerId;
    output.providerAppId = currentSeat->launchRequest.providerAppId;
    output.providerMetadataRevision = currentSeat->launchRequest.metadataRevision;
    output.requirementRevision = currentSeat->requirementRevision;
    output.providerPlanFingerprint = currentProviderPlan.fingerprint;
    output.sourceIdentityFingerprint = materializationPlan.sourceIdentityFingerprint;
    output.instanceIdentityFingerprint = materializationPlan.instanceIdentityFingerprint;
    output.recipeFingerprint = materializationPlan.recipeFingerprint;
    return {};
}

CompatibilityActivationIdentity makeCompatibilityActivationIdentity(
    const materialization::InstanceMaterializationPlan& plan,
    std::string sessionId) {
    CompatibilityActivationIdentity identity;
    identity.seatId = plan.seatId;
    identity.setupId = plan.setupId;
    identity.localDecisionId = plan.localDecisionId;
    identity.localDecisionRevision = plan.localDecisionRevision;
    identity.sessionId = std::move(sessionId);
    identity.sessionGeneration = plan.sessionGeneration;
    identity.seatGameGeneration = plan.seatGameGeneration;
    identity.activationFingerprint = plan.activationFingerprint;
    identity.instanceIndex = plan.instanceIndex;
    identity.gameId = plan.gameId;
    identity.providerId = plan.providerId;
    identity.providerAppId = plan.providerAppId;
    identity.providerMetadataRevision = plan.providerMetadataRevision;
    identity.requirementRevision = plan.requirementRevision;
    identity.providerPlanFingerprint = plan.providerPlanFingerprint;
    identity.sourceIdentityFingerprint = plan.sourceIdentityFingerprint;
    identity.instanceIdentityFingerprint = plan.instanceIdentityFingerprint;
    identity.recipeFingerprint = plan.recipeFingerprint;
    return identity;
}

ProductionCompatibilityActivation::ProductionCompatibilityActivation(
    materialization::InstanceMaterializationPlan plan,
    CompatibilityActivationIdentity identity,
    materialization::TransactionCheckpointHook checkpointHook)
    : plan_(std::move(plan)),
      identity_(std::move(identity)),
      session_(plan_, std::move(checkpointHook)) {}

CompatibilityActivationDiagnostic
ProductionCompatibilityActivation::validateIdentity() const {
    if (plan_.schemaVersion != materialization::kCompatibilityRecipeSchemaVersion ||
        identity_.seatId == 0u || identity_.setupId.empty() ||
        identity_.localDecisionId.empty() || identity_.localDecisionRevision == 0u ||
        identity_.sessionGeneration == 0u || identity_.seatGameGeneration == 0u ||
        identity_.activationFingerprint == 0u ||
        identity_.gameId.empty() || identity_.providerId.empty() ||
        identity_.providerMetadataRevision == 0u || identity_.requirementRevision == 0u ||
        identity_.providerPlanFingerprint == 0u || identity_.sourceIdentityFingerprint == 0u ||
        identity_.instanceIdentityFingerprint == 0u || identity_.recipeFingerprint == 0u ||
        !validSessionId(identity_.sessionId) || plan_.seatId == 0u || plan_.setupId.empty() ||
        plan_.localDecisionId.empty() || plan_.localDecisionRevision == 0u ||
        !validSessionId(plan_.sessionId) || plan_.sessionGeneration == 0u ||
        plan_.seatGameGeneration == 0u || plan_.activationFingerprint == 0u ||
        plan_.providerPlanFingerprint == 0u || plan_.sourceIdentityFingerprint == 0u ||
        plan_.instanceIdentityFingerprint == 0u || plan_.recipeFingerprint == 0u ||
        !plan_.instanceRoot.is_absolute() || !plan_.stagingRoot.is_absolute() ||
        !plan_.rollbackRoot.is_absolute() || !plan_.previousPhaseRoot.is_absolute()) {
        return failure(CompatibilityActivationCode::InvalidIdentity,
                       "compatibility activation identity/plan is malformed or unbounded");
    }
    if (identity_.seatId != plan_.seatId || identity_.instanceIndex != plan_.instanceIndex) {
        return failure(CompatibilityActivationCode::WrongSeat,
                       "compatibility activation belongs to another Seat/instance index");
    }
    if (identity_.setupId != plan_.setupId ||
        identity_.localDecisionId != plan_.localDecisionId ||
        identity_.localDecisionRevision != plan_.localDecisionRevision) {
        return failure(CompatibilityActivationCode::StaleSetupDecision,
                       "compatibility activation local setup/materialization decision is stale");
    }
    if (identity_.sessionId != plan_.sessionId ||
        identity_.sessionGeneration != plan_.sessionGeneration) {
        return failure(CompatibilityActivationCode::StaleSession,
                       "compatibility activation Host session identity/generation is stale");
    }
    if (identity_.seatGameGeneration != plan_.seatGameGeneration) {
        return failure(CompatibilityActivationCode::StaleSeatGameGeneration,
                       "compatibility activation Seat-game generation is stale");
    }
    if (identity_.activationFingerprint != plan_.activationFingerprint) {
        return failure(CompatibilityActivationCode::StaleActivationPlan,
                       "compatibility activation immutable Seat activation fingerprint is stale");
    }
    if (identity_.gameId != plan_.gameId) {
        return failure(CompatibilityActivationCode::WrongGame,
                       "compatibility activation Game identity does not match the compiled plan");
    }
    if (identity_.providerId != plan_.providerId ||
        identity_.providerAppId != plan_.providerAppId) {
        return failure(CompatibilityActivationCode::WrongProvider,
                       "compatibility activation provider/application identity does not match");
    }
    if (identity_.providerMetadataRevision != plan_.providerMetadataRevision) {
        return failure(CompatibilityActivationCode::StaleProviderRevision,
                       "compatibility activation provider metadata revision is stale");
    }
    if (identity_.requirementRevision != plan_.requirementRevision) {
        return failure(CompatibilityActivationCode::StaleRequirementRevision,
                       "compatibility activation requirement revision is stale");
    }
    if (identity_.providerPlanFingerprint != plan_.providerPlanFingerprint) {
        return failure(CompatibilityActivationCode::StaleProviderPlan,
                       "compatibility activation provider plan fingerprint is stale");
    }
    if (identity_.sourceIdentityFingerprint != plan_.sourceIdentityFingerprint) {
        return failure(CompatibilityActivationCode::StaleSourceIdentity,
                       "compatibility activation source identity fingerprint is stale");
    }
    const auto sessionBound = compatibilityInstanceIdentityFingerprint(identity_);
    if (sessionBound != plan_.instanceIdentityFingerprint) {
        return failure(CompatibilityActivationCode::StaleSession,
                       "compatibility activation session identity does not match the compiled instance");
    }
    if (identity_.instanceIdentityFingerprint != plan_.instanceIdentityFingerprint) {
        return failure(CompatibilityActivationCode::StaleInstanceIdentity,
                       "compatibility activation instance identity fingerprint is stale");
    }
    if (identity_.recipeFingerprint != plan_.recipeFingerprint) {
        return failure(CompatibilityActivationCode::StaleRecipe,
                       "compatibility activation recipe fingerprint is stale");
    }
    return {};
}

bool ProductionCompatibilityActivation::prepare(
    const launch::SeatActivationPlan& plan,
    const runtime::SeatGameBinding& binding,
    std::string& error) {
    error.clear();
    if (recoveryRequired_) {
        error = "compatibility activation retains recovery ownership";
        setDiagnostic(CompatibilityActivationCode::RecoveryRequired,
                      materialization::RecipeResult::RollbackFailed, error);
        return false;
    }
    if (finalized_ || aborted_ || rolledBack_) {
        error = "compatibility activation lifecycle cannot be rebound after completion/abort";
        setDiagnostic(CompatibilityActivationCode::AlreadyFinalized,
                      materialization::RecipeResult::AlreadyFinalized, error);
        return false;
    }

    const auto identity = validateIdentity();
    if (!identity.succeeded()) {
        diagnostic_ = identity;
        error = identity.message;
        return false;
    }
    if (plan.seatId != identity_.seatId) {
        error = "Seat activation plan belongs to another Seat";
        setDiagnostic(CompatibilityActivationCode::WrongSeat,
                      materialization::RecipeResult::Success, error);
        return false;
    }
    if (plan.target.gameId != identity_.gameId || binding.gameId != identity_.gameId) {
        error = "Seat activation plan/binding Game identity does not match compatibility plan";
        setDiagnostic(CompatibilityActivationCode::WrongGame,
                      materialization::RecipeResult::Success, error);
        return false;
    }
    if (plan.fingerprint == 0u || plan.fingerprint != identity_.activationFingerprint ||
        plan.fingerprint != plan_.activationFingerprint) {
        error = "Seat activation plan fingerprint does not match the trusted materialization epoch";
        setDiagnostic(CompatibilityActivationCode::StaleActivationPlan,
                      materialization::RecipeResult::Success, error);
        return false;
    }

    std::size_t processIndex = 0u;
    std::size_t windowIndex = 0u;
    if (!containsExactlyOnce(plan.resources, launch::ResourceKind::Process, processIndex) ||
        !containsExactlyOnce(plan.resources, launch::ResourceKind::Window, windowIndex) ||
        processIndex >= windowIndex) {
        error = "compatibility lifecycle requires one Process boundary followed by one Window boundary";
        setDiagnostic(CompatibilityActivationCode::MissingLifecycleBoundary,
                      materialization::RecipeResult::Success, error);
        return false;
    }

    if (prepared_) {
        if (launchPlanFingerprint_ == plan.fingerprint && boundGameId_ == binding.gameId) {
            setDiagnostic(CompatibilityActivationCode::AlreadySatisfied,
                          materialization::RecipeResult::Success,
                          "compatibility lifecycle is already bound to this immutable activation");
            return true;
        }
        error = "compatibility lifecycle was already prepared for another activation identity";
        setDiagnostic(CompatibilityActivationCode::InvalidIdentity,
                      materialization::RecipeResult::Success, error);
        return false;
    }

    launchPlanFingerprint_ = plan.fingerprint;
    boundGameId_ = binding.gameId;
    prepared_ = true;
    setDiagnostic(CompatibilityActivationCode::Ok,
                  materialization::RecipeResult::Success,
                  "compatibility lifecycle bound without filesystem mutation");
    return true;
}

bool ProductionCompatibilityActivation::captureBaseline(std::string& error) {
    error.clear();
    const auto recovered = materialization::recoverInterruptedMaterialization(plan_);
    if (!recovered.succeeded()) {
        error = "interrupted compatibility materialization could not be recovered: " +
                recovered.message;
        setDiagnostic(CompatibilityActivationCode::RecoveryRequired,
                      recovered.code, error);
        recoveryRequired_ = true;
        return false;
    }

    materialization::InstanceState state = materialization::InstanceState::Unsafe;
    const auto inspected = materialization::inspectInstanceMaterialization(plan_, state);
    if (state == materialization::InstanceState::Unsafe ||
        state == materialization::InstanceState::Partial) {
        error = "compatibility materialization baseline is not a verified recoverable state";
        if (!inspected.message.empty()) error += ": " + inspected.message;
        setDiagnostic(CompatibilityActivationCode::RecoveryRequired,
                      inspected.code, error);
        recoveryRequired_ = true;
        return false;
    }
    baselineState_ = state;
    return true;
}

bool ProductionCompatibilityActivation::advance(
    setup::RecipeExecutionPhase phase,
    std::string& error) {
    error.clear();
    if (!prepared_) {
        error = "compatibility lifecycle phase executed before prepare";
        setDiagnostic(CompatibilityActivationCode::InvalidIdentity,
                      materialization::RecipeResult::WrongPhaseOrder, error);
        return false;
    }
    if (recoveryRequired_) {
        error = "compatibility lifecycle is blocked by retained recovery ownership";
        setDiagnostic(CompatibilityActivationCode::RecoveryRequired,
                      materialization::RecipeResult::RollbackFailed, error);
        return false;
    }
    if (finalized_) {
        error = "compatibility lifecycle is already finalized";
        setDiagnostic(CompatibilityActivationCode::AlreadyFinalized,
                      materialization::RecipeResult::AlreadyFinalized, error);
        return false;
    }
    if (aborted_ || rolledBack_) {
        error = "compatibility lifecycle was already aborted/rolled back";
        setDiagnostic(CompatibilityActivationCode::AlreadyFinalized,
                      materialization::RecipeResult::AlreadyFinalized, error);
        return false;
    }

    const int ordinal = phaseOrdinal(phase);
    if (ordinal < 0) {
        error = "unsupported compatibility lifecycle phase";
        setDiagnostic(CompatibilityActivationCode::WrongPhaseOrder,
                      materialization::RecipeResult::UnsupportedPhase, error);
        return false;
    }
    if (ordinal == completedPhase_) {
        setDiagnostic(CompatibilityActivationCode::AlreadySatisfied,
                      materialization::RecipeResult::AlreadyCurrent,
                      "compatibility lifecycle phase was already executed");
        return true;
    }
    if (ordinal != completedPhase_ + 1) {
        error = "compatibility lifecycle phases must execute PreSpawn -> Startup -> PostWindow -> Runtime";
        setDiagnostic(CompatibilityActivationCode::WrongPhaseOrder,
                      materialization::RecipeResult::WrongPhaseOrder, error);
        return false;
    }

    if (phase == setup::RecipeExecutionPhase::PreSpawn && !baselineState_) {
        if (!captureBaseline(error)) return false;
    }

    const auto result = session_.executePhase(phase);
    if (!result.succeeded()) {
        aborted_ = true;
        std::string verifyError;
        if (!verifyExpectedSafeState(verifyError)) {
            recoveryRequired_ = true;
            error = "compatibility materialization phase failed and rollback could not be verified: " +
                    result.message;
            if (!verifyError.empty()) error += "; verify-safe: " + verifyError;
            setDiagnostic(CompatibilityActivationCode::RecoveryRequired,
                          result.code, error);
            return false;
        }
        error = "compatibility materialization phase failed: " + result.message;
        setDiagnostic(CompatibilityActivationCode::MaterializationFailure,
                      result.code, error);
        return false;
    }

    completedPhase_ = ordinal;
    if (phase == setup::RecipeExecutionPhase::Runtime) {
        if (!session_.finalized()) {
            recoveryRequired_ = true;
            error = "compatibility materialization reached Runtime without a finalized transaction";
            setDiagnostic(CompatibilityActivationCode::RecoveryRequired,
                          materialization::RecipeResult::CommitFailed, error);
            return false;
        }
        finalized_ = true;
    }
    setDiagnostic(result.code == materialization::RecipeResult::AlreadyCurrent
                      ? CompatibilityActivationCode::AlreadySatisfied
                      : CompatibilityActivationCode::Ok,
                  result.code,
                  result.message);
    return true;
}

bool ProductionCompatibilityActivation::preSpawn(std::string& error) {
    return advance(setup::RecipeExecutionPhase::PreSpawn, error);
}

bool ProductionCompatibilityActivation::startup(std::string& error) {
    return advance(setup::RecipeExecutionPhase::Startup, error);
}

bool ProductionCompatibilityActivation::postWindow(std::string& error) {
    return advance(setup::RecipeExecutionPhase::PostWindow, error);
}

bool ProductionCompatibilityActivation::runtime(std::string& error) {
    return advance(setup::RecipeExecutionPhase::Runtime, error);
}

bool ProductionCompatibilityActivation::auxiliaryPathsAbsent(
    std::string& error) const noexcept {
    error.clear();
    try {
        for (const auto& path : {plan_.stagingRoot, plan_.rollbackRoot,
                                 plan_.previousPhaseRoot}) {
            std::error_code ec;
            const bool exists = std::filesystem::exists(path, ec);
            if (ec) {
                error = "failed to inspect compatibility transaction debris: " + ec.message();
                return false;
            }
            if (exists) {
                error = "compatibility transaction debris remains after rollback/cleanup";
                return false;
            }
        }
        return true;
    } catch (...) {
        error = "compatibility transaction debris verification failed unexpectedly";
        return false;
    }
}

bool ProductionCompatibilityActivation::verifyExpectedSafeState(
    std::string& error) const noexcept {
    error.clear();
    if (!baselineState_) {
        if (recoveryRequired_) {
            error = "compatibility activation has no verified pre-mutation baseline";
            return false;
        }
        return true;
    }
    if (!auxiliaryPathsAbsent(error)) return false;

    try {
        materialization::InstanceState current = materialization::InstanceState::Unsafe;
        const auto inspected = materialization::inspectInstanceMaterialization(plan_, current);
        if (cleanupAfterFinalize_) {
            if (current != materialization::InstanceState::Missing) {
                error = "finalized compatibility instance remains after Seat cleanup";
                if (!inspected.message.empty()) error += ": " + inspected.message;
                return false;
            }
            return true;
        }
        if (current != *baselineState_) {
            error = "compatibility rollback did not restore the exact pre-activation instance state";
            if (!inspected.message.empty()) error += ": " + inspected.message;
            return false;
        }
        if (current == materialization::InstanceState::Unsafe ||
            current == materialization::InstanceState::Partial) {
            error = "compatibility rollback reached an unsafe/partial instance state";
            return false;
        }
        return true;
    } catch (...) {
        error = "compatibility safe-state verification failed unexpectedly";
        return false;
    }
}

bool ProductionCompatibilityActivation::rollback(std::string& error) noexcept {
    error.clear();
    if (rolledBack_) return verifySafe(error);

    try {
        bool rollbackSucceeded = true;
        materialization::RecipeDiagnostic rollbackDiagnostic;

        if (finalized_) {
            rollbackDiagnostic = materialization::cleanupInstanceMaterialization(plan_);
            rollbackSucceeded = rollbackDiagnostic.succeeded();
            if (rollbackSucceeded) cleanupAfterFinalize_ = true;
        } else if (aborted_ || session_.failed()) {
            // The recipe engine already attempted reverse rollback. Recovery is a
            // bounded retry for interrupted rename/debris states before verification.
            rollbackDiagnostic = materialization::recoverInterruptedMaterialization(plan_);
            rollbackSucceeded = rollbackDiagnostic.succeeded();
        } else if (completedPhase_ >= 0) {
            rollbackDiagnostic = session_.rollback();
            rollbackSucceeded = rollbackDiagnostic.succeeded();
        }

        std::string verifyError;
        const bool verified = rollbackSucceeded && verifyExpectedSafeState(verifyError);
        if (!verified) {
            recoveryRequired_ = true;
            error = rollbackDiagnostic.message.empty()
                        ? "compatibility rollback could not be verified"
                        : rollbackDiagnostic.message;
            if (!verifyError.empty()) error += "; verify-safe: " + verifyError;
            setDiagnostic(CompatibilityActivationCode::RecoveryRequired,
                          rollbackDiagnostic.code, error);
            return false;
        }

        rolledBack_ = true;
        recoveryRequired_ = false;
        setDiagnostic(CompatibilityActivationCode::Ok,
                      materialization::RecipeResult::Success,
                      "compatibility lifecycle rollback/cleanup verified safe");
        return true;
    } catch (...) {
        recoveryRequired_ = true;
        error = "compatibility lifecycle rollback failed unexpectedly";
        setDiagnostic(CompatibilityActivationCode::RecoveryRequired,
                      materialization::RecipeResult::RollbackFailed, error);
        return false;
    }
}

bool ProductionCompatibilityActivation::verifySafe(std::string& error) noexcept {
    error.clear();
    if (recoveryRequired_) {
        error = "compatibility lifecycle retains recovery ownership";
        return false;
    }
    if (completedPhase_ >= 0 && !aborted_ && !rolledBack_) {
        error = "compatibility lifecycle is still active and has not been rolled back/cleaned";
        return false;
    }
    const bool safe = verifyExpectedSafeState(error);
    if (!safe) recoveryRequired_ = true;
    return safe;
}

void ProductionCompatibilityActivation::setDiagnostic(
    CompatibilityActivationCode code,
    materialization::RecipeResult recipeCode,
    std::string message) {
    diagnostic_ = {code, recipeCode, std::move(message)};
}

std::unique_ptr<launch::ISeatActivationLifecycleHook>
makeProductionCompatibilityActivation(
    materialization::InstanceMaterializationPlan plan,
    CompatibilityActivationIdentity identity,
    materialization::TransactionCheckpointHook checkpointHook) {
    return std::make_unique<ProductionCompatibilityActivation>(
        std::move(plan), std::move(identity), std::move(checkpointHook));
}

std::string_view compatibilityActivationCodeName(
    CompatibilityActivationCode code) noexcept {
    switch (code) {
        case CompatibilityActivationCode::Ok: return "Ok";
        case CompatibilityActivationCode::AlreadySatisfied: return "AlreadySatisfied";
        case CompatibilityActivationCode::InvalidIdentity: return "InvalidIdentity";
        case CompatibilityActivationCode::WrongSeat: return "WrongSeat";
        case CompatibilityActivationCode::WrongGame: return "WrongGame";
        case CompatibilityActivationCode::WrongProvider: return "WrongProvider";
        case CompatibilityActivationCode::StaleSetupDecision: return "StaleSetupDecision";
        case CompatibilityActivationCode::StaleSession: return "StaleSession";
        case CompatibilityActivationCode::StaleSeatGameGeneration: return "StaleSeatGameGeneration";
        case CompatibilityActivationCode::StaleActivationPlan: return "StaleActivationPlan";
        case CompatibilityActivationCode::StaleProviderRevision: return "StaleProviderRevision";
        case CompatibilityActivationCode::StaleRequirementRevision: return "StaleRequirementRevision";
        case CompatibilityActivationCode::StaleProviderPlan: return "StaleProviderPlan";
        case CompatibilityActivationCode::StaleSourceIdentity: return "StaleSourceIdentity";
        case CompatibilityActivationCode::StaleInstanceIdentity: return "StaleInstanceIdentity";
        case CompatibilityActivationCode::StaleRecipe: return "StaleRecipe";
        case CompatibilityActivationCode::MissingLifecycleBoundary: return "MissingLifecycleBoundary";
        case CompatibilityActivationCode::WrongPhaseOrder: return "WrongPhaseOrder";
        case CompatibilityActivationCode::MaterializationFailure: return "MaterializationFailure";
        case CompatibilityActivationCode::AlreadyFinalized: return "AlreadyFinalized";
        case CompatibilityActivationCode::RecoveryRequired: return "RecoveryRequired";
    }
    return "Unknown";
}

} // namespace hydra::production
