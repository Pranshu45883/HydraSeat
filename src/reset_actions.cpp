#include "hydra/reset_actions.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hydra::reset {
namespace {

void setError(std::string* error, std::string_view value) {
    if (error != nullptr) *error = value;
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
    void bytes(std::span<const std::uint8_t> value) {
        for (const auto byte : value) u8(byte);
    }
    void raw(std::span<const std::byte> value) {
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
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
    bool bytes(std::span<std::uint8_t> value) {
        for (auto& byte : value) {
            if (!u8(byte)) return false;
        }
        return true;
    }
    bool raw(std::size_t count, std::span<const std::byte>& value) {
        if (count > remaining()) return false;
        value = m_bytes.subspan(m_offset, count);
        m_offset += count;
        return true;
    }
    std::size_t remaining() const noexcept { return m_bytes.size() - m_offset; }
    bool empty() const noexcept { return remaining() == 0; }

private:
    std::span<const std::byte> m_bytes;
    std::size_t m_offset{0};
};

bool isSatisfied(watchdog::RollbackActionResult result) noexcept {
    return result == watchdog::RollbackActionResult::Success ||
           result == watchdog::RollbackActionResult::AlreadySatisfied;
}

class PreparedOwnedProcessGuard {
public:
    explicit PreparedOwnedProcessGuard(watchdog::RollbackExecutor& executor)
        : m_executor(executor) {}
    ~PreparedOwnedProcessGuard() { m_executor.clearPreparedOwnedProcesses(); }

    PreparedOwnedProcessGuard(const PreparedOwnedProcessGuard&) = delete;
    PreparedOwnedProcessGuard& operator=(const PreparedOwnedProcessGuard&) = delete;

private:
    watchdog::RollbackExecutor& m_executor;
};

recovery::Hash256 hashRuntimeResetRegistrationLegacy(
    const RuntimeResetRegistration& registration,
    std::span<const std::byte> canonicalManifestFrame) {
    ByteWriter writer(sizeof(registration.ownerProcess.processId) +
                      sizeof(registration.ownerProcess.creationTime100ns) +
                      canonicalManifestFrame.size());
    writer.u32(registration.ownerProcess.processId);
    writer.u64(registration.ownerProcess.creationTime100ns);
    writer.raw(canonicalManifestFrame);
    const auto canonical = writer.take();
    return recovery::hashCrashJournalBytes(canonical);
}

recovery::Hash256 hashRuntimeResetRegistrationV2(
    const RuntimeResetRegistration& registration,
    std::span<const std::byte> attachmentBytes,
    std::span<const std::byte> canonicalManifestFrame) {
    ByteWriter writer(sizeof(registration.ownerProcess.processId) +
                      sizeof(registration.ownerProcess.creationTime100ns) +
                      sizeof(std::uint32_t) + attachmentBytes.size() +
                      canonicalManifestFrame.size());
    writer.u32(registration.ownerProcess.processId);
    writer.u64(registration.ownerProcess.creationTime100ns);
    writer.u32(static_cast<std::uint32_t>(attachmentBytes.size()));
    writer.raw(attachmentBytes);
    writer.raw(canonicalManifestFrame);
    const auto canonical = writer.take();
    return recovery::hashCrashJournalBytes(canonical);
}

bool validateRuntimeResetRegistrationShape(
    const RuntimeResetRegistration& registration,
    bool requireExactAttachment,
    std::string* error) {
    if (registration.ownerProcess.processId == 0 ||
        registration.ownerProcess.creationTime100ns == 0) {
        setError(error, "runtime reset registration owner identity is incomplete");
        return false;
    }
    if (!watchdog::validateRollbackPlan(registration.manifest, error)) return false;
    for (const auto& action : registration.manifest.actions) {
        if (action.actionId == kResetOwnerActionId) {
            setError(error, "runtime reset manifest uses the reserved owner action id");
            return false;
        }
        if (action.kind == watchdog::RollbackActionKind::TerminateOwnedProcess &&
            action.process == registration.ownerProcess) {
            setError(error, "runtime reset owner must not duplicate a rollback target");
            return false;
        }
    }
    if (!registration.attachment) {
        if (requireExactAttachment) {
            setError(error,
                     "runtime reset v2 registration requires exact recovery attachment authority");
            return false;
        }
        if (error != nullptr) error->clear();
        return true;
    }
    recovery::RecoveryProcessAttachmentRegistration attached;
    attached.identity = *registration.attachment;
    attached.manifest = registration.manifest;
    if (!recovery::validateRecoveryProcessAttachmentRegistration(attached, error)) {
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool executeRegisteredActions(
    const RuntimeResetRegistration& registration,
    std::uint64_t generation,
    watchdog::RollbackExecutor& executor,
    ResetExecutionReport& report) {
    if (!registration.attachment) {
        report.diagnostic =
            "legacy runtime reset registration cannot authorize process mutation";
        return false;
    }
#if defined(_WIN32)
    watchdog::ProcessIdentity currentIdentity;
    std::uint32_t currentIdentityError = 0;
    if (watchdog::queryProcessIdentity(GetCurrentProcessId(), currentIdentity,
                                       &currentIdentityError) &&
        currentIdentity == registration.ownerProcess) {
        report.diagnostic =
            "reset registration attempts to terminate the reset process itself";
        return false;
    }
#endif

    watchdog::RollbackRegistry registry;
    std::string error;
    if (!registry.registerPlan(registration.manifest, &error)) {
        report.diagnostic = "reset rollback plan cannot be armed: " + error;
        return false;
    }
    if (!executor.prepareOwnedProcesses(registration.manifest.actions, &error)) {
        report.diagnostic =
            "exact rollback process preflight failed before owner stop: " + error;
        return false;
    }
    PreparedOwnedProcessGuard preparedGuard(executor);

    watchdog::RollbackActionDescriptor ownerAction;
    ownerAction.actionId = kResetOwnerActionId;
    ownerAction.kind = watchdog::RollbackActionKind::TerminateOwnedProcess;
    ownerAction.activationOrdinal = std::numeric_limits<std::uint32_t>::max();
    ownerAction.timeoutMilliseconds = kResetOwnerTimeoutMs;
    ownerAction.generation = generation;
    ownerAction.process = registration.ownerProcess;
    report.ownerOutcome = executor.terminateOwnedProcess(
        ownerAction, kResetOwnerTimeoutMs);
    report.ownerSatisfied = isSatisfied(report.ownerOutcome->result);
    if (!report.ownerSatisfied) {
        report.diagnostic =
            "runtime owner exact-process stop could not be verified";
        return false;
    }

    report.rollback = registry.execute(executor);
    report.rollbackSatisfied = report.rollback.allSatisfied;
    if (!report.rollbackSatisfied) {
        report.diagnostic =
            "one or more exact reset actions could not be verified";
        return false;
    }
    return true;
}

bool registrationMatchesJournal(
    const RuntimeResetRegistration& registration,
    const recovery::CrashJournalState& journal,
    std::string* error) {
    if (registration.manifest.lease.sessionId != journal.sessionId) {
        setError(error, "runtime reset registration session does not match crash journal");
        return false;
    }
    if (registration.manifest.lease.generation != journal.lease.generation ||
        journal.runtimeGeneration == 0) {
        setError(error, "runtime reset registration generation does not match crash journal");
        return false;
    }
    const auto hash = recovery::hashRollbackPlanManifest(registration.manifest);
    if (hash != journal.planHash) {
        setError(error, "runtime reset registration plan hash does not match crash journal");
        return false;
    }
    if (!recovery::validateCrashJournalAgainstPlan(
            journal, registration.manifest, error)) {
        return false;
    }
    const bool journalHasAttachment = std::any_of(
        journal.snapshots.begin(), journal.snapshots.end(), [](const auto& snapshot) {
            return snapshot.snapshotId ==
                   recovery::kRecoveryProcessAttachmentSnapshotId;
        });
    const bool journalClean =
        journal.phase == recovery::CrashJournalPhase::Clean &&
        journal.finalResult == recovery::CrashJournalFinalResult::Clean;
    if (registration.attachment) {
        if (!recovery::validateRecoveryProcessAttachmentJournalBinding(
                journal, *registration.attachment, error)) {
            return false;
        }
    } else if (journalHasAttachment) {
        setError(error,
                 "attachment-bound crash journal cannot be recovered by a legacy reset registration");
        return false;
    } else if (!journalClean) {
        setError(error,
                 "legacy runtime reset registration is diagnostic-only for incomplete recovery state");
        return false;
    }
    return true;
}

bool safeModeMatchesJournal(const recovery::SafeModeMarker& marker,
                            const recovery::CrashJournalState& journal,
                            std::string* error) {
    if (!watchdog::isZeroSessionId(marker.sessionId) &&
        (marker.sessionId != journal.sessionId ||
         marker.runtimeGeneration != journal.runtimeGeneration)) {
        setError(error, "safe-mode marker identity does not match crash journal");
        return false;
    }
    return true;
}

void writeRecoveryRequiredMarker(recovery::CrashJournalStore& store,
                                 const recovery::CrashJournalState& journal,
                                 std::uint32_t diagnosticCode) {
    recovery::SafeModeMarker marker;
    marker.sessionId = journal.sessionId;
    marker.runtimeGeneration = journal.runtimeGeneration;
    marker.reason = recovery::SafeModeReason::RecoveryRequired;
    marker.diagnosticCode = diagnosticCode;
    const auto encoded = recovery::encodeCrashJournal(journal);
    if (!encoded.empty()) {
        marker.journalHash = recovery::hashCrashJournalBytes(encoded);
    }
    (void)store.writeSafeMode(marker, nullptr);
}

std::optional<recovery::CrashJournalState> makeVerifiedResetCleanState(
    const RuntimeResetRegistration& registration,
    const recovery::CrashJournalState& prior,
    std::string* error) {
    auto state = recovery::makeInitialCrashJournal(
        registration.manifest, prior.runtimeGeneration, prior.snapshots, error);
    if (!state) return std::nullopt;

    for (const auto& action : registration.manifest.actions) {
        if (!recovery::appendCrashJournalRecord(
                *state, registration.manifest,
                recovery::CrashJournalRecordKind::ActionPrepared,
                action.actionId, action.generation, error)) {
            return std::nullopt;
        }
    }
    if (!recovery::appendCrashJournalRecord(
            *state, registration.manifest,
            recovery::CrashJournalRecordKind::RollbackStarted,
            0, prior.runtimeGeneration, error)) {
        return std::nullopt;
    }

    auto reverse = registration.manifest.actions;
    std::sort(reverse.begin(), reverse.end(), [](const auto& left, const auto& right) {
        if (left.activationOrdinal != right.activationOrdinal) {
            return left.activationOrdinal > right.activationOrdinal;
        }
        return left.actionId > right.actionId;
    });
    for (const auto& action : reverse) {
        if (!recovery::appendCrashJournalRecord(
                *state, registration.manifest,
                recovery::CrashJournalRecordKind::ActionRolledBack,
                action.actionId, action.generation, error)) {
            return std::nullopt;
        }
    }
    if (!recovery::appendCrashJournalRecord(
            *state, registration.manifest,
            recovery::CrashJournalRecordKind::RollbackVerified,
            0, prior.runtimeGeneration, error) ||
        !recovery::appendCrashJournalRecord(
            *state, registration.manifest,
            recovery::CrashJournalRecordKind::CleanStop,
            0, prior.runtimeGeneration, error)) {
        return std::nullopt;
    }
    return state;
}

bool durableWriteFile(const std::filesystem::path& path,
                      std::span<const std::byte> bytes,
                      std::uint32_t* systemError) {
#if defined(_WIN32)
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (systemError != nullptr) *systemError = GetLastError();
        return false;
    }
    std::size_t offset = 0;
    bool ok = true;
    std::uint32_t error = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) == FALSE ||
            written != chunk) {
            error = GetLastError();
            if (error == 0) error = ERROR_WRITE_FAULT;
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && FlushFileBuffers(file) == FALSE) {
        error = GetLastError();
        ok = false;
    }
    CloseHandle(file);
    if (systemError != nullptr) *systemError = ok ? 0u : error;
    return ok;
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
    if (systemError != nullptr) {
        *systemError = ok ? 0u : static_cast<std::uint32_t>(savedError);
    }
    return ok;
#endif
}

bool atomicReplaceFile(const std::filesystem::path& from,
                       const std::filesystem::path& to,
                       std::uint32_t* systemError) {
#if defined(_WIN32)
    if (MoveFileExW(from.c_str(), to.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        if (systemError != nullptr) *systemError = GetLastError();
        return false;
    }
    if (systemError != nullptr) *systemError = 0;
    return true;
#else
    if (::rename(from.c_str(), to.c_str()) != 0) {
        if (systemError != nullptr) *systemError = static_cast<std::uint32_t>(errno);
        return false;
    }
    const auto directoryPath = to.parent_path();
    const int directory = ::open(directoryPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0 || ::fsync(directory) != 0) {
        const int savedError = errno;
        if (directory >= 0) (void)::close(directory);
        if (systemError != nullptr) *systemError = static_cast<std::uint32_t>(savedError);
        return false;
    }
    (void)::close(directory);
    if (systemError != nullptr) *systemError = 0;
    return true;
#endif
}

} // namespace

bool validateRuntimeResetRegistration(
    const RuntimeResetRegistration& registration,
    std::string* error) {
    return validateRuntimeResetRegistrationShape(
        registration, true, error);
}

std::vector<std::byte> encodeRuntimeResetRegistration(
    const RuntimeResetRegistration& registration) {
    if (!validateRuntimeResetRegistration(registration, nullptr)) return {};
    const auto frame = watchdog::encodeRegisterPlan(1, registration.manifest);
    if (frame.empty() || frame.size() > watchdog::kWatchdogMaxFrameBytes) return {};

    std::vector<std::byte> attachmentBytes;
    if (registration.attachment) {
        attachmentBytes = recovery::encodeRecoveryProcessAttachmentIdentity(
            *registration.attachment);
        if (attachmentBytes.size() !=
            recovery::kRecoveryProcessAttachmentIdentityBytes) {
            return {};
        }
    }
    const auto registrationHash = hashRuntimeResetRegistrationV2(
        registration, attachmentBytes, frame);
    const auto totalBytes = kRuntimeResetRegistrationHeaderBytes +
        attachmentBytes.size() + frame.size();
    if (totalBytes > kRuntimeResetRegistrationMaxBytes ||
        totalBytes > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return {};
    }

    ByteWriter writer(totalBytes);
    writer.u32(kRuntimeResetRegistrationMagic);
    writer.u16(kRuntimeResetRegistrationVersion);
    writer.u16(static_cast<std::uint16_t>(kRuntimeResetRegistrationHeaderBytes));
    writer.u32(static_cast<std::uint32_t>(totalBytes));
    writer.u32(registration.ownerProcess.processId);
    writer.u64(registration.ownerProcess.creationTime100ns);
    writer.bytes(registrationHash);
    writer.u32(static_cast<std::uint32_t>(attachmentBytes.size()));
    writer.u32(static_cast<std::uint32_t>(frame.size()));
    writer.u32(0);
    writer.raw(attachmentBytes);
    writer.raw(frame);
    return writer.take();
}

std::optional<RuntimeResetRegistration> decodeRuntimeResetRegistration(
    std::span<const std::byte> bytes,
    std::string* error) {
    if (bytes.size() < kRuntimeResetRegistrationLegacyHeaderBytes ||
        bytes.size() > kRuntimeResetRegistrationMaxBytes) {
        setError(error, "runtime reset registration size is out of bounds");
        return std::nullopt;
    }
    ByteReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t headerBytes = 0;
    if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(headerBytes)) {
        setError(error, "runtime reset registration header is truncated");
        return std::nullopt;
    }
    if (magic != kRuntimeResetRegistrationMagic ||
        (version != kRuntimeResetRegistrationLegacyVersion &&
         version != kRuntimeResetRegistrationVersion)) {
        setError(error, "runtime reset registration magic/version is unsupported");
        return std::nullopt;
    }
    const std::size_t expectedHeaderBytes =
        version == kRuntimeResetRegistrationLegacyVersion
            ? kRuntimeResetRegistrationLegacyHeaderBytes
            : kRuntimeResetRegistrationHeaderBytes;
    if (headerBytes != expectedHeaderBytes) {
        setError(error, "runtime reset registration header size is invalid");
        return std::nullopt;
    }

    std::uint32_t totalBytes = 0;
    RuntimeResetRegistration result;
    recovery::Hash256 storedHash{};
    std::uint32_t attachmentBytes = 0;
    std::uint32_t frameBytes = 0;
    std::uint32_t reserved = 0;
    if (!reader.u32(totalBytes) || !reader.u32(result.ownerProcess.processId) ||
        !reader.u64(result.ownerProcess.creationTime100ns) ||
        !reader.bytes(storedHash)) {
        setError(error, "runtime reset registration header is truncated");
        return std::nullopt;
    }
    if (version == kRuntimeResetRegistrationVersion &&
        !reader.u32(attachmentBytes)) {
        setError(error, "runtime reset attachment length is truncated");
        return std::nullopt;
    }
    if (version == kRuntimeResetRegistrationVersion && attachmentBytes == 0u) {
        setError(error,
                 "runtime reset v2 registration is missing exact recovery attachment authority");
        return std::nullopt;
    }
    if (!reader.u32(frameBytes) || !reader.u32(reserved)) {
        setError(error, "runtime reset registration payload lengths are truncated");
        return std::nullopt;
    }
    if (reserved != 0u || totalBytes != bytes.size() ||
        frameBytes > watchdog::kWatchdogMaxFrameBytes ||
        (attachmentBytes != 0u &&
         attachmentBytes != recovery::kRecoveryProcessAttachmentIdentityBytes) ||
        static_cast<std::size_t>(attachmentBytes) +
                static_cast<std::size_t>(frameBytes) != reader.remaining()) {
        setError(error, "runtime reset registration header is invalid");
        return std::nullopt;
    }

    std::span<const std::byte> attachmentBytesView;
    if (!reader.raw(attachmentBytes, attachmentBytesView)) {
        setError(error, "runtime reset attachment payload is truncated");
        return std::nullopt;
    }
    if (attachmentBytes != 0u) {
        const auto attachment =
            recovery::decodeRecoveryProcessAttachmentIdentity(
                attachmentBytesView, error);
        if (!attachment) return std::nullopt;
        result.attachment = *attachment;
    }

    std::span<const std::byte> frameBytesView;
    if (!reader.raw(frameBytes, frameBytesView) || !reader.empty()) {
        setError(error, "runtime reset registration manifest payload is truncated");
        return std::nullopt;
    }
    const auto frame = watchdog::decodeWatchdogFrame(frameBytesView);
    if (!frame || frame.frame->type != watchdog::WatchdogMessageType::RegisterPlan ||
        frame.frame->sequence != 1 ||
        !watchdog::decodeRegisterPlan(*frame.frame, result.manifest, error)) {
        if (error != nullptr && error->empty()) {
            *error = "runtime reset registration manifest is invalid";
        }
        return std::nullopt;
    }

    const auto actualHash = version == kRuntimeResetRegistrationLegacyVersion
        ? hashRuntimeResetRegistrationLegacy(result, frameBytesView)
        : hashRuntimeResetRegistrationV2(
              result, attachmentBytesView, frameBytesView);
    if (actualHash != storedHash) {
        setError(error, "runtime reset registration integrity hash is invalid");
        return std::nullopt;
    }
    if (!validateRuntimeResetRegistrationShape(
            result, version == kRuntimeResetRegistrationVersion, error)) {
        return std::nullopt;
    }
    return result;
}

std::filesystem::path RuntimeResetRegistrationStore::currentPath() const {
    return m_rootDirectory / "reset-runtime.bin";
}

std::filesystem::path RuntimeResetRegistrationStore::temporaryPath() const {
    return m_rootDirectory / "reset-runtime.tmp";
}

bool RuntimeResetRegistrationStore::ensureRoot(std::uint32_t* systemError) const {
    std::error_code error;
    std::filesystem::create_directories(m_rootDirectory, error);
    if (error) {
        if (systemError != nullptr) *systemError = static_cast<std::uint32_t>(error.value());
        return false;
    }
    if (systemError != nullptr) *systemError = 0;
    return true;
}

bool RuntimeResetRegistrationStore::write(
    const RuntimeResetRegistration& registration,
    std::string* error) {
    const auto bytes = encodeRuntimeResetRegistration(registration);
    if (bytes.empty()) {
        setError(error, "runtime reset registration cannot be encoded");
        return false;
    }
    std::uint32_t systemError = 0;
    if (!ensureRoot(&systemError) ||
        !durableWriteFile(temporaryPath(), bytes, &systemError) ||
        !atomicReplaceFile(temporaryPath(), currentPath(), &systemError)) {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath(), ignored);
        if (error != nullptr) {
            *error = "runtime reset registration durable write failed: " +
                     std::to_string(systemError);
        }
        return false;
    }
    return true;
}

RuntimeRegistrationReadResult RuntimeResetRegistrationStore::load() const {
    RuntimeRegistrationReadResult result;
    std::error_code error;
    const bool exists = std::filesystem::exists(currentPath(), error);
    if (error) {
        result.status = RuntimeRegistrationReadStatus::Failed;
        result.systemError = static_cast<std::uint32_t>(error.value());
        result.diagnostic = "runtime reset registration existence check failed";
        return result;
    }
    if (!exists) {
        result.status = RuntimeRegistrationReadStatus::Missing;
        result.diagnostic = "runtime reset registration is absent";
        return result;
    }
    const auto size = std::filesystem::file_size(currentPath(), error);
    if (error) {
        result.status = RuntimeRegistrationReadStatus::Failed;
        result.systemError = static_cast<std::uint32_t>(error.value());
        result.diagnostic = "runtime reset registration size check failed";
        return result;
    }
    if (size > kRuntimeResetRegistrationMaxBytes) {
        result.status = RuntimeRegistrationReadStatus::TooLarge;
        result.diagnostic = "runtime reset registration exceeds the bounded file size";
        return result;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream input(currentPath(), std::ios::binary);
    if (!input) {
        result.status = RuntimeRegistrationReadStatus::Failed;
        result.systemError = static_cast<std::uint32_t>(errno);
        result.diagnostic = "runtime reset registration cannot be opened";
        return result;
    }
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        result.status = RuntimeRegistrationReadStatus::Failed;
        result.systemError = static_cast<std::uint32_t>(errno);
        result.diagnostic = "runtime reset registration cannot be read completely";
        return result;
    }
    std::string decodeError;
    auto registration = decodeRuntimeResetRegistration(bytes, &decodeError);
    if (!registration) {
        result.status = RuntimeRegistrationReadStatus::Corrupt;
        result.diagnostic = "runtime reset registration is corrupt: " + decodeError;
        return result;
    }
    result.status = RuntimeRegistrationReadStatus::Success;
    result.registration = std::move(*registration);
    result.diagnostic = result.registration->attachment
        ? "runtime reset registration has exact attachment authority"
        : "legacy runtime reset registration is readable diagnostic evidence only";
    return result;
}

bool RuntimeResetRegistrationStore::remove(std::string* error) {
    std::error_code filesystemError;
    (void)std::filesystem::remove(currentPath(), filesystemError);
    if (filesystemError) {
        if (error != nullptr) {
            *error = "runtime reset registration removal failed: " +
                     std::to_string(filesystemError.value());
        }
        return false;
    }
    std::error_code ignored;
    (void)std::filesystem::remove(temporaryPath(), ignored);
    return true;
}

ResetInspection inspectResetState(
    recovery::CrashJournalStore& journalStore,
    RuntimeResetRegistrationStore& registrationStore) {
    ResetInspection result;
    std::string journalError;
    result.journal = journalStore.loadCurrent(&journalError);
    if (!journalError.empty()) {
        result.state = ResetState::RecoveryRequired;
        result.diagnostic = "crash journal cannot be inspected: " + journalError;
        return result;
    }
    std::string safeModeError;
    result.safeMode = journalStore.loadSafeMode(&safeModeError);
    if (!safeModeError.empty()) {
        result.state = ResetState::RecoveryRequired;
        result.diagnostic = "safe-mode marker cannot be inspected: " + safeModeError;
        return result;
    }
    const auto registrationRead = registrationStore.load();
    if (registrationRead.status == RuntimeRegistrationReadStatus::Success) {
        result.registration = registrationRead.registration;
    } else if (registrationRead.status != RuntimeRegistrationReadStatus::Missing) {
        result.state = ResetState::RecoveryRequired;
        result.diagnostic = registrationRead.diagnostic;
        return result;
    }

    if (!result.journal) {
        if (result.safeMode) {
            result.state = ResetState::RecoveryRequired;
            result.diagnostic = "safe mode is active without a correlating crash journal";
            return result;
        }
        if (result.registration) {
            if (!result.registration->attachment) {
                result.state = ResetState::RecoveryRequired;
                result.diagnostic =
                    "pre-journal legacy reset registration lacks exact recovery attachment authority";
                return result;
            }
            result.state = ResetState::Recoverable;
            result.diagnostic =
                "pre-journal exact attachment registration is available for cleanup";
            return result;
        }
        result.state = ResetState::Clean;
        result.diagnostic = "no active recovery state exists";
        return result;
    }

    if (result.safeMode) {
        std::string error;
        if (!safeModeMatchesJournal(*result.safeMode, *result.journal, &error)) {
            result.state = ResetState::RecoveryRequired;
            result.diagnostic = error;
            return result;
        }
    }

    if (result.registration) {
        std::string error;
        if (!registrationMatchesJournal(*result.registration, *result.journal, &error)) {
            result.state = ResetState::RecoveryRequired;
            result.diagnostic = error;
            return result;
        }
    }

    const bool clean = result.journal->phase == recovery::CrashJournalPhase::Clean &&
                       result.journal->finalResult == recovery::CrashJournalFinalResult::Clean;
    if (clean && !result.registration && !result.safeMode) {
        result.state = ResetState::Clean;
        result.diagnostic = "crash journal is clean and no reset metadata remains";
        return result;
    }
    if (clean) {
        result.state = ResetState::Recoverable;
        result.diagnostic = "runtime state is clean but stale reset/safe-mode metadata remains";
        return result;
    }
    if (!result.registration) {
        result.state = ResetState::RecoveryRequired;
        result.diagnostic = "incomplete runtime journal has no trusted reset registration";
        return result;
    }
    result.state = ResetState::Recoverable;
    result.diagnostic = "incomplete runtime state has a journal-correlated reset registration";
    return result;
}

ResetExecutionReport executeVerifiedReset(
    recovery::CrashJournalStore& journalStore,
    RuntimeResetRegistrationStore& registrationStore,
    watchdog::RollbackExecutor& executor,
    std::optional<watchdog::SessionId> expectedSession) {
    ResetExecutionReport report;
    report.before = inspectResetState(journalStore, registrationStore);
    if (report.before.state == ResetState::Clean) {
        report.success = true;
        report.noOp = true;
        report.ownerSatisfied = true;
        report.rollbackSatisfied = true;
        report.journalClean = true;
        report.safeModeCleared = true;
        report.registrationCleared = true;
        report.after = report.before;
        report.diagnostic = "reset is already satisfied";
        return report;
    }
    if (!report.before.journal) {
        if (report.before.state != ResetState::Recoverable ||
            !report.before.registration) {
            report.after = report.before;
            report.diagnostic = report.before.diagnostic;
            return report;
        }
        const auto& registration = *report.before.registration;
        if (expectedSession &&
            *expectedSession != registration.manifest.lease.sessionId) {
            report.after = report.before;
            report.diagnostic =
                "requested session does not match pre-journal recovery registration";
            return report;
        }
        if (!executeRegisteredActions(
                registration, registration.manifest.lease.generation,
                executor, report)) {
            // The runtime registration is durably written before the host starts
            // its crash journal. If reset itself fails inside that narrow window,
            // promote the trusted registration into the minimal Preparing journal
            // so the failure becomes durable RecoveryRequired evidence and a later
            // reset can safely retry against the same correlated manifest.
            std::string recoveryError;
            std::vector<recovery::SnapshotReference> recoverySnapshots;
            if (registration.attachment) {
                const auto attachmentSnapshot =
                    recovery::makeRecoveryProcessAttachmentSnapshot(
                        *registration.attachment, &recoveryError);
                if (attachmentSnapshot) {
                    recoverySnapshots.push_back(*attachmentSnapshot);
                }
            }
            const auto failureJournal =
                (!registration.attachment || !recoverySnapshots.empty())
                ? recovery::makeInitialCrashJournal(
                      registration.manifest,
                      registration.manifest.lease.generation,
                      recoverySnapshots, &recoveryError)
                : std::optional<recovery::CrashJournalState>{};
            if (failureJournal &&
                journalStore.beginActivation(*failureJournal, &recoveryError)) {
                writeRecoveryRequiredMarker(
                    journalStore, *failureJournal, 0x52535409u);
            } else if (!recoveryError.empty()) {
                if (!report.diagnostic.empty()) report.diagnostic += "; ";
                report.diagnostic +=
                    "failed to persist pre-journal RecoveryRequired evidence: " +
                    recoveryError;
            }
            report.after = inspectResetState(journalStore, registrationStore);
            return report;
        }
        std::string error;
        report.registrationCleared = registrationStore.remove(&error);
        report.journalClean = true;
        report.safeModeCleared = !report.before.safeMode.has_value();
        report.after = inspectResetState(journalStore, registrationStore);
        report.success = report.registrationCleared &&
            report.after.state == ResetState::Clean;
        if (report.success) {
            report.diagnostic = "pre-journal emergency reset completed";
        } else if (report.diagnostic.empty()) {
            report.diagnostic = error.empty() ? report.after.diagnostic : error;
        }
        return report;
    }
    const auto& journal = *report.before.journal;
    if (expectedSession && *expectedSession != journal.sessionId) {
        writeRecoveryRequiredMarker(journalStore, journal, 0x52535401u);
        report.after = inspectResetState(journalStore, registrationStore);
        report.diagnostic = "requested session does not match current recovery session";
        return report;
    }
    if (report.before.state == ResetState::RecoveryRequired) {
        writeRecoveryRequiredMarker(journalStore, journal, 0x52535402u);
        report.after = inspectResetState(journalStore, registrationStore);
        report.diagnostic = report.before.diagnostic;
        return report;
    }

    const bool journalWasClean =
        journal.phase == recovery::CrashJournalPhase::Clean &&
        journal.finalResult == recovery::CrashJournalFinalResult::Clean;
    if (journalWasClean) {
        report.ownerSatisfied = true;
        report.rollbackSatisfied = true;
        report.journalClean = true;
        report.safeModeCleared = true;
        report.registrationCleared = true;
        std::string error;
        if (report.before.safeMode &&
            !journalStore.clearSafeModeAfterVerifiedReset(
                journal.sessionId, journal.runtimeGeneration, true, &error)) {
            report.safeModeCleared = false;
            report.diagnostic = error;
        }
        if (report.before.registration && !registrationStore.remove(&error)) {
            report.registrationCleared = false;
            if (report.diagnostic.empty()) report.diagnostic = error;
        }
        report.after = inspectResetState(journalStore, registrationStore);
        report.success = report.after.state == ResetState::Clean;
        if (report.success) report.diagnostic = "stale recovery metadata was cleared";
        return report;
    }

    if (!report.before.registration) {
        writeRecoveryRequiredMarker(journalStore, journal, 0x52535403u);
        report.after = inspectResetState(journalStore, registrationStore);
        report.diagnostic = "no trusted runtime reset registration is available";
        return report;
    }
    const auto& registration = *report.before.registration;

    std::string error;
    if (!executeRegisteredActions(
            registration, journal.runtimeGeneration, executor, report)) {
        const std::uint32_t diagnosticCode = !report.ownerSatisfied
            ? 0x52535405u
            : (report.rollback.outcomes.empty() ? 0x52535406u : 0x52535407u);
        writeRecoveryRequiredMarker(journalStore, journal, diagnosticCode);
        report.after = inspectResetState(journalStore, registrationStore);
        return report;
    }

    const auto cleanState = makeVerifiedResetCleanState(registration, journal, &error);
    if (!cleanState ||
        !journalStore.replaceWithVerifiedCleanState(*cleanState, true, &error)) {
        writeRecoveryRequiredMarker(journalStore, journal, 0x52535408u);
        report.after = inspectResetState(journalStore, registrationStore);
        report.diagnostic = "verified cleanup could not be persisted as clean: " + error;
        return report;
    }
    report.journalClean = true;

    report.safeModeCleared = true;
    if (report.before.safeMode &&
        !journalStore.clearSafeModeAfterVerifiedReset(
            journal.sessionId, journal.runtimeGeneration, true, &error)) {
        report.safeModeCleared = false;
        report.diagnostic = error;
    }

    report.registrationCleared = registrationStore.remove(&error);
    if (!report.registrationCleared && report.diagnostic.empty()) {
        report.diagnostic = error;
    }

    report.after = inspectResetState(journalStore, registrationStore);
    report.success = report.after.state == ResetState::Clean;
    if (report.success) {
        report.diagnostic = "verified emergency reset completed";
    } else if (report.diagnostic.empty()) {
        report.diagnostic = report.after.diagnostic;
    }
    return report;
}

bool enableManualSafeMode(recovery::CrashJournalStore& journalStore,
                          std::string* error) {
    std::string existingMarkerError;
    const auto existingMarker = journalStore.loadSafeMode(&existingMarkerError);
    if (!existingMarkerError.empty()) {
        setError(error, "cannot enable manual safe mode while the existing marker is unreadable");
        return false;
    }

    std::string journalError;
    const auto journal = journalStore.loadCurrent(&journalError);
    if (!journalError.empty()) {
        setError(error, "cannot enable manual safe mode while journal is unreadable");
        return false;
    }
    recovery::SafeModeMarker marker;
    marker.reason = recovery::SafeModeReason::ManualRecovery;
    marker.diagnosticCode = 0x52534d01u;
    if (journal) {
        marker.sessionId = journal->sessionId;
        marker.runtimeGeneration = journal->runtimeGeneration;
        const auto encoded = recovery::encodeCrashJournal(*journal);
        if (!encoded.empty()) {
            marker.journalHash = recovery::hashCrashJournalBytes(encoded);
        }
    }
    if (existingMarker) {
        if (*existingMarker == marker) return true;
        setError(error,
                 "an existing safe-mode marker cannot be overwritten by manual mode");
        return false;
    }
    return journalStore.writeSafeMode(marker, error);
}

bool disableManualSafeMode(recovery::CrashJournalStorage& storage,
                           recovery::CrashJournalStore& journalStore,
                           std::string* error) {
    std::string markerError;
    const auto marker = journalStore.loadSafeMode(&markerError);
    if (!markerError.empty()) {
        setError(error, markerError);
        return false;
    }
    if (!marker) return true;

    std::string journalError;
    const auto journal = journalStore.loadCurrent(&journalError);
    if (!journalError.empty()) {
        setError(error, journalError);
        return false;
    }
    if (journal) {
        if (journal->phase != recovery::CrashJournalPhase::Clean ||
            journal->finalResult != recovery::CrashJournalFinalResult::Clean) {
            setError(error, "safe mode cannot be disabled until runtime recovery is verified clean");
            return false;
        }
        return journalStore.clearSafeModeAfterVerifiedReset(
            journal->sessionId, journal->runtimeGeneration, true, error);
    }

    if (marker->reason != recovery::SafeModeReason::ManualRecovery ||
        !watchdog::isZeroSessionId(marker->sessionId) ||
        marker->runtimeGeneration != 0) {
        setError(error, "session-bound safe mode cannot be disabled without a verified clean journal");
        return false;
    }
    std::uint32_t systemError = 0;
    if (!storage.remove(recovery::JournalStorageSlot::SafeMode, &systemError)) {
        if (error != nullptr) {
            *error = "manual safe-mode marker removal failed: " +
                     std::to_string(systemError);
        }
        return false;
    }
    return true;
}

std::string sessionIdHex(const watchdog::SessionId& sessionId) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(sessionId.size() * 2u);
    for (const auto byte : sessionId) {
        result.push_back(digits[(byte >> 4u) & 0x0fu]);
        result.push_back(digits[byte & 0x0fu]);
    }
    return result;
}

std::optional<watchdog::SessionId> parseSessionIdHex(
    std::string_view text,
    std::string* error) {
    if (text.size() != watchdog::SessionId{}.size() * 2u) {
        setError(error, "session id must contain exactly 32 hexadecimal characters");
        return std::nullopt;
    }
    watchdog::SessionId result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto pair = text.substr(index * 2u, 2u);
        unsigned value = 0;
        const auto parsed = std::from_chars(pair.data(), pair.data() + pair.size(),
                                            value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != pair.data() + pair.size() ||
            value > 0xffu) {
            setError(error, "session id contains a non-hexadecimal byte");
            return std::nullopt;
        }
        result[index] = static_cast<std::uint8_t>(value);
    }
    if (watchdog::isZeroSessionId(result)) {
        setError(error, "session id must not be all zero");
        return std::nullopt;
    }
    return result;
}

std::string_view resetStateName(ResetState state) noexcept {
    switch (state) {
    case ResetState::Clean: return "clean";
    case ResetState::Recoverable: return "recoverable";
    case ResetState::RecoveryRequired: return "recovery-required";
    }
    return "unknown";
}

std::string_view runtimeRegistrationReadStatusName(
    RuntimeRegistrationReadStatus status) noexcept {
    switch (status) {
    case RuntimeRegistrationReadStatus::Success: return "success";
    case RuntimeRegistrationReadStatus::Missing: return "missing";
    case RuntimeRegistrationReadStatus::TooLarge: return "too-large";
    case RuntimeRegistrationReadStatus::Corrupt: return "corrupt";
    case RuntimeRegistrationReadStatus::Failed: return "failed";
    }
    return "unknown";
}

} // namespace hydra::reset
