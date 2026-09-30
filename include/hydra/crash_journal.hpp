#pragma once

#include "hydra/watchdog_protocol.hpp"
#include "hydra/recovery_process_attachment.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::recovery {

using Hash256 = std::array<std::uint8_t, 32>;

inline constexpr std::uint32_t kCrashJournalMagic = 0x31524a48u; // "HJR1".
inline constexpr std::uint16_t kCrashJournalSchemaVersion = 1;
inline constexpr std::size_t kCrashJournalHeaderBytes = 24;
inline constexpr std::size_t kCrashJournalMaxRecords = 160;
inline constexpr std::size_t kCrashJournalMaxSnapshots = 16;
inline constexpr std::uint64_t kRecoveryProcessAttachmentSnapshotId =
    0x5250415454414348ull; // "RPATTACH" reserved identity binding.
inline constexpr std::size_t kCrashJournalMaxFileBytes = 8 * 1024;
inline constexpr std::size_t kCrashJournalHistoryDepth = 4;

inline constexpr std::uint32_t kSafeModeMarkerMagic = 0x314d5348u; // "HSM1".
inline constexpr std::uint16_t kSafeModeMarkerSchemaVersion = 1;
inline constexpr std::size_t kSafeModeMarkerFileBytes = 88;

enum class CrashJournalPhase : std::uint16_t {
    Preparing = 1,
    Applying = 2,
    Active = 3,
    RollingBack = 4,
    Clean = 5,
    RecoveryRequired = 6
};

enum class CrashJournalFinalResult : std::uint16_t {
    None = 0,
    Clean = 1,
    Failed = 2,
    RecoveryRequired = 3
};

enum class CrashJournalRecordKind : std::uint16_t {
    ActivationStarted = 1,
    ActionPrepared = 2,
    ActionApplied = 3,
    ActionVerified = 4,
    ActivationCommitted = 5,
    RollbackStarted = 6,
    ActionRolledBack = 7,
    RollbackVerified = 8,
    CleanStop = 9,
    FailureRecorded = 10,
    RecoveryRequired = 11
};

enum class SafeModeReason : std::uint16_t {
    IncompleteSession = 1,
    CorruptJournal = 2,
    RecoveryRequired = 3,
    JournalWriteFailure = 4,
    ManualRecovery = 5
};

enum class StartupRecoveryState : std::uint16_t {
    Clean = 1,
    RecoverableIncomplete = 2,
    RecoveryRequired = 3
};

struct CrashJournalRecord {
    std::uint64_t sequence{0};
    CrashJournalRecordKind kind{CrashJournalRecordKind::ActivationStarted};
    std::uint32_t actionId{0};
    std::uint64_t generation{0};

    bool operator==(const CrashJournalRecord&) const = default;
};

struct SnapshotReference {
    std::uint64_t snapshotId{0};
    Hash256 sha256{};
    std::uint64_t generation{0};

    bool operator==(const SnapshotReference&) const = default;
};

std::optional<SnapshotReference> makeRecoveryProcessAttachmentSnapshot(
    const RecoveryProcessAttachmentIdentity& identity,
    std::string* error = nullptr);

struct CrashJournalState {
    watchdog::SessionId sessionId{};
    Hash256 planHash{};
    std::uint64_t runtimeGeneration{0};
    watchdog::WatchdogLease lease{};
    CrashJournalPhase phase{CrashJournalPhase::Preparing};
    CrashJournalFinalResult finalResult{CrashJournalFinalResult::None};
    std::vector<CrashJournalRecord> records;
    std::vector<SnapshotReference> snapshots;

    bool operator==(const CrashJournalState&) const = default;
};

struct SafeModeMarker {
    watchdog::SessionId sessionId{};
    std::uint64_t runtimeGeneration{0};
    SafeModeReason reason{SafeModeReason::IncompleteSession};
    std::uint32_t diagnosticCode{0};
    Hash256 journalHash{};

    bool operator==(const SafeModeMarker&) const = default;
};

struct StartupAssessment {
    StartupRecoveryState state{StartupRecoveryState::RecoveryRequired};
    std::optional<CrashJournalState> journal;
    std::optional<SafeModeMarker> safeMode;
    bool safeModeWriteFailed{false};
    std::string diagnostic;
};

enum class JournalReadStatus : std::uint16_t {
    Success = 1,
    Missing = 2,
    TooLarge = 3,
    Failed = 4
};

struct JournalReadResult {
    JournalReadStatus status{JournalReadStatus::Failed};
    std::vector<std::byte> bytes;
    std::uint32_t systemError{0};
};

enum class JournalStorageSlot : std::uint16_t {
    Current = 1,
    CurrentTemp = 2,
    History0 = 3,
    History1 = 4,
    History2 = 5,
    History3 = 6,
    SafeMode = 7,
    SafeModeTemp = 8
};

// Storage operations are synchronous and may flush disk state. They must run on
// a recovery/runtime control path, never from Raw Input or other latency-sensitive
// callbacks. Implementations are not internally synchronized; callers serialize
// access to one recovery directory.
class CrashJournalStorage {
public:
    virtual ~CrashJournalStorage() = default;

    virtual JournalReadResult read(JournalStorageSlot slot,
                                   std::size_t maxBytes) = 0;
    virtual bool durableWrite(JournalStorageSlot slot,
                              std::span<const std::byte> bytes,
                              std::uint32_t* systemError = nullptr) = 0;
    virtual bool atomicReplace(JournalStorageSlot from,
                               JournalStorageSlot to,
                               std::uint32_t* systemError = nullptr) = 0;
    virtual bool remove(JournalStorageSlot slot,
                        std::uint32_t* systemError = nullptr) = 0;
};

// Production callers should use defaultCrashJournalDirectory(), which resolves to
// the current user's LocalAppData recovery area on Windows. Custom roots exist for
// deterministic tests and must provide equivalent same-user/admin access control.
class NativeCrashJournalStorage final : public CrashJournalStorage {
public:
    explicit NativeCrashJournalStorage(std::filesystem::path rootDirectory);

    JournalReadResult read(JournalStorageSlot slot,
                           std::size_t maxBytes) override;
    bool durableWrite(JournalStorageSlot slot,
                      std::span<const std::byte> bytes,
                      std::uint32_t* systemError = nullptr) override;
    bool atomicReplace(JournalStorageSlot from,
                       JournalStorageSlot to,
                       std::uint32_t* systemError = nullptr) override;
    bool remove(JournalStorageSlot slot,
                std::uint32_t* systemError = nullptr) override;

    const std::filesystem::path& rootDirectory() const noexcept {
        return m_rootDirectory;
    }

private:
    std::filesystem::path slotPath(JournalStorageSlot slot) const;
    bool ensureRoot(std::uint32_t* systemError);

    std::filesystem::path m_rootDirectory;
};

std::optional<std::filesystem::path> defaultCrashJournalDirectory(
    std::uint32_t* systemError = nullptr);

bool isZeroHash(const Hash256& hash) noexcept;
bool validateCrashJournalState(const CrashJournalState& state,
                               std::string* error = nullptr);
bool validateCrashJournalAgainstPlan(
    const CrashJournalState& state,
    const watchdog::RollbackPlanManifest& manifest,
    std::string* error = nullptr);
bool validateRecoveryProcessAttachmentJournalBinding(
    const CrashJournalState& state,
    const RecoveryProcessAttachmentIdentity& identity,
    std::string* error = nullptr);
// These 256-bit deterministic digests correlate persisted evidence with the
// canonical trusted plan/journal bytes. They are not an authentication primitive;
// recovery actions are still validated against the in-memory trusted manifest.
Hash256 hashRollbackPlanManifest(
    const watchdog::RollbackPlanManifest& manifest);

std::optional<CrashJournalState> makeInitialCrashJournal(
    const watchdog::RollbackPlanManifest& manifest,
    std::uint64_t runtimeGeneration,
    std::span<const SnapshotReference> snapshots,
    std::string* error = nullptr);

bool appendCrashJournalRecord(
    CrashJournalState& state,
    const watchdog::RollbackPlanManifest& manifest,
    CrashJournalRecordKind kind,
    std::uint32_t actionId,
    std::uint64_t generation,
    std::string* error = nullptr);

std::vector<std::byte> encodeCrashJournal(const CrashJournalState& state);
std::optional<CrashJournalState> decodeCrashJournal(
    std::span<const std::byte> bytes,
    std::string* error = nullptr);

std::vector<std::byte> encodeSafeModeMarker(const SafeModeMarker& marker);
std::optional<SafeModeMarker> decodeSafeModeMarker(
    std::span<const std::byte> bytes,
    std::string* error = nullptr);

Hash256 hashCrashJournalBytes(std::span<const std::byte> bytes) noexcept;

// CrashJournalStore is a single-writer control-plane object. It performs
// synchronous durable writes and is not thread-safe. beginActivation() and each
// persistTransition() call create one crash boundary; a transition may advance by
// at most one record so callers cannot accidentally skip a required durable marker.
// assessStartupAndEnterSafeMode() may persist a safe-mode marker. Reset helpers
// clear that marker only after the caller has independently verified rollback
// postconditions; the journal never executes recovery actions by itself.
class CrashJournalStore {
public:
    explicit CrashJournalStore(CrashJournalStorage& storage)
        : m_storage(storage) {}

    bool beginActivation(const CrashJournalState& initial,
                         std::string* error = nullptr);
    bool persistTransition(const CrashJournalState& state,
                           std::string* error = nullptr);

    StartupAssessment assessStartupAndEnterSafeMode();

    bool writeSafeMode(const SafeModeMarker& marker,
                       std::string* error = nullptr);
    bool replaceWithVerifiedCleanState(const CrashJournalState& cleanState,
                                       bool resetVerified,
                                       std::string* error = nullptr);
    bool clearSafeModeAfterVerifiedReset(
        const watchdog::SessionId& expectedSession,
        std::uint64_t expectedRuntimeGeneration,
        bool resetVerified,
        std::string* error = nullptr);

    std::optional<CrashJournalState> loadCurrent(
        std::string* error = nullptr) const;
    std::optional<SafeModeMarker> loadSafeMode(
        std::string* error = nullptr) const;

private:
    bool persistStateInternal(const CrashJournalState& state,
                              bool archiveCurrent,
                              std::string* error);
    bool archiveCurrentBestEffort(std::span<const std::byte> currentBytes);
    bool writeSafeModeForFailure(SafeModeReason reason,
                                 const CrashJournalState* state,
                                 std::uint32_t diagnosticCode);
    bool stateExtendsCurrent(const CrashJournalState& current,
                             const CrashJournalState& next,
                             std::string* error) const;

    CrashJournalStorage& m_storage;
};

std::string_view crashJournalPhaseName(CrashJournalPhase phase) noexcept;
std::string_view crashJournalRecordKindName(
    CrashJournalRecordKind kind) noexcept;
std::string_view startupRecoveryStateName(
    StartupRecoveryState state) noexcept;
std::string_view safeModeReasonName(SafeModeReason reason) noexcept;

} // namespace hydra::recovery
