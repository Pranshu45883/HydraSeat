#include "hydra/watchdog_protocol.hpp"

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
