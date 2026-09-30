#include "hydra/gate_c_recovery.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace hydra::gatec {
namespace {

void setError(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

} // namespace

std::optional<watchdog::RollbackPlanManifest> makeGateCRecoveryPlan(
    const watchdog::SessionId& sessionId,
    std::uint64_t leaseGeneration,
    std::uint32_t leaseTimeoutMilliseconds,
    std::uint32_t rollbackTimeoutMilliseconds,
    std::span<const GateCRecoveryTarget> targets,
    std::string* error) {
    if (watchdog::isZeroSessionId(sessionId) || leaseGeneration == 0 ||
        targets.empty() || targets.size() > watchdog::kWatchdogMaxRollbackActions) {
        setError(error, "Gate C recovery plan identity/count is invalid");
        return std::nullopt;
    }

    watchdog::RollbackPlanManifest manifest;
    manifest.lease.sessionId = sessionId;
    manifest.lease.generation = leaseGeneration;
    manifest.lease.timeoutMilliseconds = leaseTimeoutMilliseconds;
    manifest.rollbackTimeoutMilliseconds = rollbackTimeoutMilliseconds;
    manifest.actions.reserve(targets.size());

    std::unordered_set<std::uint32_t> actionIds;
    std::unordered_set<std::uint32_t> ordinals;
    for (const auto& target : targets) {
        const bool duplicateProcess = std::any_of(
            manifest.actions.begin(), manifest.actions.end(),
            [&target](const auto& action) {
                return action.process == target.process;
            });
        if (target.actionId == 0 || target.activationOrdinal == 0 ||
            target.generation == 0 || target.process.processId == 0 ||
            target.process.creationTime100ns == 0 || duplicateProcess ||
            !actionIds.insert(target.actionId).second ||
            !ordinals.insert(target.activationOrdinal).second) {
            setError(error, "Gate C recovery target identity is invalid or duplicated");
            return std::nullopt;
        }
        watchdog::RollbackActionDescriptor action;
        action.actionId = target.actionId;
        action.kind = watchdog::RollbackActionKind::TerminateOwnedProcess;
        action.activationOrdinal = target.activationOrdinal;
        action.timeoutMilliseconds = 2'000;
        action.generation = target.generation;
        action.process = target.process;
        manifest.actions.push_back(action);
    }

    if (!watchdog::validateRollbackPlan(manifest, error)) {
        return std::nullopt;
    }
    return manifest;
}

GateCRecoveryJournal::GateCRecoveryJournal(
    recovery::CrashJournalStore& store,
    watchdog::RollbackPlanManifest manifest,
    std::uint64_t runtimeGeneration)
    : m_store(store),
      m_manifest(std::move(manifest)),
      m_runtimeGeneration(runtimeGeneration) {}

bool GateCRecoveryJournal::begin(
    std::span<const recovery::SnapshotReference> snapshots,
    std::string* error) {
    if (m_state.has_value()) {
        setError(error, "Gate C recovery journal is already active");
        return false;
    }
    const auto initial = recovery::makeInitialCrashJournal(
        m_manifest, m_runtimeGeneration, snapshots, error);
    if (!initial || !m_store.beginActivation(*initial, error)) {
        return false;
    }
    m_state = *initial;
    return true;
}

const watchdog::RollbackActionDescriptor* GateCRecoveryJournal::findAction(
    std::uint32_t actionId) const noexcept {
    const auto found = std::find_if(
        m_manifest.actions.begin(), m_manifest.actions.end(),
        [actionId](const auto& action) { return action.actionId == actionId; });
    return found == m_manifest.actions.end() ? nullptr : &*found;
}

bool GateCRecoveryJournal::appendAndPersist(
    recovery::CrashJournalRecordKind kind,
    std::uint32_t actionId,
    std::uint64_t generation,
    std::string* error) {
    if (!m_state) {
        setError(error, "Gate C recovery journal has not started");
        return false;
    }
    auto next = *m_state;
    if (!recovery::appendCrashJournalRecord(
            next, m_manifest, kind, actionId, generation, error) ||
        !m_store.persistTransition(next, error)) {
        return false;
    }
    m_state = std::move(next);
    return true;
}

bool GateCRecoveryJournal::prepareAction(std::uint32_t actionId,
                                         std::string* error) {
    const auto* action = findAction(actionId);
    if (action == nullptr) {
        setError(error, "Gate C recovery prepare references an unknown action");
        return false;
    }
    return appendAndPersist(recovery::CrashJournalRecordKind::ActionPrepared,
                            actionId, action->generation, error);
}

bool GateCRecoveryJournal::markActionApplied(std::uint32_t actionId,
                                             std::string* error) {
    const auto* action = findAction(actionId);
    if (action == nullptr) {
        setError(error, "Gate C recovery apply references an unknown action");
        return false;
    }
    return appendAndPersist(recovery::CrashJournalRecordKind::ActionApplied,
                            actionId, action->generation, error);
}

bool GateCRecoveryJournal::markActionVerified(std::uint32_t actionId,
                                              std::string* error) {
    const auto* action = findAction(actionId);
    if (action == nullptr) {
        setError(error, "Gate C recovery verify references an unknown action");
        return false;
    }
    return appendAndPersist(recovery::CrashJournalRecordKind::ActionVerified,
                            actionId, action->generation, error);
}

bool GateCRecoveryJournal::commitActivation(std::string* error) {
    return appendAndPersist(recovery::CrashJournalRecordKind::ActivationCommitted,
                            0, m_runtimeGeneration, error);
}

bool GateCRecoveryJournal::beginRollback(std::string* error) {
    return appendAndPersist(recovery::CrashJournalRecordKind::RollbackStarted,
                            0, m_runtimeGeneration, error);
}

bool GateCRecoveryJournal::markActionRolledBack(std::uint32_t actionId,
                                                std::string* error) {
    const auto* action = findAction(actionId);
    if (action == nullptr) {
        setError(error, "Gate C recovery rollback references an unknown action");
        return false;
    }
    return appendAndPersist(recovery::CrashJournalRecordKind::ActionRolledBack,
                            actionId, action->generation, error);
}

bool GateCRecoveryJournal::verifyRollback(std::string* error) {
    return appendAndPersist(recovery::CrashJournalRecordKind::RollbackVerified,
                            0, m_runtimeGeneration, error);
}

bool GateCRecoveryJournal::markCleanStop(std::string* error) {
    return appendAndPersist(recovery::CrashJournalRecordKind::CleanStop,
                            0, m_runtimeGeneration, error);
}

bool GateCRecoveryJournal::markRecoveryRequired(std::string* error) {
    return appendAndPersist(recovery::CrashJournalRecordKind::RecoveryRequired,
                            0, m_runtimeGeneration, error);
}

} // namespace hydra::gatec
