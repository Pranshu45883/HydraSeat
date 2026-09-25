#pragma once

#include "hydra/watchdog_protocol.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace hydra::watchdog {

enum class RollbackActionResult : std::uint16_t {
    Success = 1,
    AlreadySatisfied = 2,
    InvalidAction = 3,
    IdentityMismatch = 4,
    Unsupported = 5,
    TimedOut = 6,
    Failed = 7
};

struct RollbackActionOutcome {
    std::uint32_t actionId{0};
    RollbackActionKind kind{RollbackActionKind::TerminateOwnedProcess};
    RollbackActionResult result{RollbackActionResult::Failed};
    std::uint32_t systemError{0};

    bool operator==(const RollbackActionOutcome&) const = default;
};

struct RollbackExecutionSummary {
    bool allSatisfied{false};
    bool recoveryRequired{false};
    std::uint32_t firstFailedActionId{0};
    std::uint32_t firstSystemError{0};
    std::vector<RollbackActionOutcome> outcomes;
};

class RollbackExecutor {
public:
    virtual ~RollbackExecutor() = default;

    // P8-RESET-01 may pre-acquire exact process objects before it stops the
    // runtime owner. This is non-mutating and optional: watchdog executors and
    // test doubles that do not need process leases keep the default no-op.
    virtual bool prepareOwnedProcesses(
        std::span<const RollbackActionDescriptor> actions,
        std::string* error = nullptr) {
        (void)actions;
        if (error != nullptr) error->clear();
        return true;
    }
    virtual void clearPreparedOwnedProcesses() noexcept {}

    virtual RollbackActionOutcome terminateOwnedProcess(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) = 0;
    virtual RollbackActionOutcome closeOwnedSession(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) = 0;
    virtual RollbackActionOutcome clearOptionalBackendState(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) = 0;
    virtual RollbackActionOutcome releaseOverlayState(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) = 0;
    virtual RollbackActionOutcome restoreSnapshotState(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) = 0;
    virtual RollbackActionOutcome writeSafeModeResult(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) = 0;
};

class RollbackRegistry {
public:
    bool registerPlan(const RollbackPlanManifest& manifest,
                      std::string* error = nullptr);

    bool armed() const noexcept { return m_manifest.has_value(); }
    const RollbackPlanManifest* manifest() const noexcept {
        return m_manifest ? &*m_manifest : nullptr;
    }

    RollbackExecutionSummary execute(RollbackExecutor& executor);
    void clear() noexcept;

private:
    RollbackActionOutcome executeOne(
        RollbackExecutor& executor,
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds);

    std::optional<RollbackPlanManifest> m_manifest;
    std::unordered_set<std::uint32_t> m_completedActionIds;
};

// P8-WATCH-01 ships only the process-termination OS action. Other allowlisted
// action kinds require a narrow subsystem implementation in the packet that
// owns that mutable state. Unsupported actions fail closed as RecoveryRequired.
class DefaultRollbackExecutor final : public RollbackExecutor {
public:
    DefaultRollbackExecutor() = default;
    ~DefaultRollbackExecutor() override;
    DefaultRollbackExecutor(const DefaultRollbackExecutor&) = delete;
    DefaultRollbackExecutor& operator=(const DefaultRollbackExecutor&) = delete;
    DefaultRollbackExecutor(DefaultRollbackExecutor&&) = delete;
    DefaultRollbackExecutor& operator=(DefaultRollbackExecutor&&) = delete;

    bool prepareOwnedProcesses(
        std::span<const RollbackActionDescriptor> actions,
        std::string* error = nullptr) override;
    void clearPreparedOwnedProcesses() noexcept override;

    RollbackActionOutcome terminateOwnedProcess(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;
    RollbackActionOutcome closeOwnedSession(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;
    RollbackActionOutcome clearOptionalBackendState(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;
    RollbackActionOutcome releaseOverlayState(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;
    RollbackActionOutcome restoreSnapshotState(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;
    RollbackActionOutcome writeSafeModeResult(
        const RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;

private:
    struct PreparedOwnedProcess {
        std::uint32_t actionId{0};
        ProcessIdentity process{};
        std::uintptr_t nativeHandle{0};
        bool alreadySatisfied{false};
    };

    std::vector<PreparedOwnedProcess> m_preparedOwnedProcesses;
};

bool queryProcessIdentity(std::uint32_t processId,
                          ProcessIdentity& identity,
                          std::uint32_t* systemError = nullptr) noexcept;

std::string_view rollbackActionResultName(RollbackActionResult result) noexcept;

} // namespace hydra::watchdog
