#include "hydra/crash_journal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hydra::recovery {
namespace {

constexpr std::size_t kJournalPayloadPrefixBytes = 80;
constexpr std::size_t kJournalRecordBytes = 24;
constexpr std::size_t kSnapshotReferenceBytes = 48;
constexpr std::size_t kSafeModePayloadBytes = 64;

void setError(std::string* error, std::string_view value) {
    if (error != nullptr) {
        *error = value;
    }
}

bool knownPhase(CrashJournalPhase phase) noexcept {
    switch (phase) {
    case CrashJournalPhase::Preparing:
    case CrashJournalPhase::Applying:
    case CrashJournalPhase::Active:
    case CrashJournalPhase::RollingBack:
    case CrashJournalPhase::Clean:
    case CrashJournalPhase::RecoveryRequired:
        return true;
    }
    return false;
}

bool knownFinalResult(CrashJournalFinalResult result) noexcept {
    switch (result) {
    case CrashJournalFinalResult::None:
    case CrashJournalFinalResult::Clean:
    case CrashJournalFinalResult::Failed:
    case CrashJournalFinalResult::RecoveryRequired:
        return true;
    }
    return false;
}

bool knownRecordKind(CrashJournalRecordKind kind) noexcept {
    switch (kind) {
    case CrashJournalRecordKind::ActivationStarted:
    case CrashJournalRecordKind::ActionPrepared:
    case CrashJournalRecordKind::ActionApplied:
    case CrashJournalRecordKind::ActionVerified:
    case CrashJournalRecordKind::ActivationCommitted:
    case CrashJournalRecordKind::RollbackStarted:
    case CrashJournalRecordKind::ActionRolledBack:
    case CrashJournalRecordKind::RollbackVerified:
    case CrashJournalRecordKind::CleanStop:
    case CrashJournalRecordKind::FailureRecorded:
    case CrashJournalRecordKind::RecoveryRequired:
        return true;
    }
    return false;
}

bool knownSafeModeReason(SafeModeReason reason) noexcept {
    switch (reason) {
    case SafeModeReason::IncompleteSession:
    case SafeModeReason::CorruptJournal:
    case SafeModeReason::RecoveryRequired:
    case SafeModeReason::JournalWriteFailure:
    case SafeModeReason::ManualRecovery:
        return true;
    }
    return false;
}

bool actionRecord(CrashJournalRecordKind kind) noexcept {
    return kind == CrashJournalRecordKind::ActionPrepared ||
           kind == CrashJournalRecordKind::ActionApplied ||
           kind == CrashJournalRecordKind::ActionVerified ||
           kind == CrashJournalRecordKind::ActionRolledBack;
}

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffu;
    for (const auto value : bytes) {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(0u - (crc & 1u));
            crc = (crc >> 1u) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

class ByteWriter {
public:
    explicit ByteWriter(std::size_t reserveBytes) { m_bytes.reserve(reserveBytes); }

    void u8(std::uint8_t value) {
        m_bytes.push_back(static_cast<std::byte>(value));
    }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value & 0xffu));
        u8(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    template <std::size_t Size>
    void bytes(const std::array<std::uint8_t, Size>& value) {
        for (const auto byte : value) u8(byte);
    }
    void raw(std::span<const std::byte> bytes) {
        m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
    }
    std::vector<std::byte> take() { return std::move(m_bytes); }

private:
    std::vector<std::byte> m_bytes;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> bytes) : m_bytes(bytes) {}

    bool u8(std::uint8_t& value) {
        if (m_offset >= m_bytes.size()) return false;
        value = std::to_integer<std::uint8_t>(m_bytes[m_offset++]);
        return true;
    }
    bool u16(std::uint16_t& value) {
        std::uint8_t b0 = 0;
        std::uint8_t b1 = 0;
        if (!u8(b0) || !u8(b1)) return false;
        value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(b0) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(b1) << 8u));
        return true;
    }
    bool u32(std::uint32_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool u64(std::uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }
    template <std::size_t Size>
    bool bytes(std::array<std::uint8_t, Size>& value) {
        for (auto& byte : value) {
            if (!u8(byte)) return false;
        }
        return true;
    }
    bool empty() const noexcept { return m_offset == m_bytes.size(); }

private:
    std::span<const std::byte> m_bytes;
    std::size_t m_offset{0};
};

struct DerivedState {
    CrashJournalPhase phase{CrashJournalPhase::Preparing};
    CrashJournalFinalResult finalResult{CrashJournalFinalResult::None};
};

enum class ActionProgress : std::uint8_t {
    Prepared = 1,
    Applied = 2,
    Verified = 3,
    RolledBack = 4
};

bool deriveStateFromRecords(const std::vector<CrashJournalRecord>& records,
                            DerivedState& derived,
                            std::string* error) {
    if (records.empty() || records.size() > kCrashJournalMaxRecords) {
        setError(error, "crash journal record count is out of range");
        return false;
    }

    std::unordered_map<std::uint32_t, ActionProgress> actions;
    bool rollbackVerified = false;
    derived = {};

    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        if (record.sequence != index + 1 || record.generation == 0 ||
            !knownRecordKind(record.kind)) {
            setError(error, "crash journal record identity is invalid");
            return false;
        }
        if (actionRecord(record.kind) != (record.actionId != 0)) {
            setError(error, "crash journal action marker shape is invalid");
            return false;
        }
        if (derived.phase == CrashJournalPhase::Clean ||
            derived.phase == CrashJournalPhase::RecoveryRequired) {
            setError(error, "crash journal contains records after terminal state");
            return false;
        }

        switch (record.kind) {
        case CrashJournalRecordKind::ActivationStarted:
            if (index != 0 || record.actionId != 0) {
                setError(error, "activation-started must be the first record");
                return false;
            }
            derived.phase = CrashJournalPhase::Preparing;
            break;
        case CrashJournalRecordKind::ActionPrepared:
            if (derived.phase != CrashJournalPhase::Preparing &&
                derived.phase != CrashJournalPhase::Applying) {
                setError(error, "action-prepared is outside activation");
                return false;
            }
            if (actions.contains(record.actionId)) {
                setError(error, "action-prepared is duplicated");
                return false;
            }
            actions.emplace(record.actionId, ActionProgress::Prepared);
            break;
        case CrashJournalRecordKind::ActionApplied: {
            if (derived.phase != CrashJournalPhase::Preparing &&
                derived.phase != CrashJournalPhase::Applying) {
                setError(error, "action-applied is outside activation");
                return false;
            }
            const auto found = actions.find(record.actionId);
            if (found == actions.end() || found->second != ActionProgress::Prepared) {
                setError(error, "action-applied requires exactly one prepare marker");
                return false;
            }
            found->second = ActionProgress::Applied;
            derived.phase = CrashJournalPhase::Applying;
            break;
        }
        case CrashJournalRecordKind::ActionVerified: {
            if (derived.phase != CrashJournalPhase::Applying) {
                setError(error, "action-verified is outside applying state");
                return false;
            }
            const auto found = actions.find(record.actionId);
            if (found == actions.end() || found->second != ActionProgress::Applied) {
                setError(error, "action-verified requires an applied action");
                return false;
            }
            found->second = ActionProgress::Verified;
            break;
        }
        case CrashJournalRecordKind::ActivationCommitted:
            if (derived.phase != CrashJournalPhase::Preparing &&
                derived.phase != CrashJournalPhase::Applying) {
                setError(error, "activation-committed is outside activation");
                return false;
            }
            for (const auto& [actionId, progress] : actions) {
                (void)actionId;
                if (progress != ActionProgress::Verified) {
                    setError(error, "activation commit requires all prepared actions verified");
                    return false;
                }
            }
            derived.phase = CrashJournalPhase::Active;
            break;
        case CrashJournalRecordKind::RollbackStarted:
            if (derived.phase != CrashJournalPhase::Preparing &&
                derived.phase != CrashJournalPhase::Applying &&
                derived.phase != CrashJournalPhase::Active) {
                setError(error, "rollback-started is outside a recoverable state");
                return false;
            }
            derived.phase = CrashJournalPhase::RollingBack;
            rollbackVerified = false;
            break;
        case CrashJournalRecordKind::ActionRolledBack: {
            if (derived.phase != CrashJournalPhase::RollingBack) {
                setError(error, "action-rolled-back is outside rollback");
                return false;
            }
            const auto found = actions.find(record.actionId);
            if (found == actions.end() || found->second == ActionProgress::RolledBack) {
                setError(error, "action-rolled-back requires an outstanding prepared action");
                return false;
            }
            found->second = ActionProgress::RolledBack;
            break;
        }
        case CrashJournalRecordKind::RollbackVerified:
            if (derived.phase != CrashJournalPhase::RollingBack) {
                setError(error, "rollback-verified is outside rollback");
                return false;
            }
            for (const auto& [actionId, progress] : actions) {
                (void)actionId;
                if (progress != ActionProgress::RolledBack) {
                    setError(error, "rollback verification requires every prepared action restored");
                    return false;
                }
            }
            rollbackVerified = true;
            break;
        case CrashJournalRecordKind::CleanStop:
            if (derived.phase != CrashJournalPhase::RollingBack ||
                !rollbackVerified) {
                setError(error, "clean-stop requires verified rollback");
                return false;
            }
            derived.phase = CrashJournalPhase::Clean;
            derived.finalResult = CrashJournalFinalResult::Clean;
            break;
        case CrashJournalRecordKind::FailureRecorded:
            derived.phase = CrashJournalPhase::RecoveryRequired;
            derived.finalResult = CrashJournalFinalResult::Failed;
            break;
        case CrashJournalRecordKind::RecoveryRequired:
            derived.phase = CrashJournalPhase::RecoveryRequired;
            derived.finalResult = CrashJournalFinalResult::RecoveryRequired;
            break;
        }
    }
    return true;
}

const watchdog::RollbackActionDescriptor* findAction(
    const watchdog::RollbackPlanManifest& manifest,
    std::uint32_t actionId) noexcept {
    const auto found = std::find_if(
        manifest.actions.begin(), manifest.actions.end(),
        [actionId](const auto& action) { return action.actionId == actionId; });
    return found == manifest.actions.end() ? nullptr : &*found;
}

bool validSnapshot(const SnapshotReference& snapshot) noexcept {
    return snapshot.snapshotId != 0 && snapshot.generation != 0 &&
           !isZeroHash(snapshot.sha256);
}

std::vector<std::byte> encodeHeader(std::uint32_t magic,
                                    std::uint16_t version,
                                    std::span<const std::byte> payload) {
    ByteWriter writer(kCrashJournalHeaderBytes);
    writer.u32(magic);
    writer.u16(version);
    writer.u16(static_cast<std::uint16_t>(kCrashJournalHeaderBytes));
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.u32(crc32(payload));
    writer.u64(0);
    return writer.take();
}

bool decodeHeader(std::span<const std::byte> bytes,
                  std::uint32_t expectedMagic,
                  std::uint16_t expectedVersion,
                  std::span<const std::byte>& payload,
                  std::string* error) {
    if (bytes.size() < kCrashJournalHeaderBytes) {
        setError(error, "journal header is truncated");
        return false;
    }
    ByteReader reader(bytes.first(kCrashJournalHeaderBytes));
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t headerBytes = 0;
    std::uint32_t payloadBytes = 0;
    std::uint32_t expectedCrc = 0;
    std::uint64_t reserved = 0;
    if (!reader.u32(magic) || !reader.u16(version) ||
        !reader.u16(headerBytes) || !reader.u32(payloadBytes) ||
        !reader.u32(expectedCrc) || !reader.u64(reserved) || !reader.empty()) {
        setError(error, "journal header cannot be decoded");
        return false;
    }
    if (magic != expectedMagic) {
        setError(error, "journal magic mismatch");
        return false;
    }
    if (version != expectedVersion) {
        setError(error, "journal schema version mismatch");
        return false;
    }
    if (headerBytes != kCrashJournalHeaderBytes || reserved != 0) {
        setError(error, "journal header reserved/size fields are invalid");
        return false;
    }
    if (payloadBytes != bytes.size() - kCrashJournalHeaderBytes) {
        setError(error, "journal payload length mismatch");
        return false;
    }
    payload = bytes.subspan(kCrashJournalHeaderBytes);
    if (crc32(payload) != expectedCrc) {
        setError(error, "journal checksum mismatch");
        return false;
    }
    return true;
}

std::filesystem::path slotFileName(JournalStorageSlot slot) {
    switch (slot) {
    case JournalStorageSlot::Current:
        return L"crash-journal-v1.bin";
    case JournalStorageSlot::CurrentTemp:
        return L".crash-journal-v1.tmp";
    case JournalStorageSlot::History0:
        return L"crash-journal-v1.history.0.bin";
    case JournalStorageSlot::History1:
        return L"crash-journal-v1.history.1.bin";
    case JournalStorageSlot::History2:
        return L"crash-journal-v1.history.2.bin";
    case JournalStorageSlot::History3:
        return L"crash-journal-v1.history.3.bin";
    case JournalStorageSlot::SafeMode:
        return L"safe-mode-v1.bin";
    case JournalStorageSlot::SafeModeTemp:
        return L".safe-mode-v1.tmp";
    }
    return L"invalid-slot";
}

JournalStorageSlot historySlot(std::size_t index) {
    switch (index) {
    case 0: return JournalStorageSlot::History0;
    case 1: return JournalStorageSlot::History1;
    case 2: return JournalStorageSlot::History2;
    default: return JournalStorageSlot::History3;
    }
}

#if defined(_WIN32)
class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : m_handle(handle) {}
    ~ScopedHandle() {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const noexcept { return m_handle; }
private:
    HANDLE m_handle;
};
#endif

} // namespace

bool isZeroHash(const Hash256& hash) noexcept {
    return std::all_of(hash.begin(), hash.end(),
                       [](std::uint8_t byte) { return byte == 0; });
}

std::optional<SnapshotReference> makeRecoveryProcessAttachmentSnapshot(
    const RecoveryProcessAttachmentIdentity& identity,
    std::string* error) {
    const auto canonical = encodeRecoveryProcessAttachmentIdentity(identity);
    if (canonical.size() != kRecoveryProcessAttachmentIdentityBytes) {
        setError(error, "recovery attachment identity cannot be encoded for journal binding");
        return std::nullopt;
    }
    const auto digest = hashCrashJournalBytes(canonical);
    if (isZeroHash(digest)) {
        setError(error, "recovery attachment journal binding hash is zero");
        return std::nullopt;
    }
    SnapshotReference snapshot;
    snapshot.snapshotId = kRecoveryProcessAttachmentSnapshotId;
    snapshot.sha256 = digest;
    snapshot.generation = identity.recoveryEpoch;
    if (error != nullptr) error->clear();
    return snapshot;
}

bool validateCrashJournalState(const CrashJournalState& state,
                               std::string* error) {
    if (watchdog::isZeroSessionId(state.sessionId) ||
        isZeroHash(state.planHash) || state.runtimeGeneration == 0) {
        setError(error, "crash journal top-level identity is invalid");
        return false;
    }
    if (state.lease.sessionId != state.sessionId ||
        state.lease.generation == 0 ||
        state.lease.timeoutMilliseconds < watchdog::kWatchdogMinLeaseTimeoutMs ||
        state.lease.timeoutMilliseconds > watchdog::kWatchdogMaxLeaseTimeoutMs) {
        setError(error, "crash journal lease identity is invalid");
        return false;
    }
    if (!knownPhase(state.phase) || !knownFinalResult(state.finalResult) ||
        state.snapshots.size() > kCrashJournalMaxSnapshots) {
        setError(error, "crash journal phase/result/snapshot count is invalid");
        return false;
    }

    std::unordered_set<std::uint64_t> snapshotIds;
    for (const auto& snapshot : state.snapshots) {
        if (!validSnapshot(snapshot) ||
            !snapshotIds.insert(snapshot.snapshotId).second) {
            setError(error, "crash journal snapshot identity is invalid or duplicated");
            return false;
        }
    }

    DerivedState derived;
    if (!deriveStateFromRecords(state.records, derived, error)) return false;
    if (derived.phase != state.phase || derived.finalResult != state.finalResult) {
        setError(error, "crash journal stored state does not match record replay");
        return false;
    }
    return true;
}

bool validateCrashJournalAgainstPlan(
    const CrashJournalState& state,
    const watchdog::RollbackPlanManifest& manifest,
    std::string* error) {
    if (!validateCrashJournalState(state, error)) return false;
    if (!watchdog::validateRollbackPlan(manifest, error)) return false;
    const auto expectedPlanHash = hashRollbackPlanManifest(manifest);
    if (isZeroHash(expectedPlanHash) || state.planHash != expectedPlanHash) {
        setError(error, "crash journal plan hash does not match rollback manifest");
        return false;
    }
    if (state.sessionId != manifest.lease.sessionId ||
        state.lease != manifest.lease) {
        setError(error, "crash journal lease/session does not match rollback plan");
        return false;
    }
    std::optional<std::uint32_t> previousRollbackOrdinal;
    for (const auto& record : state.records) {
        if (!actionRecord(record.kind)) continue;
        const auto* action = findAction(manifest, record.actionId);
        if (action == nullptr || action->generation != record.generation) {
            setError(error, "crash journal action marker is not present in rollback plan");
            return false;
        }
        if (record.kind == CrashJournalRecordKind::ActionRolledBack) {
            if (previousRollbackOrdinal.has_value() &&
                action->activationOrdinal >= *previousRollbackOrdinal) {
                setError(error, "crash journal rollback markers are not in reverse activation order");
                return false;
            }
            previousRollbackOrdinal = action->activationOrdinal;
        }
    }
    return true;
}

bool validateRecoveryProcessAttachmentJournalBinding(
    const CrashJournalState& state,
    const RecoveryProcessAttachmentIdentity& identity,
    std::string* error) {
    if (!validateCrashJournalState(state, error)) return false;
    const auto expected = makeRecoveryProcessAttachmentSnapshot(identity, error);
    if (!expected) return false;
    const auto found = std::find_if(
        state.snapshots.begin(), state.snapshots.end(), [](const auto& snapshot) {
            return snapshot.snapshotId == kRecoveryProcessAttachmentSnapshotId;
        });
    if (found == state.snapshots.end()) {
        setError(error,
                 "crash journal has no exact recovery-process attachment binding");
        return false;
    }
    if (*found != *expected) {
        setError(error,
                 "crash journal recovery-process attachment binding does not match the exact epoch");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

Hash256 hashRollbackPlanManifest(
    const watchdog::RollbackPlanManifest& manifest) {
    const auto canonical = watchdog::encodeRegisterPlan(1, manifest);
    if (canonical.empty()) return {};
    return hashCrashJournalBytes(canonical);
}

std::optional<CrashJournalState> makeInitialCrashJournal(
    const watchdog::RollbackPlanManifest& manifest,
    std::uint64_t runtimeGeneration,
    std::span<const SnapshotReference> snapshots,
    std::string* error) {
    if (!watchdog::validateRollbackPlan(manifest, error) ||
        runtimeGeneration == 0 ||
        snapshots.size() > kCrashJournalMaxSnapshots) {
        setError(error, "initial crash journal arguments are invalid");
        return std::nullopt;
    }
    const auto planHash = hashRollbackPlanManifest(manifest);
    if (isZeroHash(planHash)) {
        setError(error, "rollback plan hash cannot be computed");
        return std::nullopt;
    }

    CrashJournalState state;
    state.sessionId = manifest.lease.sessionId;
    state.planHash = planHash;
    state.runtimeGeneration = runtimeGeneration;
    state.lease = manifest.lease;
    state.phase = CrashJournalPhase::Preparing;
    state.finalResult = CrashJournalFinalResult::None;
    state.snapshots.assign(snapshots.begin(), snapshots.end());
    state.records.push_back({1, CrashJournalRecordKind::ActivationStarted,
                             0, runtimeGeneration});
    if (!validateCrashJournalAgainstPlan(state, manifest, error)) {
        return std::nullopt;
    }
    return state;
}

bool appendCrashJournalRecord(
    CrashJournalState& state,
    const watchdog::RollbackPlanManifest& manifest,
    CrashJournalRecordKind kind,
    std::uint32_t actionId,
    std::uint64_t generation,
    std::string* error) {
    if (!knownRecordKind(kind) || generation == 0 ||
        state.records.size() >= kCrashJournalMaxRecords) {
        setError(error, "crash journal append arguments are invalid");
        return false;
    }
    if (actionRecord(kind)) {
        const auto* action = findAction(manifest, actionId);
        if (action == nullptr || action->generation != generation) {
            setError(error, "crash journal action marker does not match rollback plan");
            return false;
        }
    } else if (actionId != 0 || generation != state.runtimeGeneration) {
        setError(error, "crash journal session marker identity is invalid");
        return false;
    }

    CrashJournalState candidate = state;
    candidate.records.push_back({
        static_cast<std::uint64_t>(candidate.records.size() + 1),
        kind, actionId, generation});
    DerivedState derived;
    if (!deriveStateFromRecords(candidate.records, derived, error)) return false;
    candidate.phase = derived.phase;
    candidate.finalResult = derived.finalResult;
    if (!validateCrashJournalAgainstPlan(candidate, manifest, error)) return false;
    state = std::move(candidate);
    return true;
}

std::vector<std::byte> encodeCrashJournal(const CrashJournalState& state) {
    if (!validateCrashJournalState(state)) return {};
    const std::size_t payloadBytes = kJournalPayloadPrefixBytes +
        state.records.size() * kJournalRecordBytes +
        state.snapshots.size() * kSnapshotReferenceBytes;
    if (payloadBytes + kCrashJournalHeaderBytes > kCrashJournalMaxFileBytes) {
        return {};
    }

    ByteWriter payloadWriter(payloadBytes);
    payloadWriter.bytes(state.sessionId);
    payloadWriter.bytes(state.planHash);
    payloadWriter.u64(state.runtimeGeneration);
    payloadWriter.u64(state.lease.generation);
    payloadWriter.u32(state.lease.timeoutMilliseconds);
    payloadWriter.u16(static_cast<std::uint16_t>(state.phase));
    payloadWriter.u16(static_cast<std::uint16_t>(state.finalResult));
    payloadWriter.u16(static_cast<std::uint16_t>(state.records.size()));
    payloadWriter.u16(static_cast<std::uint16_t>(state.snapshots.size()));
    payloadWriter.u32(0);
    for (const auto& record : state.records) {
        payloadWriter.u64(record.sequence);
        payloadWriter.u16(static_cast<std::uint16_t>(record.kind));
        payloadWriter.u16(0);
        payloadWriter.u32(record.actionId);
        payloadWriter.u64(record.generation);
    }
    for (const auto& snapshot : state.snapshots) {
        payloadWriter.u64(snapshot.snapshotId);
        payloadWriter.bytes(snapshot.sha256);
        payloadWriter.u64(snapshot.generation);
    }
    auto payload = payloadWriter.take();
    auto header = encodeHeader(kCrashJournalMagic,
                               kCrashJournalSchemaVersion, payload);
    header.insert(header.end(), payload.begin(), payload.end());
    return header;
}

std::optional<CrashJournalState> decodeCrashJournal(
    std::span<const std::byte> bytes,
    std::string* error) {
    if (bytes.size() > kCrashJournalMaxFileBytes) {
        setError(error, "crash journal exceeds file-size bound");
        return std::nullopt;
    }
    std::span<const std::byte> payload;
    if (!decodeHeader(bytes, kCrashJournalMagic,
                      kCrashJournalSchemaVersion, payload, error)) {
        return std::nullopt;
    }
    if (payload.size() < kJournalPayloadPrefixBytes) {
        setError(error, "crash journal payload is truncated");
        return std::nullopt;
    }

    ByteReader reader(payload);
    CrashJournalState state;
    std::uint16_t phaseValue = 0;
    std::uint16_t resultValue = 0;
    std::uint16_t recordCount = 0;
    std::uint16_t snapshotCount = 0;
    std::uint32_t reserved = 0;
    if (!reader.bytes(state.sessionId) || !reader.bytes(state.planHash) ||
        !reader.u64(state.runtimeGeneration) ||
        !reader.u64(state.lease.generation) ||
        !reader.u32(state.lease.timeoutMilliseconds) ||
        !reader.u16(phaseValue) || !reader.u16(resultValue) ||
        !reader.u16(recordCount) || !reader.u16(snapshotCount) ||
        !reader.u32(reserved)) {
        setError(error, "crash journal prefix cannot be decoded");
        return std::nullopt;
    }
    state.lease.sessionId = state.sessionId;
    state.phase = static_cast<CrashJournalPhase>(phaseValue);
    state.finalResult = static_cast<CrashJournalFinalResult>(resultValue);
    if (reserved != 0 || recordCount == 0 ||
        recordCount > kCrashJournalMaxRecords ||
        snapshotCount > kCrashJournalMaxSnapshots) {
        setError(error, "crash journal count/reserved fields are invalid");
        return std::nullopt;
    }
    const std::size_t expectedPayload = kJournalPayloadPrefixBytes +
        static_cast<std::size_t>(recordCount) * kJournalRecordBytes +
        static_cast<std::size_t>(snapshotCount) * kSnapshotReferenceBytes;
    if (payload.size() != expectedPayload) {
        setError(error, "crash journal payload size does not match counts");
        return std::nullopt;
    }

    state.records.reserve(recordCount);
    for (std::uint16_t index = 0; index < recordCount; ++index) {
        CrashJournalRecord record;
        std::uint16_t kindValue = 0;
        std::uint16_t recordReserved = 0;
        if (!reader.u64(record.sequence) || !reader.u16(kindValue) ||
            !reader.u16(recordReserved) || !reader.u32(record.actionId) ||
            !reader.u64(record.generation) || recordReserved != 0) {
            setError(error, "crash journal record cannot be decoded");
            return std::nullopt;
        }
        record.kind = static_cast<CrashJournalRecordKind>(kindValue);
        state.records.push_back(record);
    }
    state.snapshots.reserve(snapshotCount);
    for (std::uint16_t index = 0; index < snapshotCount; ++index) {
        SnapshotReference snapshot;
        if (!reader.u64(snapshot.snapshotId) ||
            !reader.bytes(snapshot.sha256) ||
            !reader.u64(snapshot.generation)) {
            setError(error, "crash journal snapshot cannot be decoded");
            return std::nullopt;
        }
        state.snapshots.push_back(snapshot);
    }
    if (!reader.empty() || !validateCrashJournalState(state, error)) {
        if (error != nullptr && error->empty()) {
            *error = "crash journal has trailing bytes";
        }
        return std::nullopt;
    }
    return state;
}

std::vector<std::byte> encodeSafeModeMarker(const SafeModeMarker& marker) {
    if (!knownSafeModeReason(marker.reason)) return {};
    const bool hasIdentity = !watchdog::isZeroSessionId(marker.sessionId);
    if (hasIdentity != (marker.runtimeGeneration != 0)) return {};

    ByteWriter payloadWriter(kSafeModePayloadBytes);
    payloadWriter.bytes(marker.sessionId);
    payloadWriter.u64(marker.runtimeGeneration);
    payloadWriter.u16(static_cast<std::uint16_t>(marker.reason));
    payloadWriter.u16(0);
    payloadWriter.u32(marker.diagnosticCode);
    payloadWriter.bytes(marker.journalHash);
    auto payload = payloadWriter.take();
    auto header = encodeHeader(kSafeModeMarkerMagic,
                               kSafeModeMarkerSchemaVersion, payload);
    header.insert(header.end(), payload.begin(), payload.end());
    return header;
}

std::optional<SafeModeMarker> decodeSafeModeMarker(
    std::span<const std::byte> bytes,
    std::string* error) {
    if (bytes.size() != kSafeModeMarkerFileBytes) {
        setError(error, "safe-mode marker size mismatch");
        return std::nullopt;
    }
    std::span<const std::byte> payload;
    if (!decodeHeader(bytes, kSafeModeMarkerMagic,
                      kSafeModeMarkerSchemaVersion, payload, error) ||
        payload.size() != kSafeModePayloadBytes) {
        return std::nullopt;
    }
    ByteReader reader(payload);
    SafeModeMarker marker;
    std::uint16_t reasonValue = 0;
    std::uint16_t reserved = 0;
    if (!reader.bytes(marker.sessionId) ||
        !reader.u64(marker.runtimeGeneration) ||
        !reader.u16(reasonValue) || !reader.u16(reserved) ||
        !reader.u32(marker.diagnosticCode) ||
        !reader.bytes(marker.journalHash) || !reader.empty()) {
        setError(error, "safe-mode marker cannot be decoded");
        return std::nullopt;
    }
    marker.reason = static_cast<SafeModeReason>(reasonValue);
    const bool hasIdentity = !watchdog::isZeroSessionId(marker.sessionId);
    if (reserved != 0 || !knownSafeModeReason(marker.reason) ||
        hasIdentity != (marker.runtimeGeneration != 0)) {
        setError(error, "safe-mode marker fields are invalid");
        return std::nullopt;
    }
    return marker;
}

Hash256 hashCrashJournalBytes(std::span<const std::byte> bytes) noexcept {
    constexpr std::array<std::uint64_t, 4> seeds{
        0xcbf29ce484222325ull,
        0x84222325cbf29ce4ull,
        0x9e3779b185ebca87ull,
        0x517cc1b727220a95ull};
    std::array<std::uint64_t, 4> values = seeds;
    for (const auto byteValue : bytes) {
        const auto byte = std::to_integer<std::uint8_t>(byteValue);
        for (std::size_t index = 0; index < values.size(); ++index) {
            values[index] ^= static_cast<std::uint64_t>(byte) +
                static_cast<std::uint64_t>(index * 17u);
            values[index] *= 0x100000001b3ull;
            values[index] ^= values[index] >> 32u;
        }
    }
    Hash256 result{};
    for (std::size_t word = 0; word < values.size(); ++word) {
        for (std::size_t byte = 0; byte < 8; ++byte) {
            result[word * 8 + byte] = static_cast<std::uint8_t>(
                (values[word] >> (byte * 8)) & 0xffu);
        }
    }
    return result;
}

NativeCrashJournalStorage::NativeCrashJournalStorage(
    std::filesystem::path rootDirectory)
    : m_rootDirectory(std::move(rootDirectory)) {}

std::filesystem::path NativeCrashJournalStorage::slotPath(
    JournalStorageSlot slot) const {
    return m_rootDirectory / slotFileName(slot);
}

bool NativeCrashJournalStorage::ensureRoot(std::uint32_t* systemError) {
    std::error_code error;
    std::filesystem::create_directories(m_rootDirectory, error);
    if (error) {
        if (systemError != nullptr) {
            *systemError = static_cast<std::uint32_t>(error.value());
        }
        return false;
    }
#if !defined(_WIN32)
    std::filesystem::permissions(
        m_rootDirectory,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, error);
    if (error) {
        if (systemError != nullptr) {
            *systemError = static_cast<std::uint32_t>(error.value());
        }
        return false;
    }
#endif
    if (systemError != nullptr) *systemError = 0;
    return true;
}

JournalReadResult NativeCrashJournalStorage::read(JournalStorageSlot slot,
                                                  std::size_t maxBytes) {
    JournalReadResult result;
    const auto path = slotPath(slot);
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        result.status = JournalReadStatus::Failed;
        result.systemError = static_cast<std::uint32_t>(error.value());
        return result;
    }
    if (!exists) {
        result.status = JournalReadStatus::Missing;
        return result;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        result.status = JournalReadStatus::Failed;
        result.systemError = static_cast<std::uint32_t>(error.value());
        return result;
    }
    if (size > maxBytes ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        result.status = JournalReadStatus::TooLarge;
        return result;
    }
    result.bytes.resize(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.status = JournalReadStatus::Failed;
        result.systemError = static_cast<std::uint32_t>(errno);
        return result;
    }
    if (!result.bytes.empty()) {
        input.read(reinterpret_cast<char*>(result.bytes.data()),
                   static_cast<std::streamsize>(result.bytes.size()));
    }
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        result.status = JournalReadStatus::Failed;
        result.bytes.clear();
        result.systemError = static_cast<std::uint32_t>(errno);
        return result;
    }
    result.status = JournalReadStatus::Success;
    return result;
}

bool NativeCrashJournalStorage::durableWrite(
    JournalStorageSlot slot,
    std::span<const std::byte> bytes,
    std::uint32_t* systemError) {
    if (!ensureRoot(systemError)) return false;
    const auto path = slotPath(slot);
#if defined(_WIN32)
    const HANDLE raw = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        if (systemError != nullptr) *systemError = GetLastError();
        return false;
    }
    ScopedHandle file(raw);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            remaining, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (WriteFile(file.get(), bytes.data() + offset, chunk,
                      &written, nullptr) == FALSE) {
            const DWORD error = GetLastError();
            if (systemError != nullptr) *systemError = error;
            return false;
        }
        if (written != chunk) {
            if (systemError != nullptr) *systemError = ERROR_WRITE_FAULT;
            return false;
        }
        offset += written;
    }
    if (FlushFileBuffers(file.get()) == FALSE) {
        if (systemError != nullptr) *systemError = GetLastError();
        return false;
    }
    if (systemError != nullptr) *systemError = 0;
    return true;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          S_IRUSR | S_IWUSR);
    if (fd < 0) {
        if (systemError != nullptr) *systemError = static_cast<std::uint32_t>(errno);
        return false;
    }
    std::size_t offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    const int savedError = errno;
    (void)::close(fd);
    if (!ok) {
        if (systemError != nullptr) {
            *systemError = static_cast<std::uint32_t>(savedError);
        }
        return false;
    }
    if (systemError != nullptr) *systemError = 0;
    return true;
#endif
}

bool NativeCrashJournalStorage::atomicReplace(
    JournalStorageSlot from,
    JournalStorageSlot to,
    std::uint32_t* systemError) {
    if (!ensureRoot(systemError)) return false;
    const auto source = slotPath(from);
    const auto destination = slotPath(to);
#if defined(_WIN32)
    if (MoveFileExW(source.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        if (systemError != nullptr) *systemError = GetLastError();
        return false;
    }
    if (systemError != nullptr) *systemError = 0;
    return true;
#else
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        if (systemError != nullptr) *systemError = static_cast<std::uint32_t>(errno);
        return false;
    }
    const int directory = ::open(m_rootDirectory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0 || ::fsync(directory) != 0) {
        const int savedError = errno;
        if (directory >= 0) (void)::close(directory);
        if (systemError != nullptr) {
            *systemError = static_cast<std::uint32_t>(savedError);
        }
        return false;
    }
    (void)::close(directory);
    if (systemError != nullptr) *systemError = 0;
    return true;
#endif
}

bool NativeCrashJournalStorage::remove(JournalStorageSlot slot,
                                       std::uint32_t* systemError) {
    const auto path = slotPath(slot);
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    (void)removed;
    if (error) {
        if (systemError != nullptr) {
            *systemError = static_cast<std::uint32_t>(error.value());
        }
        return false;
    }
    if (systemError != nullptr) *systemError = 0;
    return true;
}

std::optional<std::filesystem::path> defaultCrashJournalDirectory(
    std::uint32_t* systemError) {
#if defined(_WIN32)
    PWSTR localAppData = nullptr;
    const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData);
    if (FAILED(result) || localAppData == nullptr) {
        if (systemError != nullptr) {
            *systemError = static_cast<std::uint32_t>(result);
        }
        if (localAppData != nullptr) CoTaskMemFree(localAppData);
        return std::nullopt;
    }
    std::filesystem::path path(localAppData);
    CoTaskMemFree(localAppData);
    if (systemError != nullptr) *systemError = 0;
    return path / L"HydraSeat" / L"Recovery";
#else
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        if (systemError != nullptr) *systemError = ENOENT;
        return std::nullopt;
    }
    if (systemError != nullptr) *systemError = 0;
    return std::filesystem::path(home) / ".local" / "state" /
           "HydraSeat" / "Recovery";
#endif
}

bool CrashJournalStore::archiveCurrentBestEffort(
    std::span<const std::byte> currentBytes) {
    for (std::size_t index = kCrashJournalHistoryDepth - 1; index > 0; --index) {
        auto previous = m_storage.read(historySlot(index - 1),
                                       kCrashJournalMaxFileBytes);
        if (previous.status == JournalReadStatus::Success) {
            (void)m_storage.durableWrite(historySlot(index), previous.bytes, nullptr);
        }
    }
    return m_storage.durableWrite(JournalStorageSlot::History0,
                                  currentBytes, nullptr);
}

bool CrashJournalStore::persistStateInternal(const CrashJournalState& state,
                                             bool archiveCurrent,
                                             std::string* error) {
    const auto bytes = encodeCrashJournal(state);
    if (bytes.empty()) {
        setError(error, "crash journal state cannot be encoded");
        return false;
    }

    std::vector<std::byte> currentBytes;
    if (archiveCurrent) {
        auto current = m_storage.read(JournalStorageSlot::Current,
                                      kCrashJournalMaxFileBytes);
        if (current.status == JournalReadStatus::Success) {
            currentBytes = std::move(current.bytes);
        }
    }

    std::uint32_t systemError = 0;
    if (!m_storage.durableWrite(JournalStorageSlot::CurrentTemp,
                                bytes, &systemError) ||
        !m_storage.atomicReplace(JournalStorageSlot::CurrentTemp,
                                 JournalStorageSlot::Current,
                                 &systemError)) {
        (void)m_storage.remove(JournalStorageSlot::CurrentTemp, nullptr);
        if (error != nullptr) {
            *error = "durable crash journal write/replace failed: " +
                     std::to_string(systemError);
        }
        return false;
    }

    if (!currentBytes.empty()) {
        (void)archiveCurrentBestEffort(currentBytes);
    }
    return true;
}

bool CrashJournalStore::writeSafeMode(const SafeModeMarker& marker,
                                      std::string* error) {
    const auto bytes = encodeSafeModeMarker(marker);
    if (bytes.empty()) {
        setError(error, "safe-mode marker cannot be encoded");
        return false;
    }
    std::uint32_t systemError = 0;
    if (!m_storage.durableWrite(JournalStorageSlot::SafeModeTemp,
                                bytes, &systemError) ||
        !m_storage.atomicReplace(JournalStorageSlot::SafeModeTemp,
                                 JournalStorageSlot::SafeMode,
                                 &systemError)) {
        (void)m_storage.remove(JournalStorageSlot::SafeModeTemp, nullptr);
        if (error != nullptr) {
            *error = "safe-mode marker write/replace failed: " +
                     std::to_string(systemError);
        }
        return false;
    }
    return true;
}

bool CrashJournalStore::writeSafeModeForFailure(
    SafeModeReason reason,
    const CrashJournalState* state,
    std::uint32_t diagnosticCode) {
    SafeModeMarker marker;
    marker.reason = reason;
    marker.diagnosticCode = diagnosticCode;
    if (state != nullptr) {
        marker.sessionId = state->sessionId;
        marker.runtimeGeneration = state->runtimeGeneration;
        const auto encoded = encodeCrashJournal(*state);
        if (!encoded.empty()) marker.journalHash = hashCrashJournalBytes(encoded);
    }
    return writeSafeMode(marker, nullptr);
}

std::optional<CrashJournalState> CrashJournalStore::loadCurrent(
    std::string* error) const {
    auto result = m_storage.read(JournalStorageSlot::Current,
                                 kCrashJournalMaxFileBytes);
    if (result.status == JournalReadStatus::Missing) return std::nullopt;
    if (result.status != JournalReadStatus::Success) {
        setError(error, result.status == JournalReadStatus::TooLarge
                            ? "crash journal file is too large"
                            : "crash journal read failed");
        return std::nullopt;
    }
    return decodeCrashJournal(result.bytes, error);
}

std::optional<SafeModeMarker> CrashJournalStore::loadSafeMode(
    std::string* error) const {
    auto result = m_storage.read(JournalStorageSlot::SafeMode,
                                 kSafeModeMarkerFileBytes);
    if (result.status == JournalReadStatus::Missing) return std::nullopt;
    if (result.status != JournalReadStatus::Success) {
        setError(error, result.status == JournalReadStatus::TooLarge
                            ? "safe-mode marker file is too large"
                            : "safe-mode marker read failed");
        return std::nullopt;
    }
    return decodeSafeModeMarker(result.bytes, error);
}

bool CrashJournalStore::beginActivation(const CrashJournalState& initial,
                                        std::string* error) {
    if (!validateCrashJournalState(initial, error) ||
        initial.phase != CrashJournalPhase::Preparing ||
        initial.finalResult != CrashJournalFinalResult::None ||
        initial.records.size() != 1 ||
        initial.records.front().kind != CrashJournalRecordKind::ActivationStarted) {
        setError(error, "begin-activation requires a canonical initial journal");
        return false;
    }

    auto marker = m_storage.read(JournalStorageSlot::SafeMode,
                                 kSafeModeMarkerFileBytes);
    if (marker.status == JournalReadStatus::Success) {
        setError(error, "safe mode is active; risky activation is blocked");
        return false;
    }
    if (marker.status != JournalReadStatus::Missing) {
        setError(error, "safe-mode marker cannot be safely inspected");
        return false;
    }

    auto current = m_storage.read(JournalStorageSlot::Current,
                                  kCrashJournalMaxFileBytes);
    bool archiveCurrent = false;
    if (current.status == JournalReadStatus::Success) {
        std::string decodeError;
        const auto prior = decodeCrashJournal(current.bytes, &decodeError);
        if (!prior) {
            (void)writeSafeModeForFailure(SafeModeReason::CorruptJournal,
                                          nullptr, 1);
            setError(error, "existing crash journal is corrupt; activation blocked");
            return false;
        }
        if (prior->phase != CrashJournalPhase::Clean ||
            prior->finalResult != CrashJournalFinalResult::Clean ||
            initial.runtimeGeneration <= prior->runtimeGeneration) {
            (void)writeSafeModeForFailure(SafeModeReason::IncompleteSession,
                                          &*prior, 2);
            setError(error, "previous session is not clean or generation did not advance");
            return false;
        }
        archiveCurrent = true;
    } else if (current.status != JournalReadStatus::Missing) {
        (void)writeSafeModeForFailure(SafeModeReason::CorruptJournal,
                                      nullptr, 3);
        setError(error, "existing crash journal cannot be safely inspected");
        return false;
    }

    if (!persistStateInternal(initial, archiveCurrent, error)) {
        (void)writeSafeModeForFailure(SafeModeReason::JournalWriteFailure,
                                      &initial, 4);
        return false;
    }
    return true;
}

bool CrashJournalStore::stateExtendsCurrent(const CrashJournalState& current,
                                            const CrashJournalState& next,
                                            std::string* error) const {
    if (current.sessionId != next.sessionId ||
        current.planHash != next.planHash ||
        current.runtimeGeneration != next.runtimeGeneration ||
        current.lease != next.lease || current.snapshots != next.snapshots) {
        setError(error, "crash journal transition changed immutable session identity");
        return false;
    }
    if (next.records.size() < current.records.size()) {
        setError(error, "crash journal transition attempted to truncate history");
        return false;
    }
    if (next.records.size() > current.records.size() + 1) {
        setError(error, "crash journal transition skipped a durable record boundary");
        return false;
    }
    if (!std::equal(current.records.begin(), current.records.end(),
                    next.records.begin())) {
        setError(error, "crash journal transition rewrote existing records");
        return false;
    }
    if (next.records.size() == current.records.size() && next != current) {
        setError(error, "crash journal transition changed state without a new record");
        return false;
    }
    return true;
}

bool CrashJournalStore::persistTransition(const CrashJournalState& state,
                                          std::string* error) {
    if (!validateCrashJournalState(state, error)) return false;
    auto currentRead = m_storage.read(JournalStorageSlot::Current,
                                      kCrashJournalMaxFileBytes);
    if (currentRead.status != JournalReadStatus::Success) {
        (void)writeSafeModeForFailure(SafeModeReason::CorruptJournal,
                                      &state, 5);
        setError(error, "current crash journal is missing/unreadable during transition");
        return false;
    }
    std::string decodeError;
    const auto current = decodeCrashJournal(currentRead.bytes, &decodeError);
    if (!current || !stateExtendsCurrent(*current, state, error)) {
        (void)writeSafeModeForFailure(SafeModeReason::RecoveryRequired,
                                      current ? &*current : &state, 6);
        if (!current) setError(error, "current crash journal became corrupt");
        return false;
    }
    if (state == *current) return true;
    if (!persistStateInternal(state, true, error)) {
        (void)writeSafeModeForFailure(SafeModeReason::JournalWriteFailure,
                                      &state, 7);
        return false;
    }
    return true;
}

StartupAssessment CrashJournalStore::assessStartupAndEnterSafeMode() {
    StartupAssessment assessment;

    auto markerRead = m_storage.read(JournalStorageSlot::SafeMode,
                                     kSafeModeMarkerFileBytes);
    if (markerRead.status == JournalReadStatus::Success) {
        std::string markerError;
        auto marker = decodeSafeModeMarker(markerRead.bytes, &markerError);
        assessment.state = StartupRecoveryState::RecoveryRequired;
        if (marker) {
            assessment.safeMode = *marker;
            assessment.diagnostic = "safe-mode marker is active";
        } else {
            assessment.diagnostic = "safe-mode marker is corrupt: " + markerError;
        }
        return assessment;
    }
    if (markerRead.status != JournalReadStatus::Missing) {
        assessment.state = StartupRecoveryState::RecoveryRequired;
        assessment.diagnostic = "safe-mode marker cannot be read safely";
        return assessment;
    }

    auto current = m_storage.read(JournalStorageSlot::Current,
                                  kCrashJournalMaxFileBytes);
    if (current.status == JournalReadStatus::Missing) {
        assessment.state = StartupRecoveryState::Clean;
        assessment.diagnostic = "no crash journal exists";
        return assessment;
    }
    if (current.status != JournalReadStatus::Success) {
        assessment.state = StartupRecoveryState::RecoveryRequired;
        assessment.diagnostic = "crash journal cannot be read safely";
        assessment.safeModeWriteFailed = !writeSafeModeForFailure(
            SafeModeReason::CorruptJournal, nullptr, 8);
        return assessment;
    }

    std::string decodeError;
    auto state = decodeCrashJournal(current.bytes, &decodeError);
    if (!state) {
        assessment.state = StartupRecoveryState::RecoveryRequired;
        assessment.diagnostic = "crash journal is corrupt: " + decodeError;
        SafeModeMarker marker;
        marker.reason = SafeModeReason::CorruptJournal;
        marker.diagnosticCode = 9;
        marker.journalHash = hashCrashJournalBytes(current.bytes);
        assessment.safeModeWriteFailed = !writeSafeMode(marker, nullptr);
        if (!assessment.safeModeWriteFailed) assessment.safeMode = marker;
        return assessment;
    }
    assessment.journal = *state;

    if (state->phase == CrashJournalPhase::Clean &&
        state->finalResult == CrashJournalFinalResult::Clean) {
        assessment.state = StartupRecoveryState::Clean;
        assessment.diagnostic = "previous runtime session is clean";
        return assessment;
    }

    const bool hardFailure =
        state->phase == CrashJournalPhase::RecoveryRequired ||
        state->finalResult == CrashJournalFinalResult::Failed ||
        state->finalResult == CrashJournalFinalResult::RecoveryRequired;
    assessment.state = hardFailure
        ? StartupRecoveryState::RecoveryRequired
        : StartupRecoveryState::RecoverableIncomplete;
    assessment.diagnostic = hardFailure
        ? "previous runtime session requires recovery"
        : "previous runtime session is incomplete";

    SafeModeMarker marker;
    marker.sessionId = state->sessionId;
    marker.runtimeGeneration = state->runtimeGeneration;
    marker.reason = hardFailure ? SafeModeReason::RecoveryRequired
                                : SafeModeReason::IncompleteSession;
    marker.diagnosticCode = hardFailure ? 10u : 11u;
    marker.journalHash = hashCrashJournalBytes(current.bytes);
    assessment.safeModeWriteFailed = !writeSafeMode(marker, nullptr);
    if (!assessment.safeModeWriteFailed) assessment.safeMode = marker;
    return assessment;
}

bool CrashJournalStore::replaceWithVerifiedCleanState(
    const CrashJournalState& cleanState,
    bool resetVerified,
    std::string* error) {
    if (!resetVerified || !validateCrashJournalState(cleanState, error) ||
        cleanState.phase != CrashJournalPhase::Clean ||
        cleanState.finalResult != CrashJournalFinalResult::Clean) {
        setError(error, "verified reset is required before replacing journal with clean state");
        return false;
    }
    return persistStateInternal(cleanState, true, error);
}

bool CrashJournalStore::clearSafeModeAfterVerifiedReset(
    const watchdog::SessionId& expectedSession,
    std::uint64_t expectedRuntimeGeneration,
    bool resetVerified,
    std::string* error) {
    if (!resetVerified || watchdog::isZeroSessionId(expectedSession) ||
        expectedRuntimeGeneration == 0) {
        setError(error, "verified reset identity is required to clear safe mode");
        return false;
    }

    auto markerRead = m_storage.read(JournalStorageSlot::SafeMode,
                                     kSafeModeMarkerFileBytes);
    if (markerRead.status != JournalReadStatus::Success) {
        setError(error, "safe-mode marker is missing or unreadable");
        return false;
    }
    std::string markerError;
    const auto marker = decodeSafeModeMarker(markerRead.bytes, &markerError);
    if (!marker) {
        setError(error, "safe-mode marker is corrupt and cannot be cleared automatically");
        return false;
    }
    if (!watchdog::isZeroSessionId(marker->sessionId) &&
        (marker->sessionId != expectedSession ||
         marker->runtimeGeneration != expectedRuntimeGeneration)) {
        setError(error, "safe-mode marker identity does not match verified reset");
        return false;
    }

    std::string journalError;
    const auto current = loadCurrent(&journalError);
    if (!current || current->phase != CrashJournalPhase::Clean ||
        current->finalResult != CrashJournalFinalResult::Clean ||
        current->sessionId != expectedSession ||
        current->runtimeGeneration != expectedRuntimeGeneration) {
        setError(error, "clean verified journal state is required before clearing safe mode");
        return false;
    }

    std::uint32_t systemError = 0;
    if (!m_storage.remove(JournalStorageSlot::SafeMode, &systemError)) {
        if (error != nullptr) {
            *error = "safe-mode marker removal failed: " +
                     std::to_string(systemError);
        }
        return false;
    }
    return true;
}

std::string_view crashJournalPhaseName(CrashJournalPhase phase) noexcept {
    switch (phase) {
    case CrashJournalPhase::Preparing: return "preparing";
    case CrashJournalPhase::Applying: return "applying";
    case CrashJournalPhase::Active: return "active";
    case CrashJournalPhase::RollingBack: return "rolling-back";
    case CrashJournalPhase::Clean: return "clean";
    case CrashJournalPhase::RecoveryRequired: return "recovery-required";
    }
    return "unknown";
}

std::string_view crashJournalRecordKindName(
    CrashJournalRecordKind kind) noexcept {
    switch (kind) {
    case CrashJournalRecordKind::ActivationStarted: return "activation-started";
    case CrashJournalRecordKind::ActionPrepared: return "action-prepared";
    case CrashJournalRecordKind::ActionApplied: return "action-applied";
    case CrashJournalRecordKind::ActionVerified: return "action-verified";
    case CrashJournalRecordKind::ActivationCommitted: return "activation-committed";
    case CrashJournalRecordKind::RollbackStarted: return "rollback-started";
    case CrashJournalRecordKind::ActionRolledBack: return "action-rolled-back";
    case CrashJournalRecordKind::RollbackVerified: return "rollback-verified";
    case CrashJournalRecordKind::CleanStop: return "clean-stop";
    case CrashJournalRecordKind::FailureRecorded: return "failure-recorded";
    case CrashJournalRecordKind::RecoveryRequired: return "recovery-required";
    }
    return "unknown";
}

std::string_view startupRecoveryStateName(
    StartupRecoveryState state) noexcept {
    switch (state) {
    case StartupRecoveryState::Clean: return "clean";
    case StartupRecoveryState::RecoverableIncomplete: return "recoverable-incomplete";
    case StartupRecoveryState::RecoveryRequired: return "recovery-required";
    }
    return "unknown";
}

std::string_view safeModeReasonName(SafeModeReason reason) noexcept {
    switch (reason) {
    case SafeModeReason::IncompleteSession: return "incomplete-session";
    case SafeModeReason::CorruptJournal: return "corrupt-journal";
    case SafeModeReason::RecoveryRequired: return "recovery-required";
    case SafeModeReason::JournalWriteFailure: return "journal-write-failure";
    case SafeModeReason::ManualRecovery: return "manual-recovery";
    }
    return "unknown";
}

} // namespace hydra::recovery
