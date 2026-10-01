#pragma once

#include "hydra/hidhide_session_backend.hpp"
#include "hydra/rollback_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra {

inline constexpr std::uint32_t kHidHideSnapshotMagic = 0x31534848u; // "HHS1".
inline constexpr std::uint16_t kHidHideSnapshotVersion = 1u;
inline constexpr std::size_t kHidHideSnapshotMaxFileBytes = 256u * 1024u;

struct HidHideSessionRecoveryRecord {
    std::uint64_t resourceId{0};
    std::uint64_t generation{0};
    HidHideSessionSnapshot before;
    HidHideSessionSnapshot applied;

    bool operator==(const HidHideSessionRecoveryRecord&) const = default;
};

enum class HidHideSnapshotReadStatus : std::uint8_t {
    Success = 0,
    Missing = 1,
    TooLarge = 2,
    Corrupt = 3,
    Failed = 4,
};

struct HidHideSnapshotReadResult {
    HidHideSnapshotReadStatus status{HidHideSnapshotReadStatus::Failed};
    std::optional<HidHideSessionRecoveryRecord> record;
    std::uint32_t systemError{0};
    std::string diagnostic;
};

bool validateHidHideSessionRecoveryRecord(
    const HidHideSessionRecoveryRecord& record,
    std::string* error = nullptr);
std::vector<std::byte> encodeHidHideSessionRecoveryRecord(
    const HidHideSessionRecoveryRecord& record);
std::optional<HidHideSessionRecoveryRecord> decodeHidHideSessionRecoveryRecord(
    std::span<const std::byte> bytes,
    std::string* error = nullptr);
HidHideSessionRecoveryRecord makeHidHideSessionRecoveryRecord(
    const HidHideSessionPlan& plan,
    std::uint64_t resourceId);

// One bounded file per resource ID below the normal HydraSeat recovery root.
// Paths are derived only from numeric IDs; no persisted record can select an
// arbitrary file. Writes use temp + replace and are flushed before publication.
class HidHideSessionSnapshotStore {
public:
    explicit HidHideSessionSnapshotStore(std::filesystem::path recoveryRoot)
        : recoveryRoot_(std::move(recoveryRoot)) {}

    bool write(const HidHideSessionRecoveryRecord& record,
               std::string* error = nullptr);
    HidHideSnapshotReadResult load(std::uint64_t resourceId) const;
    bool remove(std::uint64_t resourceId, std::string* error = nullptr);

    const std::filesystem::path& recoveryRoot() const noexcept {
        return recoveryRoot_;
    }

private:
    std::filesystem::path directory() const;
    std::filesystem::path pathFor(std::uint64_t resourceId) const;
    std::filesystem::path tempPathFor(std::uint64_t resourceId) const;

    std::filesystem::path recoveryRoot_;
};

// Narrow executor used by watchdog/reset. It intercepts only
// RestoreSnapshotState actions whose resource ID resolves to a HydraSeat
// HidHide snapshot. Every other allowlisted action delegates to the existing
// DefaultRollbackExecutor.
// Orchestrates the safe ordering around HidHideSessionTransaction:
// prepare (read-only) -> persist recovery record -> expose rollback action ->
// caller confirms that exact action is armed -> activate. This prevents a UI or
// future host integration from mutating HidHide before durable recovery exists.
class GuardedHidHideSession {
public:
    GuardedHidHideSession(
        std::shared_ptr<HidHideSessionPlatform> platform,
        std::filesystem::path recoveryRoot,
        std::uint64_t resourceId,
        std::uint32_t actionId,
        std::uint32_t activationOrdinal,
        std::uint32_t rollbackTimeoutMilliseconds);
    ~GuardedHidHideSession();

    GuardedHidHideSession(const GuardedHidHideSession&) = delete;
    GuardedHidHideSession& operator=(const GuardedHidHideSession&) = delete;

    HidHideSessionResult prepare(HidHideSessionRequest request,
                                 std::uint64_t nowMilliseconds);
    bool confirmRecoveryArmed(
        const watchdog::RollbackActionDescriptor& registeredAction,
        std::string* error = nullptr);
    HidHideSessionResult activate(std::uint64_t nowMilliseconds);
    HidHideSessionResult expireIfNeeded(std::uint64_t nowMilliseconds);
    HidHideSessionResult rollback();

    const std::optional<watchdog::RollbackActionDescriptor>& rollbackAction() const noexcept {
        return rollbackAction_;
    }
    const std::optional<HidHideSessionPlan>& plan() const noexcept {
        return transaction_.plan();
    }
    bool recoveryArmed() const noexcept { return recoveryArmed_; }

private:
    HidHideSessionResult cleanupRecoveryRecordIfSafe(HidHideSessionResult result);

    HidHideSessionTransaction transaction_;
    HidHideSessionSnapshotStore store_;
    std::uint64_t resourceId_{0};
    std::uint32_t actionId_{0};
    std::uint32_t activationOrdinal_{0};
    std::uint32_t rollbackTimeoutMilliseconds_{0};
    std::optional<watchdog::RollbackActionDescriptor> rollbackAction_;
    bool recoveryArmed_{false};
};

class HidHideSessionRollbackExecutor final : public watchdog::RollbackExecutor {
public:
    HidHideSessionRollbackExecutor(
        std::filesystem::path recoveryRoot,
        std::shared_ptr<HidHideSessionPlatform> platform);

    bool prepareOwnedProcesses(
        std::span<const watchdog::RollbackActionDescriptor> actions,
        std::string* error = nullptr) override;
    void clearPreparedOwnedProcesses() noexcept override;

    watchdog::RollbackActionOutcome terminateOwnedProcess(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;
    watchdog::RollbackActionOutcome closeOwnedSession(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;
    watchdog::RollbackActionOutcome clearOptionalBackendState(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;
    watchdog::RollbackActionOutcome releaseOverlayState(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;
    watchdog::RollbackActionOutcome restoreSnapshotState(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;
    watchdog::RollbackActionOutcome writeSafeModeResult(
        const watchdog::RollbackActionDescriptor& action,
        std::uint32_t timeoutMilliseconds) override;

private:
    HidHideSessionSnapshotStore store_;
    std::shared_ptr<HidHideSessionPlatform> platform_;
    watchdog::DefaultRollbackExecutor delegate_;
};

std::string_view hidHideSnapshotReadStatusName(
    HidHideSnapshotReadStatus status) noexcept;

} // namespace hydra
