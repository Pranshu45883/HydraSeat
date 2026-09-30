#pragma once

#include "hydra/crash_journal.hpp"
#include "hydra/watchdog_protocol.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hydra::gatec {

// P3-REC-01 keeps Gate C rollback intentionally narrow: every mutable shim and
// adapter object is process-local, so the independent watchdog owns only exact
// controlled-process termination actions. The journal is evidence of those
// trusted actions; it never supplies commands or process identities by itself.
struct GateCRecoveryTarget {
    std::uint32_t actionId{0};
    std::uint32_t activationOrdinal{0};
    std::uint64_t generation{0};
    watchdog::ProcessIdentity process{};

    bool operator==(const GateCRecoveryTarget&) const = default;
};

std::optional<watchdog::RollbackPlanManifest> makeGateCRecoveryPlan(
    const watchdog::SessionId& sessionId,
    std::uint64_t leaseGeneration,
    std::uint32_t leaseTimeoutMilliseconds,
    std::uint32_t rollbackTimeoutMilliseconds,
    std::span<const GateCRecoveryTarget> targets,
    std::string* error = nullptr);

// Synchronous single-writer journal helper used only from the Gate C control
// path. It never runs on Raw Input/writer callbacks. Each method persists at
// most one journal record so a caller cannot skip a required crash boundary.
class GateCRecoveryJournal {
public:
    GateCRecoveryJournal(recovery::CrashJournalStore& store,
                         watchdog::RollbackPlanManifest manifest,
                         std::uint64_t runtimeGeneration);

    bool begin(std::span<const recovery::SnapshotReference> snapshots = {},
               std::string* error = nullptr);
    bool prepareAction(std::uint32_t actionId,
                       std::string* error = nullptr);
    bool markActionApplied(std::uint32_t actionId,
                           std::string* error = nullptr);
    bool markActionVerified(std::uint32_t actionId,
                            std::string* error = nullptr);
    bool commitActivation(std::string* error = nullptr);
    bool beginRollback(std::string* error = nullptr);
    bool markActionRolledBack(std::uint32_t actionId,
                              std::string* error = nullptr);
    bool verifyRollback(std::string* error = nullptr);
    bool markCleanStop(std::string* error = nullptr);
    bool markRecoveryRequired(std::string* error = nullptr);

    const watchdog::RollbackPlanManifest& manifest() const noexcept {
        return m_manifest;
    }
    const recovery::CrashJournalState* state() const noexcept {
        return m_state ? &*m_state : nullptr;
    }
    std::uint64_t runtimeGeneration() const noexcept {
        return m_runtimeGeneration;
    }

private:
    const watchdog::RollbackActionDescriptor* findAction(
        std::uint32_t actionId) const noexcept;
    bool appendAndPersist(recovery::CrashJournalRecordKind kind,
                          std::uint32_t actionId,
                          std::uint64_t generation,
                          std::string* error);

    recovery::CrashJournalStore& m_store;
    watchdog::RollbackPlanManifest m_manifest;
    std::uint64_t m_runtimeGeneration{0};
    std::optional<recovery::CrashJournalState> m_state;
};

} // namespace hydra::gatec
