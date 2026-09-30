#include "hydra/hidhide_session_recovery.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hydra {
namespace {

constexpr std::size_t kHeaderBytes = 24u;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void setError(std::string* error, std::string value) {
    if (error != nullptr) *error = std::move(value);
}

class ByteWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value & 0xffu));
        u8(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32u; shift += 8u) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64u; shift += 8u) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void raw(std::span<const std::byte> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    std::vector<std::byte> take() { return std::move(bytes_); }
private:
    std::vector<std::byte> bytes_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    bool u8(std::uint8_t& value) {
        if (offset_ + 1u > bytes_.size()) return false;
        value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
        return true;
    }
    bool u16(std::uint16_t& value) {
        std::uint8_t a = 0, b = 0;
        if (!u8(a) || !u8(b)) return false;
        value = static_cast<std::uint16_t>(a) |
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(b) << 8u);
        return true;
    }
    bool u32(std::uint32_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 32u; shift += 8u) {
            std::uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool u64(std::uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 64u; shift += 8u) {
            std::uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }
    bool empty() const noexcept { return offset_ == bytes_.size(); }
    std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{0};
};

std::uint64_t hashBytes(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value = kFnvOffset;
    for (const auto byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        value *= kFnvPrime;
    }
    return value;
}

bool validText(std::wstring_view value) {
    return !value.empty() && value.size() <= kHidHideSessionMaxIdentifierChars &&
           value.find(L'\0') == std::wstring_view::npos;
}

bool validSnapshot(const HidHideSessionSnapshot& snapshot) {
    if (snapshot.blockedDeviceInstanceIds.size() > kHidHideSessionMaxDevices ||
        snapshot.allowedApplications.size() > kHidHideSessionMaxApplications) {
        return false;
    }
    return std::all_of(snapshot.blockedDeviceInstanceIds.begin(),
                       snapshot.blockedDeviceInstanceIds.end(), validText) &&
           std::all_of(snapshot.allowedApplications.begin(),
                       snapshot.allowedApplications.end(), validText);
}

bool uniqueStrings(const std::vector<std::wstring>& values) {
    for (std::size_t left = 0; left < values.size(); ++left) {
        for (std::size_t right = left + 1u; right < values.size(); ++right) {
            if (values[left] == values[right]) return false;
        }
    }
    return true;
}

void writeString(ByteWriter& writer, std::wstring_view value) {
    writer.u32(static_cast<std::uint32_t>(value.size()));
    for (const wchar_t character : value) {
        writer.u32(static_cast<std::uint32_t>(character));
    }
}

bool readString(ByteReader& reader, std::wstring& value) {
    std::uint32_t length = 0;
    if (!reader.u32(length) || length == 0 ||
        length > kHidHideSessionMaxIdentifierChars ||
        static_cast<std::uint64_t>(length) * 4u > reader.remaining()) {
        return false;
    }
    value.clear();
    value.reserve(length);
    for (std::uint32_t index = 0; index < length; ++index) {
        std::uint32_t codeUnit = 0;
        if (!reader.u32(codeUnit) || codeUnit == 0 ||
            codeUnit > static_cast<std::uint32_t>(
                std::numeric_limits<wchar_t>::max())) {
            return false;
        }
        value.push_back(static_cast<wchar_t>(codeUnit));
    }
    return true;
}

void writeSnapshot(ByteWriter& writer, const HidHideSessionSnapshot& snapshot) {
    writer.u8(snapshot.active ? 1u : 0u);
    writer.u8(snapshot.inverseWhitelist ? 1u : 0u);
    writer.u16(static_cast<std::uint16_t>(snapshot.blockedDeviceInstanceIds.size()));
    writer.u16(static_cast<std::uint16_t>(snapshot.allowedApplications.size()));
    writer.u16(0);
    for (const auto& value : snapshot.blockedDeviceInstanceIds) writeString(writer, value);
    for (const auto& value : snapshot.allowedApplications) writeString(writer, value);
}

bool readSnapshot(ByteReader& reader, HidHideSessionSnapshot& snapshot) {
    std::uint8_t active = 0, inverse = 0;
    std::uint16_t deviceCount = 0, appCount = 0, reserved = 0;
    if (!reader.u8(active) || !reader.u8(inverse) ||
        !reader.u16(deviceCount) || !reader.u16(appCount) ||
        !reader.u16(reserved) || active > 1u || inverse > 1u || reserved != 0 ||
        deviceCount > kHidHideSessionMaxDevices ||
        appCount > kHidHideSessionMaxApplications) {
        return false;
    }
    snapshot = {};
    snapshot.active = active != 0;
    snapshot.inverseWhitelist = inverse != 0;
    snapshot.blockedDeviceInstanceIds.reserve(deviceCount);
    snapshot.allowedApplications.reserve(appCount);
    for (std::uint16_t index = 0; index < deviceCount; ++index) {
        std::wstring value;
        if (!readString(reader, value)) return false;
        snapshot.blockedDeviceInstanceIds.push_back(std::move(value));
    }
    for (std::uint16_t index = 0; index < appCount; ++index) {
        std::wstring value;
        if (!readString(reader, value)) return false;
        snapshot.allowedApplications.push_back(std::move(value));
    }
    return true;
}

watchdog::RollbackActionOutcome outcome(
    const watchdog::RollbackActionDescriptor& action,
    watchdog::RollbackActionResult result,
    std::uint32_t systemError = 0) {
    return {action.actionId, action.kind, result, systemError};
}

std::uint32_t errorValue(const std::error_code& error) {
    return error ? static_cast<std::uint32_t>(error.value()) : 0u;
}

bool durableFlush(const std::filesystem::path& path) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const BOOL flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    return flushed != FALSE;
#else
    (void)path;
    return true;
#endif
}

bool replaceFile(const std::filesystem::path& from,
                 const std::filesystem::path& to,
                 std::uint32_t* systemError) {
#if defined(_WIN32)
    if (MoveFileExW(from.c_str(), to.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
        if (systemError != nullptr) *systemError = 0;
        return true;
    }
    if (systemError != nullptr) *systemError = GetLastError();
    return false;
#else
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (systemError != nullptr) *systemError = errorValue(error);
    return !error;
#endif
}

} // namespace

bool validateHidHideSessionRecoveryRecord(
    const HidHideSessionRecoveryRecord& record,
    std::string* error) {
    if (record.resourceId == 0 || record.generation == 0) {
        setError(error, "HidHide snapshot resource and generation must be nonzero");
        return false;
    }
    if (!validSnapshot(record.before) || !validSnapshot(record.applied) ||
        !uniqueStrings(record.before.blockedDeviceInstanceIds) ||
        !uniqueStrings(record.before.allowedApplications) ||
        !uniqueStrings(record.applied.blockedDeviceInstanceIds) ||
        !uniqueStrings(record.applied.allowedApplications)) {
        setError(error, "HidHide snapshot contains invalid or duplicate bounded identities");
        return false;
    }
    if (record.before.inverseWhitelist || record.applied.inverseWhitelist ||
        !record.applied.active) {
        setError(error, "HidHide guarded snapshot mode is unsupported or inactive");
        return false;
    }
    if (record.before.blockedDeviceInstanceIds !=
        record.applied.blockedDeviceInstanceIds) {
        setError(error, "HydraSeat session devices must never mutate the persistent HidHide blacklist");
        return false;
    }
    return true;
}

std::vector<std::byte> encodeHidHideSessionRecoveryRecord(
    const HidHideSessionRecoveryRecord& record) {
    if (!validateHidHideSessionRecoveryRecord(record)) return {};

    ByteWriter payload;
    payload.u64(record.resourceId);
    payload.u64(record.generation);
    writeSnapshot(payload, record.before);
    writeSnapshot(payload, record.applied);
    auto payloadBytes = payload.take();
    if (payloadBytes.size() + kHeaderBytes > kHidHideSnapshotMaxFileBytes ||
        payloadBytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }

    ByteWriter writer;
    writer.u32(kHidHideSnapshotMagic);
    writer.u16(kHidHideSnapshotVersion);
    writer.u16(0);
    writer.u32(static_cast<std::uint32_t>(payloadBytes.size()));
    writer.u32(0);
    writer.u64(hashBytes(payloadBytes));
    writer.raw(payloadBytes);
    return writer.take();
}

std::optional<HidHideSessionRecoveryRecord> decodeHidHideSessionRecoveryRecord(
    std::span<const std::byte> bytes,
    std::string* error) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kHidHideSnapshotMaxFileBytes) {
        setError(error, "HidHide snapshot file size is invalid");
        return std::nullopt;
    }

    ByteReader header(bytes.first(kHeaderBytes));
    std::uint32_t magic = 0, payloadSize = 0, reserved32 = 0;
    std::uint16_t version = 0, reserved16 = 0;
    std::uint64_t checksum = 0;
    if (!header.u32(magic) || !header.u16(version) || !header.u16(reserved16) ||
        !header.u32(payloadSize) || !header.u32(reserved32) ||
        !header.u64(checksum) || !header.empty() ||
        magic != kHidHideSnapshotMagic || version != kHidHideSnapshotVersion ||
        reserved16 != 0 || reserved32 != 0 ||
        payloadSize != bytes.size() - kHeaderBytes) {
        setError(error, "HidHide snapshot header is malformed or unsupported");
        return std::nullopt;
    }
    const auto payload = bytes.subspan(kHeaderBytes);
    if (checksum == 0 || hashBytes(payload) != checksum) {
        setError(error, "HidHide snapshot checksum mismatch");
        return std::nullopt;
    }

    ByteReader reader(payload);
    HidHideSessionRecoveryRecord record;
    if (!reader.u64(record.resourceId) || !reader.u64(record.generation) ||
        !readSnapshot(reader, record.before) ||
        !readSnapshot(reader, record.applied) || !reader.empty() ||
        !validateHidHideSessionRecoveryRecord(record, error)) {
        setError(error, error != nullptr && !error->empty()
                            ? *error
                            : "HidHide snapshot payload is malformed");
        return std::nullopt;
    }
    return record;
}

HidHideSessionRecoveryRecord makeHidHideSessionRecoveryRecord(
    const HidHideSessionPlan& plan,
    std::uint64_t resourceId) {
    return {resourceId, plan.request.generation, plan.before, plan.applied};
}

std::filesystem::path HidHideSessionSnapshotStore::directory() const {
    return recoveryRoot_ / "hidhide-snapshots";
}

std::filesystem::path HidHideSessionSnapshotStore::pathFor(
    std::uint64_t resourceId) const {
    std::ostringstream name;
    name << "snapshot-" << std::hex << std::setw(16) << std::setfill('0')
         << resourceId << ".bin";
    return directory() / name.str();
}

std::filesystem::path HidHideSessionSnapshotStore::tempPathFor(
    std::uint64_t resourceId) const {
    auto path = pathFor(resourceId);
    path += ".tmp";
    return path;
}

bool HidHideSessionSnapshotStore::write(
    const HidHideSessionRecoveryRecord& record,
    std::string* error) {
    const auto bytes = encodeHidHideSessionRecoveryRecord(record);
    if (bytes.empty()) {
        setError(error, "HidHide snapshot record is invalid");
        return false;
    }
    std::error_code fsError;
    std::filesystem::create_directories(directory(), fsError);
    if (fsError) {
        setError(error, "HidHide snapshot directory creation failed: " +
                        std::to_string(fsError.value()));
        return false;
    }

    const auto temporary = tempPathFor(record.resourceId);
    const auto current = pathFor(record.resourceId);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            setError(error, "HidHide snapshot temp file could not be opened");
            return false;
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            setError(error, "HidHide snapshot temp file write failed");
            return false;
        }
    }
    if (!durableFlush(temporary)) {
        setError(error, "HidHide snapshot temp file flush failed");
        return false;
    }
    std::uint32_t replaceError = 0;
    if (!replaceFile(temporary, current, &replaceError)) {
        setError(error, "HidHide snapshot atomic replace failed: " +
                        std::to_string(replaceError));
        return false;
    }
    return true;
}

HidHideSnapshotReadResult HidHideSessionSnapshotStore::load(
    std::uint64_t resourceId) const {
    if (resourceId == 0) {
        return {HidHideSnapshotReadStatus::Corrupt, std::nullopt, 0,
                "HidHide snapshot resource ID is zero"};
    }
    const auto path = pathFor(resourceId);
    std::error_code fsError;
    const auto size = std::filesystem::file_size(path, fsError);
    if (fsError) {
        if (fsError == std::errc::no_such_file_or_directory) {
            return {HidHideSnapshotReadStatus::Missing, std::nullopt,
                    errorValue(fsError), "HidHide snapshot is missing"};
        }
        return {HidHideSnapshotReadStatus::Failed, std::nullopt,
                errorValue(fsError), "HidHide snapshot size query failed"};
    }
    if (size > kHidHideSnapshotMaxFileBytes) {
        return {HidHideSnapshotReadStatus::TooLarge, std::nullopt, 0,
                "HidHide snapshot exceeds the file-size bound"};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {HidHideSnapshotReadStatus::Failed, std::nullopt, 0,
                "HidHide snapshot could not be opened"};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return {HidHideSnapshotReadStatus::Failed, std::nullopt, 0,
                "HidHide snapshot read was incomplete"};
    }
    std::string decodeError;
    auto record = decodeHidHideSessionRecoveryRecord(bytes, &decodeError);
    if (!record || record->resourceId != resourceId) {
        return {HidHideSnapshotReadStatus::Corrupt, std::nullopt, 0,
                record ? "HidHide snapshot resource identity mismatch"
                       : std::move(decodeError)};
    }
    return {HidHideSnapshotReadStatus::Success, std::move(record), 0,
            "HidHide snapshot loaded"};
}

bool HidHideSessionSnapshotStore::remove(
    std::uint64_t resourceId,
    std::string* error) {
    if (resourceId == 0) {
        setError(error, "HidHide snapshot resource ID is zero");
        return false;
    }
    std::error_code fsError;
    const bool removed = std::filesystem::remove(pathFor(resourceId), fsError);
    if (fsError) {
        setError(error, "HidHide snapshot removal failed: " +
                        std::to_string(fsError.value()));
        return false;
    }
    if (!removed && std::filesystem::exists(pathFor(resourceId), fsError)) {
        setError(error, "HidHide snapshot could not be removed");
        return false;
    }
    std::filesystem::remove(tempPathFor(resourceId), fsError);
    return true;
}

GuardedHidHideSession::GuardedHidHideSession(
    std::shared_ptr<HidHideSessionPlatform> platform,
    std::filesystem::path recoveryRoot,
    std::uint64_t resourceId,
    std::uint32_t actionId,
    std::uint32_t activationOrdinal,
    std::uint32_t rollbackTimeoutMilliseconds)
    : transaction_(std::move(platform)),
      store_(std::move(recoveryRoot)),
      resourceId_(resourceId),
      actionId_(actionId),
      activationOrdinal_(activationOrdinal),
      rollbackTimeoutMilliseconds_(rollbackTimeoutMilliseconds) {}

GuardedHidHideSession::~GuardedHidHideSession() {
    (void)rollback();
}

HidHideSessionResult GuardedHidHideSession::prepare(
    HidHideSessionRequest request,
    std::uint64_t nowMilliseconds) {
    if (resourceId_ == 0 || actionId_ == 0 || activationOrdinal_ == 0 ||
        rollbackTimeoutMilliseconds_ < watchdog::kWatchdogMinActionTimeoutMs ||
        rollbackTimeoutMilliseconds_ > watchdog::kWatchdogMaxActionTimeoutMs) {
        return {HidHideSessionResultCode::InvalidRequest, transaction_.phase(),
                transaction_.plan(),
                "guarded HidHide recovery identity or timeout is invalid"};
    }

    recoveryArmed_ = false;
    rollbackAction_.reset();
    auto prepared = transaction_.prepare(std::move(request), nowMilliseconds);
    if (!prepared.succeeded() || !prepared.plan) return prepared;

    const auto record = makeHidHideSessionRecoveryRecord(
        *prepared.plan, resourceId_);
    std::string error;
    if (!store_.write(record, &error)) {
        (void)transaction_.rollback();
        return {HidHideSessionResultCode::BackendFailure,
                transaction_.phase(), transaction_.plan(),
                "durable HidHide recovery snapshot write failed: " + error};
    }

    rollbackAction_ = makeHidHideSessionRollbackAction(
        actionId_, activationOrdinal_, rollbackTimeoutMilliseconds_,
        prepared.plan->request.generation, resourceId_);
    prepared.diagnostic += "; durable recovery snapshot persisted";
    return prepared;
}

bool GuardedHidHideSession::confirmRecoveryArmed(
    const watchdog::RollbackActionDescriptor& registeredAction,
    std::string* error) {
    if (!rollbackAction_ || transaction_.phase() != HidHideSessionPhase::Prepared) {
        setError(error, "guarded HidHide session has no prepared recovery action");
        return false;
    }
    if (registeredAction != *rollbackAction_) {
        setError(error, "registered watchdog action does not match the exact guarded HidHide recovery action");
        return false;
    }
    recoveryArmed_ = true;
    if (error != nullptr) error->clear();
    return true;
}

HidHideSessionResult GuardedHidHideSession::activate(
    std::uint64_t nowMilliseconds) {
    if (!recoveryArmed_ || !rollbackAction_) {
        return {HidHideSessionResultCode::RecoveryNotArmed,
                transaction_.phase(), transaction_.plan(),
                "native HidHide activation requires the exact durable rollback action to be armed first"};
    }
    return cleanupRecoveryRecordIfSafe(transaction_.activate(nowMilliseconds));
}

HidHideSessionResult GuardedHidHideSession::expireIfNeeded(
    std::uint64_t nowMilliseconds) {
    return cleanupRecoveryRecordIfSafe(
        transaction_.expireIfNeeded(nowMilliseconds));
}

HidHideSessionResult GuardedHidHideSession::rollback() {
    return cleanupRecoveryRecordIfSafe(transaction_.rollback());
}

HidHideSessionResult GuardedHidHideSession::cleanupRecoveryRecordIfSafe(
    HidHideSessionResult result) {
    if (result.phase != HidHideSessionPhase::Idle) return result;

    std::string error;
    if (rollbackAction_ && !store_.remove(resourceId_, &error)) {
        result.code = HidHideSessionResultCode::BackendFailure;
        result.diagnostic += "; recovery snapshot cleanup failed: " + error;
        return result;
    }
    rollbackAction_.reset();
    recoveryArmed_ = false;
    return result;
}

HidHideSessionRollbackExecutor::HidHideSessionRollbackExecutor(
    std::filesystem::path recoveryRoot,
    std::shared_ptr<HidHideSessionPlatform> platform)
    : store_(std::move(recoveryRoot)), platform_(std::move(platform)) {}

bool HidHideSessionRollbackExecutor::prepareOwnedProcesses(
    std::span<const watchdog::RollbackActionDescriptor> actions,
    std::string* error) {
    return delegate_.prepareOwnedProcesses(actions, error);
}

void HidHideSessionRollbackExecutor::clearPreparedOwnedProcesses() noexcept {
    delegate_.clearPreparedOwnedProcesses();
}

watchdog::RollbackActionOutcome HidHideSessionRollbackExecutor::terminateOwnedProcess(
    const watchdog::RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    return delegate_.terminateOwnedProcess(action, timeoutMilliseconds);
}

watchdog::RollbackActionOutcome HidHideSessionRollbackExecutor::closeOwnedSession(
    const watchdog::RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    return delegate_.closeOwnedSession(action, timeoutMilliseconds);
}

watchdog::RollbackActionOutcome HidHideSessionRollbackExecutor::clearOptionalBackendState(
    const watchdog::RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    return delegate_.clearOptionalBackendState(action, timeoutMilliseconds);
}

watchdog::RollbackActionOutcome HidHideSessionRollbackExecutor::releaseOverlayState(
    const watchdog::RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    return delegate_.releaseOverlayState(action, timeoutMilliseconds);
}

watchdog::RollbackActionOutcome HidHideSessionRollbackExecutor::restoreSnapshotState(
    const watchdog::RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    (void)timeoutMilliseconds;
    if (action.kind != watchdog::RollbackActionKind::RestoreSnapshotState ||
        action.resourceId == 0 || action.generation == 0) {
        return outcome(action, watchdog::RollbackActionResult::InvalidAction);
    }
    const auto loaded = store_.load(action.resourceId);
    if (loaded.status != HidHideSnapshotReadStatus::Success || !loaded.record) {
        return outcome(action, watchdog::RollbackActionResult::Failed,
                       loaded.systemError);
    }
    const auto& record = *loaded.record;
    if (record.generation != action.generation) {
        return outcome(action, watchdog::RollbackActionResult::IdentityMismatch);
    }
    if (!platform_) {
        return outcome(action, watchdog::RollbackActionResult::Unsupported);
    }

    std::string error;
    HidHideSessionSnapshot current;
    if (!platform_->readState(current, error)) {
        return outcome(action, watchdog::RollbackActionResult::Failed);
    }
    if (equivalentHidHideSessionSnapshots(current, record.before)) {
        return outcome(action, watchdog::RollbackActionResult::AlreadySatisfied);
    }
    if (!equivalentHidHideSessionSnapshots(current, record.applied)) {
        return outcome(action, watchdog::RollbackActionResult::IdentityMismatch);
    }
    if (!platform_->mutationSupported()) {
        return outcome(action, watchdog::RollbackActionResult::Unsupported);
    }
    if (!platform_->writeState(record.before, error)) {
        return outcome(action, watchdog::RollbackActionResult::Failed);
    }
    HidHideSessionSnapshot restored;
    if (!platform_->readState(restored, error) ||
        !equivalentHidHideSessionSnapshots(restored, record.before)) {
        return outcome(action, watchdog::RollbackActionResult::Failed);
    }
    return outcome(action, watchdog::RollbackActionResult::Success);
}

watchdog::RollbackActionOutcome HidHideSessionRollbackExecutor::writeSafeModeResult(
    const watchdog::RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    return delegate_.writeSafeModeResult(action, timeoutMilliseconds);
}

std::string_view hidHideSnapshotReadStatusName(
    HidHideSnapshotReadStatus status) noexcept {
    switch (status) {
        case HidHideSnapshotReadStatus::Success: return "success";
        case HidHideSnapshotReadStatus::Missing: return "missing";
        case HidHideSnapshotReadStatus::TooLarge: return "too-large";
        case HidHideSnapshotReadStatus::Corrupt: return "corrupt";
        case HidHideSnapshotReadStatus::Failed: return "failed";
    }
    return "unknown";
}

} // namespace hydra
