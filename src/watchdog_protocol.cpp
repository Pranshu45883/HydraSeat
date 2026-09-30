#include "hydra/watchdog_protocol.hpp"
#include "hydra/recovery_process_attachment.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace hydra::watchdog {
namespace {

constexpr std::size_t kLeasePayloadBytes = 28;
constexpr std::size_t kManifestPrefixBytes = 36;
constexpr std::size_t kActionBytes = 48;
constexpr std::size_t kStatusPayloadBytes = 40;

void setError(std::string* error, std::string_view value) {
    if (error != nullptr) {
        *error = value;
    }
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

    void session(const SessionId& value) {
        for (const auto byte : value) {
            u8(byte);
        }
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
        value = std::to_integer<std::uint8_t>(m_bytes[m_offset]);
        ++m_offset;
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

    bool session(SessionId& value) {
        for (auto& byte : value) {
            if (!u8(byte)) return false;
        }
        return true;
    }

    std::size_t remaining() const noexcept { return m_bytes.size() - m_offset; }
    bool empty() const noexcept { return remaining() == 0; }

private:
    std::span<const std::byte> m_bytes;
    std::size_t m_offset{0};
};

bool knownMessageType(WatchdogMessageType type) noexcept {
    switch (type) {
    case WatchdogMessageType::RegisterPlan:
    case WatchdogMessageType::RenewLease:
    case WatchdogMessageType::Disarm:
    case WatchdogMessageType::Status:
        return true;
    }
    return false;
}

bool knownRunState(WatchdogRunState state) noexcept {
    switch (state) {
    case WatchdogRunState::WaitingForPlan:
    case WatchdogRunState::Armed:
    case WatchdogRunState::RollingBack:
    case WatchdogRunState::Disarmed:
    case WatchdogRunState::RollbackComplete:
    case WatchdogRunState::RecoveryRequired:
        return true;
    }
    return false;
}

bool knownTriggerReason(WatchdogTriggerReason reason) noexcept {
    switch (reason) {
    case WatchdogTriggerReason::None:
    case WatchdogTriggerReason::HostExited:
    case WatchdogTriggerReason::LeaseExpired:
    case WatchdogTriggerReason::ControlChannelClosed:
    case WatchdogTriggerReason::ProtocolViolation:
    case WatchdogTriggerReason::RollbackFailure:
    case WatchdogTriggerReason::CleanDisarm:
        return true;
    }
    return false;
}

std::vector<std::byte> encodeLeasePayload(const WatchdogLease& lease) {
    ByteWriter writer(kLeasePayloadBytes);
    writer.session(lease.sessionId);
    writer.u64(lease.generation);
    writer.u32(lease.timeoutMilliseconds);
    return writer.take();
}

bool decodeLeasePayload(std::span<const std::byte> payload,
                        WatchdogLease& lease,
                        std::string* error) {
    if (payload.size() != kLeasePayloadBytes) {
        setError(error, "watchdog lease payload size mismatch");
        return false;
    }
    ByteReader reader(payload);
    if (!reader.session(lease.sessionId) ||
        !reader.u64(lease.generation) ||
        !reader.u32(lease.timeoutMilliseconds) || !reader.empty()) {
        setError(error, "watchdog lease payload truncated");
        return false;
    }
    if (isZeroSessionId(lease.sessionId) || lease.generation == 0 ||
        lease.timeoutMilliseconds < kWatchdogMinLeaseTimeoutMs ||
        lease.timeoutMilliseconds > kWatchdogMaxLeaseTimeoutMs) {
        setError(error, "watchdog lease fields are invalid");
        return false;
    }
    return true;
}

} // namespace

bool isZeroSessionId(const SessionId& sessionId) noexcept {
    return std::all_of(sessionId.begin(), sessionId.end(),
                       [](std::uint8_t value) { return value == 0; });
}

bool isKnownRollbackActionKind(RollbackActionKind kind) noexcept {
    switch (kind) {
    case RollbackActionKind::TerminateOwnedProcess:
    case RollbackActionKind::CloseOwnedSession:
    case RollbackActionKind::ClearOptionalBackendState:
    case RollbackActionKind::ReleaseOverlayState:
    case RollbackActionKind::RestoreSnapshotState:
    case RollbackActionKind::WriteSafeModeResult:
        return true;
    }
    return false;
}

bool validateRollbackPlan(const RollbackPlanManifest& manifest,
                          std::string* error) {
    if (isZeroSessionId(manifest.lease.sessionId)) {
        setError(error, "rollback manifest session id is zero");
        return false;
    }
    if (manifest.lease.generation == 0) {
        setError(error, "rollback manifest lease generation is zero");
        return false;
    }
    if (manifest.lease.timeoutMilliseconds < kWatchdogMinLeaseTimeoutMs ||
        manifest.lease.timeoutMilliseconds > kWatchdogMaxLeaseTimeoutMs) {
        setError(error, "rollback manifest lease timeout is out of range");
        return false;
    }
    if (manifest.rollbackTimeoutMilliseconds < kWatchdogMinActionTimeoutMs ||
        manifest.rollbackTimeoutMilliseconds > kWatchdogMaxRollbackTimeoutMs) {
        setError(error, "rollback manifest total timeout is out of range");
        return false;
    }
    if (manifest.actions.empty() ||
        manifest.actions.size() > kWatchdogMaxRollbackActions) {
        setError(error, "rollback manifest action count is out of range");
        return false;
    }

    std::unordered_set<std::uint32_t> actionIds;
    std::unordered_set<std::uint32_t> ordinals;
    std::uint32_t maximumActionTimeout = 0;
    for (const auto& action : manifest.actions) {
        if (action.actionId == 0 || action.activationOrdinal == 0 ||
            action.generation == 0) {
            setError(error, "rollback action identity fields must be nonzero");
            return false;
        }
        if (!actionIds.insert(action.actionId).second ||
            !ordinals.insert(action.activationOrdinal).second) {
            setError(error, "rollback action ids and activation ordinals must be unique");
            return false;
        }
        if (!isKnownRollbackActionKind(action.kind)) {
            setError(error, "rollback action kind is unknown");
            return false;
        }
        if (action.timeoutMilliseconds < kWatchdogMinActionTimeoutMs ||
            action.timeoutMilliseconds > kWatchdogMaxActionTimeoutMs) {
            setError(error, "rollback action timeout is out of range");
            return false;
        }
        maximumActionTimeout =
            std::max(maximumActionTimeout, action.timeoutMilliseconds);

        if (action.kind == RollbackActionKind::TerminateOwnedProcess) {
            if (action.resourceId != 0 || action.process.processId == 0 ||
                action.process.creationTime100ns == 0) {
                setError(error, "process rollback action identity is invalid");
                return false;
            }
        } else if (action.resourceId == 0 || action.process.processId != 0 ||
                   action.process.creationTime100ns != 0) {
            setError(error, "resource rollback action identity is invalid");
            return false;
        }
    }

    if (manifest.rollbackTimeoutMilliseconds < maximumActionTimeout) {
        setError(error, "rollback total timeout is shorter than an action timeout");
        return false;
    }
    return true;
}

std::vector<std::byte> encodeWatchdogFrame(
    WatchdogMessageType type,
    std::uint64_t sequence,
    std::span<const std::byte> payload) {
    if (!knownMessageType(type) || sequence == 0 ||
        payload.size() > kWatchdogMaxPayloadBytes ||
        payload.size() > static_cast<std::size_t>(
                             std::numeric_limits<std::uint32_t>::max())) {
        return {};
    }

    ByteWriter writer(kWatchdogFrameHeaderBytes + payload.size());
    writer.u32(kWatchdogProtocolMagic);
    writer.u16(kWatchdogProtocolVersion);
    writer.u16(static_cast<std::uint16_t>(type));
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.u32(0);
    writer.u64(sequence);
    writer.raw(payload);
    return writer.take();
}

WatchdogFrameDecodeResult decodeWatchdogFrame(
    std::span<const std::byte> frameBytes) {
    WatchdogFrameDecodeResult result;
    if (frameBytes.size() < kWatchdogFrameHeaderBytes ||
        frameBytes.size() > kWatchdogMaxFrameBytes) {
        result.error = "watchdog frame size is out of range";
        return result;
    }

    ByteReader reader(frameBytes.first(kWatchdogFrameHeaderBytes));
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t typeValue = 0;
    std::uint32_t payloadBytes = 0;
    std::uint32_t reserved = 0;
    std::uint64_t sequence = 0;
    if (!reader.u32(magic) || !reader.u16(version) || !reader.u16(typeValue) ||
        !reader.u32(payloadBytes) || !reader.u32(reserved) ||
        !reader.u64(sequence) || !reader.empty()) {
        result.error = "watchdog frame header is truncated";
        return result;
    }
    if (magic != kWatchdogProtocolMagic) {
        result.error = "watchdog frame magic mismatch";
        return result;
    }
    if (version != kWatchdogProtocolVersion) {
        result.error = "watchdog frame version mismatch";
        return result;
    }
    if (reserved != 0 || sequence == 0) {
        result.error = "watchdog frame reserved/sequence field is invalid";
        return result;
    }
    if (payloadBytes > kWatchdogMaxPayloadBytes ||
        static_cast<std::size_t>(payloadBytes) !=
            frameBytes.size() - kWatchdogFrameHeaderBytes) {
        result.error = "watchdog frame payload length mismatch";
        return result;
    }

    const auto type = static_cast<WatchdogMessageType>(typeValue);
    if (!knownMessageType(type)) {
        result.error = "watchdog frame message type is unknown";
        return result;
    }

    DecodedWatchdogFrame frame;
    frame.type = type;
    frame.sequence = sequence;
    const auto payload = frameBytes.subspan(kWatchdogFrameHeaderBytes);
    frame.payload.assign(payload.begin(), payload.end());
    result.frame = std::move(frame);
    return result;
}

std::vector<std::byte> encodeRegisterPlan(
    std::uint64_t sequence,
    const RollbackPlanManifest& manifest) {
    if (!validateRollbackPlan(manifest)) return {};

    const std::size_t payloadBytes =
        kManifestPrefixBytes + manifest.actions.size() * kActionBytes;
    ByteWriter writer(payloadBytes);
    writer.session(manifest.lease.sessionId);
    writer.u64(manifest.lease.generation);
    writer.u32(manifest.lease.timeoutMilliseconds);
    writer.u32(manifest.rollbackTimeoutMilliseconds);
    writer.u16(static_cast<std::uint16_t>(manifest.actions.size()));
    writer.u16(0);
    for (const auto& action : manifest.actions) {
        writer.u32(action.actionId);
        writer.u16(static_cast<std::uint16_t>(action.kind));
        writer.u16(0);
        writer.u32(action.activationOrdinal);
        writer.u32(action.timeoutMilliseconds);
        writer.u64(action.generation);
        writer.u64(action.resourceId);
        writer.u32(action.process.processId);
        writer.u32(0);
        writer.u64(action.process.creationTime100ns);
    }
    const auto payload = writer.take();
    return encodeWatchdogFrame(WatchdogMessageType::RegisterPlan,
                               sequence, payload);
}

bool decodeRegisterPlan(const DecodedWatchdogFrame& frame,
                        RollbackPlanManifest& manifest,
                        std::string* error) {
    if (frame.type != WatchdogMessageType::RegisterPlan) {
        setError(error, "watchdog frame is not a register-plan message");
        return false;
    }
    if (frame.payload.size() < kManifestPrefixBytes) {
        setError(error, "rollback manifest payload is truncated");
        return false;
    }

    ByteReader reader(frame.payload);
    RollbackPlanManifest decoded;
    std::uint16_t actionCount = 0;
    std::uint16_t reserved = 0;
    if (!reader.session(decoded.lease.sessionId) ||
        !reader.u64(decoded.lease.generation) ||
        !reader.u32(decoded.lease.timeoutMilliseconds) ||
        !reader.u32(decoded.rollbackTimeoutMilliseconds) ||
        !reader.u16(actionCount) || !reader.u16(reserved)) {
        setError(error, "rollback manifest header is truncated");
        return false;
    }
    if (reserved != 0 || actionCount == 0 ||
        actionCount > kWatchdogMaxRollbackActions) {
        setError(error, "rollback manifest action count/reserved field is invalid");
        return false;
    }
    const auto expectedBytes = kManifestPrefixBytes +
        static_cast<std::size_t>(actionCount) * kActionBytes;
    if (frame.payload.size() != expectedBytes) {
        setError(error, "rollback manifest payload size mismatch");
        return false;
    }

    decoded.actions.reserve(actionCount);
    for (std::uint16_t index = 0; index < actionCount; ++index) {
        RollbackActionDescriptor action;
        std::uint16_t kindValue = 0;
        std::uint16_t actionReserved = 0;
        std::uint32_t processReserved = 0;
        if (!reader.u32(action.actionId) || !reader.u16(kindValue) ||
            !reader.u16(actionReserved) ||
            !reader.u32(action.activationOrdinal) ||
            !reader.u32(action.timeoutMilliseconds) ||
            !reader.u64(action.generation) || !reader.u64(action.resourceId) ||
            !reader.u32(action.process.processId) ||
            !reader.u32(processReserved) ||
            !reader.u64(action.process.creationTime100ns)) {
            setError(error, "rollback action payload is truncated");
            return false;
        }
        if (actionReserved != 0 || processReserved != 0) {
            setError(error, "rollback action reserved field is nonzero");
            return false;
        }
        action.kind = static_cast<RollbackActionKind>(kindValue);
        decoded.actions.push_back(action);
    }
    if (!reader.empty()) {
        setError(error, "rollback manifest has trailing bytes");
        return false;
    }
    if (!validateRollbackPlan(decoded, error)) {
        return false;
    }
    manifest = std::move(decoded);
    return true;
}

std::vector<std::byte> encodeLeaseRenewal(
    std::uint64_t sequence,
    const WatchdogLease& lease) {
    const auto payload = encodeLeasePayload(lease);
    WatchdogLease validated;
    if (!decodeLeasePayload(payload, validated, nullptr)) return {};
    return encodeWatchdogFrame(WatchdogMessageType::RenewLease, sequence, payload);
}

bool decodeLeaseRenewal(const DecodedWatchdogFrame& frame,
                        WatchdogLease& lease,
                        std::string* error) {
    if (frame.type != WatchdogMessageType::RenewLease) {
        setError(error, "watchdog frame is not a lease-renewal message");
        return false;
    }
    return decodeLeasePayload(frame.payload, lease, error);
}

std::vector<std::byte> encodeDisarm(
    std::uint64_t sequence,
    const WatchdogLease& lease) {
    const auto payload = encodeLeasePayload(lease);
    WatchdogLease validated;
    if (!decodeLeasePayload(payload, validated, nullptr)) return {};
    return encodeWatchdogFrame(WatchdogMessageType::Disarm, sequence, payload);
}

bool decodeDisarm(const DecodedWatchdogFrame& frame,
                  WatchdogLease& lease,
                  std::string* error) {
    if (frame.type != WatchdogMessageType::Disarm) {
        setError(error, "watchdog frame is not a disarm message");
        return false;
    }
    return decodeLeasePayload(frame.payload, lease, error);
}

std::vector<std::byte> encodeWatchdogStatus(
    std::uint64_t sequence,
    const WatchdogStatus& status) {
    if (isZeroSessionId(status.sessionId) || status.generation == 0 ||
        !knownRunState(status.state) || !knownTriggerReason(status.reason) ||
        status.completedActions > status.totalActions) {
        return {};
    }
    ByteWriter writer(kStatusPayloadBytes);
    writer.session(status.sessionId);
    writer.u64(status.generation);
    writer.u16(static_cast<std::uint16_t>(status.state));
    writer.u16(static_cast<std::uint16_t>(status.reason));
    writer.u16(status.completedActions);
    writer.u16(status.totalActions);
    writer.u32(status.failedActionId);
    writer.u32(status.systemError);
    const auto payload = writer.take();
    return encodeWatchdogFrame(WatchdogMessageType::Status, sequence, payload);
}

bool decodeWatchdogStatus(const DecodedWatchdogFrame& frame,
                          WatchdogStatus& status,
                          std::string* error) {
    if (frame.type != WatchdogMessageType::Status) {
        setError(error, "watchdog frame is not a status message");
        return false;
    }
    if (frame.payload.size() != kStatusPayloadBytes) {
        setError(error, "watchdog status payload size mismatch");
        return false;
    }
    ByteReader reader(frame.payload);
    WatchdogStatus decoded;
    std::uint16_t stateValue = 0;
    std::uint16_t reasonValue = 0;
    if (!reader.session(decoded.sessionId) || !reader.u64(decoded.generation) ||
        !reader.u16(stateValue) || !reader.u16(reasonValue) ||
        !reader.u16(decoded.completedActions) ||
        !reader.u16(decoded.totalActions) ||
        !reader.u32(decoded.failedActionId) ||
        !reader.u32(decoded.systemError) || !reader.empty()) {
        setError(error, "watchdog status payload is truncated");
        return false;
    }
    decoded.state = static_cast<WatchdogRunState>(stateValue);
    decoded.reason = static_cast<WatchdogTriggerReason>(reasonValue);
    if (isZeroSessionId(decoded.sessionId) || decoded.generation == 0 ||
        !knownRunState(decoded.state) || !knownTriggerReason(decoded.reason) ||
        decoded.completedActions > decoded.totalActions) {
        setError(error, "watchdog status fields are invalid");
        return false;
    }
    status = decoded;
    return true;
}

std::string_view watchdogRunStateName(WatchdogRunState state) noexcept {
    switch (state) {
    case WatchdogRunState::WaitingForPlan:
        return "waiting-for-plan";
    case WatchdogRunState::Armed:
        return "armed";
    case WatchdogRunState::RollingBack:
        return "rolling-back";
    case WatchdogRunState::Disarmed:
        return "disarmed";
    case WatchdogRunState::RollbackComplete:
        return "rollback-complete";
    case WatchdogRunState::RecoveryRequired:
        return "recovery-required";
    }
    return "unknown";
}

std::string_view watchdogTriggerReasonName(
    WatchdogTriggerReason reason) noexcept {
    switch (reason) {
    case WatchdogTriggerReason::None:
        return "none";
    case WatchdogTriggerReason::HostExited:
        return "host-exited";
    case WatchdogTriggerReason::LeaseExpired:
        return "lease-expired";
    case WatchdogTriggerReason::ControlChannelClosed:
        return "control-channel-closed";
    case WatchdogTriggerReason::ProtocolViolation:
        return "protocol-violation";
    case WatchdogTriggerReason::RollbackFailure:
        return "rollback-failure";
    case WatchdogTriggerReason::CleanDisarm:
        return "clean-disarm";
    }
    return "unknown";
}

} // namespace hydra::watchdog

namespace hydra::recovery {
namespace {

void setAttachmentError(std::string* error, std::string_view value) {
    if (error != nullptr) *error = value;
}

bool zeroRuntimeSession(const runtime::RuntimeSessionId& sessionId) noexcept {
    return std::all_of(sessionId.bytes.begin(), sessionId.bytes.end(),
                       [](std::uint8_t value) { return value == 0; });
}

bool sameWatchdogLeaseIdentity(const watchdog::WatchdogLease& left,
                               const watchdog::WatchdogLease& right) noexcept {
    return left.sessionId == right.sessionId && left.generation == right.generation;
}

void appendU16(std::vector<std::byte>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xffu));
    bytes.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
}

void appendU32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

void appendU64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

bool readU16(std::span<const std::byte> bytes, std::size_t& offset,
             std::uint16_t& value) {
    if (bytes.size() - offset < 2u) return false;
    value = static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(bytes[offset]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1u]))
            << 8u));
    offset += 2u;
    return true;
}

bool readU32(std::span<const std::byte> bytes, std::size_t& offset,
             std::uint32_t& value) {
    if (bytes.size() - offset < 4u) return false;
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(bytes[offset++])) << shift;
    }
    return true;
}

bool readU64(std::span<const std::byte> bytes, std::size_t& offset,
             std::uint64_t& value) {
    if (bytes.size() - offset < 8u) return false;
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(bytes[offset++])) << shift;
    }
    return true;
}

RecoveryAttachmentResult attachmentResult(
    RecoveryAttachmentCode code,
    std::optional<RecoveryProcessAttachmentRegistration> current,
    std::string diagnostic) {
    RecoveryAttachmentResult result;
    result.code = code;
    result.current = std::move(current);
    result.diagnostic = std::move(diagnostic);
    if (result.diagnostic.size() > 2048u) result.diagnostic.resize(2048u);
    return result;
}

} // namespace

bool validateRecoveryProcessAttachmentIdentity(
    const RecoveryProcessAttachmentIdentity& identity,
    std::string* error) {
    if (identity.schemaVersion != kRecoveryProcessAttachmentVersion) {
        setAttachmentError(error, "recovery attachment schema version is unsupported");
        return false;
    }
    if (identity.seatId == 0u || identity.seatId > kMaximumRecoveryProcessAttachments) {
        setAttachmentError(error, "recovery attachment Seat is outside the v1 two-Seat bound");
        return false;
    }
    if (zeroRuntimeSession(identity.hostSessionId)) {
        setAttachmentError(error, "recovery attachment host session is zero");
        return false;
    }
    if (identity.sessionGeneration == 0u || identity.seatGameGeneration == 0u ||
        identity.recoveryEpoch == 0u) {
        setAttachmentError(error, "recovery attachment generation fields must be nonzero");
        return false;
    }
    if (identity.process.processId == 0u ||
        identity.process.creationTime100ns == 0u) {
        setAttachmentError(error, "recovery attachment requires exact PID plus creation time");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

std::vector<std::byte> encodeRecoveryProcessAttachmentIdentity(
    const RecoveryProcessAttachmentIdentity& identity) {
    if (!validateRecoveryProcessAttachmentIdentity(identity, nullptr)) return {};
    std::vector<std::byte> bytes;
    bytes.reserve(kRecoveryProcessAttachmentIdentityBytes);
    appendU32(bytes, kRecoveryProcessAttachmentMagic);
    appendU16(bytes, identity.schemaVersion);
    appendU16(bytes, 0u);
    appendU32(bytes, identity.seatId);
    for (const auto value : identity.hostSessionId.bytes) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    appendU64(bytes, identity.sessionGeneration);
    appendU64(bytes, identity.seatGameGeneration);
    appendU32(bytes, identity.process.processId);
    appendU32(bytes, 0u);
    appendU64(bytes, identity.process.creationTime100ns);
    appendU64(bytes, identity.recoveryEpoch);
    if (bytes.size() != kRecoveryProcessAttachmentIdentityBytes) return {};
    return bytes;
}

std::optional<RecoveryProcessAttachmentIdentity>
decodeRecoveryProcessAttachmentIdentity(
    std::span<const std::byte> bytes,
    std::string* error) {
    if (bytes.size() != kRecoveryProcessAttachmentIdentityBytes) {
        setAttachmentError(error, "recovery attachment identity size is invalid");
        return std::nullopt;
    }
    std::size_t offset = 0u;
    std::uint32_t magic = 0u;
    std::uint16_t version = 0u;
    std::uint16_t reserved16 = 0u;
    std::uint32_t reserved32 = 0u;
    RecoveryProcessAttachmentIdentity identity;
    if (!readU32(bytes, offset, magic) || !readU16(bytes, offset, version) ||
        !readU16(bytes, offset, reserved16) ||
        !readU32(bytes, offset, identity.seatId)) {
        setAttachmentError(error, "recovery attachment identity header is truncated");
        return std::nullopt;
    }
    if (offset + identity.hostSessionId.bytes.size() > bytes.size()) {
        setAttachmentError(error, "recovery attachment host session is truncated");
        return std::nullopt;
    }
    for (auto& value : identity.hostSessionId.bytes) {
        value = std::to_integer<std::uint8_t>(bytes[offset++]);
    }
    if (!readU64(bytes, offset, identity.sessionGeneration) ||
        !readU64(bytes, offset, identity.seatGameGeneration) ||
        !readU32(bytes, offset, identity.process.processId) ||
        !readU32(bytes, offset, reserved32) ||
        !readU64(bytes, offset, identity.process.creationTime100ns) ||
        !readU64(bytes, offset, identity.recoveryEpoch) || offset != bytes.size()) {
        setAttachmentError(error, "recovery attachment identity payload is truncated");
        return std::nullopt;
    }
    if (magic != kRecoveryProcessAttachmentMagic ||
        version != kRecoveryProcessAttachmentVersion ||
        reserved16 != 0u || reserved32 != 0u) {
        setAttachmentError(error, "recovery attachment magic/version/reserved field is invalid");
        return std::nullopt;
    }
    identity.schemaVersion = version;
    if (!validateRecoveryProcessAttachmentIdentity(identity, error)) {
        return std::nullopt;
    }
    return identity;
}

bool validateRecoveryProcessAttachmentRegistration(
    const RecoveryProcessAttachmentRegistration& registration,
    std::string* error) {
    if (!validateRecoveryProcessAttachmentIdentity(registration.identity, error)) {
        return false;
    }
    if (!watchdog::validateRollbackPlan(registration.manifest, error)) {
        return false;
    }
    if (registration.manifest.lease.generation !=
        registration.identity.recoveryEpoch) {
        setAttachmentError(error, "recovery attachment epoch does not match watchdog lease generation");
        return false;
    }

    std::size_t attachedProcessActions = 0u;
    for (const auto& action : registration.manifest.actions) {
        if (action.generation != registration.identity.recoveryEpoch) {
            setAttachmentError(error, "recovery attachment action generation does not match its epoch");
            return false;
        }
        if (action.kind != watchdog::RollbackActionKind::TerminateOwnedProcess) {
            continue;
        }
        ++attachedProcessActions;
        if (action.process != registration.identity.process) {
            setAttachmentError(error, "recovery attachment manifest targets a different process identity");
            return false;
        }
    }
    if (attachedProcessActions != 1u) {
        setAttachmentError(error, "recovery attachment manifest must contain exactly one exact-process action");
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

RecoveryAttachmentResult RecoveryProcessAttachmentAuthority::compareAgainstCurrent(
    const RecoveryProcessAttachmentRegistration& current,
    const RecoveryProcessAttachmentIdentity& identity,
    const watchdog::WatchdogLease& lease) const {
    if (identity.seatId != current.identity.seatId) {
        return attachmentResult(RecoveryAttachmentCode::SeatMismatch, current,
                                "recovery attachment belongs to a different Seat");
    }
    if (identity.hostSessionId != current.identity.hostSessionId) {
        return attachmentResult(RecoveryAttachmentCode::SessionMismatch, current,
                                "recovery attachment belongs to a different host session");
    }
    if (identity.sessionGeneration < current.identity.sessionGeneration) {
        return attachmentResult(RecoveryAttachmentCode::StaleSessionGeneration, current,
                                "recovery attachment session generation is stale");
    }
    if (identity.sessionGeneration != current.identity.sessionGeneration) {
        return attachmentResult(RecoveryAttachmentCode::SessionGenerationMismatch, current,
                                "recovery attachment session generation does not match");
    }
    if (identity.seatGameGeneration < current.identity.seatGameGeneration) {
        return attachmentResult(RecoveryAttachmentCode::StaleSeatGameGeneration, current,
                                "recovery attachment Seat-game generation is stale");
    }
    if (identity.seatGameGeneration != current.identity.seatGameGeneration) {
        return attachmentResult(RecoveryAttachmentCode::SeatGameGenerationMismatch, current,
                                "recovery attachment Seat-game generation does not match");
    }
    if (identity.process != current.identity.process) {
        return attachmentResult(RecoveryAttachmentCode::ProcessIdentityMismatch, current,
                                "recovery attachment PID/creation identity does not match");
    }
    if (identity.recoveryEpoch != current.identity.recoveryEpoch ||
        lease != current.manifest.lease) {
        return attachmentResult(RecoveryAttachmentCode::LeaseMismatch, current,
                                "recovery attachment lease/epoch does not match");
    }
    if (identity != current.identity) {
        return attachmentResult(RecoveryAttachmentCode::ConflictingRegistration, current,
                                "recovery attachment identity conflicts with the armed registration");
    }
    return attachmentResult(RecoveryAttachmentCode::Ok, current,
                            "exact recovery attachment is armed");
}

RecoveryAttachmentResult RecoveryProcessAttachmentAuthority::registerAttachment(
    RecoveryProcessAttachmentRegistration registration) {
    std::string error;
    if (!validateRecoveryProcessAttachmentIdentity(registration.identity, &error)) {
        return attachmentResult(RecoveryAttachmentCode::InvalidIdentity, std::nullopt,
                                std::move(error));
    }
    if (!validateRecoveryProcessAttachmentRegistration(registration, &error)) {
        return attachmentResult(RecoveryAttachmentCode::InvalidPlan, std::nullopt,
                                std::move(error));
    }

    const auto activeSeat = std::find_if(
        active_.begin(), active_.end(), [&](const auto& current) {
            return current.identity.seatId == registration.identity.seatId;
        });
    if (activeSeat != active_.end()) {
        if (*activeSeat == registration) {
            return attachmentResult(RecoveryAttachmentCode::AlreadySatisfied,
                                    *activeSeat,
                                    "exact recovery attachment is already registered");
        }
        auto mismatch = compareAgainstCurrent(
            *activeSeat, registration.identity, registration.manifest.lease);
        if (mismatch.code == RecoveryAttachmentCode::Ok) {
            mismatch.code = RecoveryAttachmentCode::ConflictingRegistration;
            mismatch.diagnostic =
                "same exact attachment identity cannot replace a different armed manifest";
        }
        return mismatch;
    }

    for (const auto& current : active_) {
        if (current.identity.process == registration.identity.process) {
            return attachmentResult(RecoveryAttachmentCode::SeatMismatch, current,
                                    "exact process identity is already attached to another Seat");
        }
        if (current.identity.process.processId == registration.identity.process.processId) {
            return attachmentResult(RecoveryAttachmentCode::ProcessIdentityMismatch, current,
                                    "an active attachment already owns this PID with another creation time");
        }
        if (sameWatchdogLeaseIdentity(current.manifest.lease,
                                      registration.manifest.lease)) {
            return attachmentResult(RecoveryAttachmentCode::LeaseMismatch, current,
                                    "watchdog lease identity is already bound to another Seat attachment");
        }
    }

    for (const auto& completed : lastDisarmed_) {
        if (sameWatchdogLeaseIdentity(completed.manifest.lease,
                                      registration.manifest.lease)) {
            return attachmentResult(RecoveryAttachmentCode::ReplayRejected, completed,
                                    "a completed watchdog lease identity cannot authorize a new attachment");
        }
    }

    const auto prior = std::find_if(
        lastDisarmed_.begin(), lastDisarmed_.end(), [&](const auto& current) {
            return current.identity.seatId == registration.identity.seatId;
        });
    if (prior != lastDisarmed_.end()) {
        if (*prior == registration) {
            return attachmentResult(RecoveryAttachmentCode::ReplayRejected, *prior,
                                    "a completed exact recovery attachment cannot be replayed");
        }
        if (prior->identity.hostSessionId == registration.identity.hostSessionId) {
            if (registration.identity.sessionGeneration <
                prior->identity.sessionGeneration) {
                return attachmentResult(RecoveryAttachmentCode::StaleSessionGeneration,
                                        *prior,
                                        "recovery attachment session generation predates the last completed epoch");
            }
            if (registration.identity.sessionGeneration ==
                prior->identity.sessionGeneration) {
                if (registration.identity.seatGameGeneration <
                    prior->identity.seatGameGeneration) {
                    return attachmentResult(RecoveryAttachmentCode::StaleSeatGameGeneration,
                                            *prior,
                                            "recovery attachment Seat-game generation predates the last completed epoch");
                }
                if (registration.identity.recoveryEpoch <= prior->identity.recoveryEpoch) {
                    return attachmentResult(RecoveryAttachmentCode::ReplayRejected, *prior,
                                            "recovery attachment epoch did not advance past the last completed epoch");
                }
            }
        }
    }

    if (active_.size() >= kMaximumRecoveryProcessAttachments) {
        return attachmentResult(RecoveryAttachmentCode::SeatLimitExceeded,
                                std::nullopt,
                                "v1 recovery attachment authority already owns two Seats");
    }
    const auto insertedIdentity = registration.identity;
    active_.push_back(std::move(registration));
    std::sort(active_.begin(), active_.end(), [](const auto& left, const auto& right) {
        return left.identity.seatId < right.identity.seatId;
    });
    const auto inserted = std::find_if(
        active_.begin(), active_.end(), [&](const auto& value) {
            return value.identity == insertedIdentity;
        });
    return attachmentResult(RecoveryAttachmentCode::Ok,
                            inserted == active_.end()
                                ? std::optional<RecoveryProcessAttachmentRegistration>{}
                                : std::optional<RecoveryProcessAttachmentRegistration>{*inserted},
                            "exact recovery attachment registered");
}

RecoveryAttachmentResult RecoveryProcessAttachmentAuthority::verifyArmed(
    const RecoveryProcessAttachmentIdentity& identity,
    const watchdog::WatchdogLease& lease) const {
    std::string error;
    if (!validateRecoveryProcessAttachmentIdentity(identity, &error)) {
        return attachmentResult(RecoveryAttachmentCode::InvalidIdentity, std::nullopt,
                                std::move(error));
    }
    const auto current = std::find_if(
        active_.begin(), active_.end(), [&](const auto& value) {
            return value.identity.seatId == identity.seatId;
        });
    if (current != active_.end()) {
        return compareAgainstCurrent(*current, identity, lease);
    }
    const auto otherSeat = std::find_if(
        active_.begin(), active_.end(), [&](const auto& value) {
            return value.identity.process == identity.process;
        });
    if (otherSeat != active_.end()) {
        return attachmentResult(RecoveryAttachmentCode::SeatMismatch, *otherSeat,
                                "exact process attachment is armed for another Seat");
    }
    return attachmentResult(RecoveryAttachmentCode::NotArmed, std::nullopt,
                            "recovery attachment is not armed");
}

RecoveryAttachmentResult RecoveryProcessAttachmentAuthority::disarm(
    const RecoveryProcessAttachmentIdentity& identity,
    const watchdog::WatchdogLease& lease) {
    const auto current = std::find_if(
        active_.begin(), active_.end(), [&](const auto& value) {
            return value.identity.seatId == identity.seatId;
        });
    if (current == active_.end()) {
        const auto prior = std::find_if(
            lastDisarmed_.begin(), lastDisarmed_.end(), [&](const auto& value) {
                return value.identity == identity && value.manifest.lease == lease;
            });
        if (prior != lastDisarmed_.end()) {
            return attachmentResult(RecoveryAttachmentCode::AlreadySatisfied, *prior,
                                    "exact recovery attachment was already disarmed");
        }
        return verifyArmed(identity, lease);
    }

    auto matched = compareAgainstCurrent(*current, identity, lease);
    if (!matched.succeeded()) return matched;
    const auto completed = *current;
    const auto prior = std::find_if(
        lastDisarmed_.begin(), lastDisarmed_.end(), [&](const auto& value) {
            return value.identity.seatId == completed.identity.seatId;
        });
    if (prior == lastDisarmed_.end()) {
        lastDisarmed_.push_back(completed);
    } else {
        *prior = completed;
    }
    active_.erase(current);
    return attachmentResult(RecoveryAttachmentCode::Ok, completed,
                            "exact recovery attachment disarmed");
}

std::vector<RecoveryProcessAttachmentRegistration>
RecoveryProcessAttachmentAuthority::activeAttachments() const {
    auto result = active_;
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.identity.seatId < right.identity.seatId;
    });
    return result;
}

std::string_view recoveryAttachmentCodeName(RecoveryAttachmentCode code) noexcept {
    switch (code) {
    case RecoveryAttachmentCode::Ok: return "ok";
    case RecoveryAttachmentCode::AlreadySatisfied: return "already-satisfied";
    case RecoveryAttachmentCode::InvalidIdentity: return "invalid-identity";
    case RecoveryAttachmentCode::InvalidPlan: return "invalid-plan";
    case RecoveryAttachmentCode::SeatLimitExceeded: return "seat-limit-exceeded";
    case RecoveryAttachmentCode::SeatMismatch: return "seat-mismatch";
    case RecoveryAttachmentCode::SessionMismatch: return "session-mismatch";
    case RecoveryAttachmentCode::StaleSessionGeneration: return "stale-session-generation";
    case RecoveryAttachmentCode::SessionGenerationMismatch: return "session-generation-mismatch";
    case RecoveryAttachmentCode::StaleSeatGameGeneration: return "stale-seat-game-generation";
    case RecoveryAttachmentCode::SeatGameGenerationMismatch: return "seat-game-generation-mismatch";
    case RecoveryAttachmentCode::ProcessIdentityMismatch: return "process-identity-mismatch";
    case RecoveryAttachmentCode::LeaseMismatch: return "lease-mismatch";
    case RecoveryAttachmentCode::ConflictingRegistration: return "conflicting-registration";
    case RecoveryAttachmentCode::ReplayRejected: return "replay-rejected";
    case RecoveryAttachmentCode::NotArmed: return "not-armed";
    }
    return "unknown";
}

} // namespace hydra::recovery
