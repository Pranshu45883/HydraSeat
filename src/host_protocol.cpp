#include "hydra/host_protocol.hpp"

#include "hydra/controller_identity.hpp"

#include <limits>
#include <type_traits>
#include <utility>

namespace hydra::hostipc {
namespace {

template <typename T>
using Unsigned = std::make_unsigned_t<T>;

template <typename T>
void appendInteger(std::vector<std::byte>& out, T value) {
    using U = Unsigned<T>;
    U raw = static_cast<U>(value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        const auto octet =
            static_cast<unsigned int>((raw >> (index * 8u)) & U{255});
        out.push_back(static_cast<std::byte>(octet));
    }
}

template <typename T>
bool readInteger(std::span<const std::byte> bytes,
                 std::size_t& offset,
                 T& value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
        return false;
    }
    using U = Unsigned<T>;
    U raw = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        raw |= static_cast<U>(
                   std::to_integer<unsigned int>(bytes[offset + index]))
               << (index * 8u);
    }
    offset += sizeof(T);
    value = static_cast<T>(raw);
    return true;
}

bool validSeatId(std::uint32_t seatId) noexcept {
    return seatId == 1u || seatId == 2u;
}

bool validMessageType(MessageType type) noexcept {
    switch (type) {
    case MessageType::Hello:
    case MessageType::HelloAck:
    case MessageType::GetSnapshot:
    case MessageType::Snapshot:
    case MessageType::Ping:
    case MessageType::Pong:
    case MessageType::Error:
    case MessageType::AcquireUiLease:
    case MessageType::AcquireUiLeaseResult:
    case MessageType::ReleaseUiLease:
    case MessageType::ReleaseUiLeaseResult:
    case MessageType::PairController:
    case MessageType::PairControllerResult:
    case MessageType::RouteAudio:
    case MessageType::RouteAudioResult:
    case MessageType::ResetAudio:
    case MessageType::ResetAudioResult:
        return true;
    }
    return false;
}

bool validRole(ClientRole role) noexcept {
    return role == ClientRole::ReadOnly || role == ClientRole::Control;
}

bool validError(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None:
    case ErrorCode::Malformed:
    case ErrorCode::VersionMismatch:
    case ErrorCode::PermissionDenied:
    case ErrorCode::Unsupported:
    case ErrorCode::InternalError:
    case ErrorCode::InvalidState:
        return true;
    }
    return false;
}

void setDecodeError(DecodeResult* result,
                    ErrorCode code,
                    std::string diagnostic) {
    if (!result) return;
    result->error = code;
    result->diagnostic = std::move(diagnostic);
}

constexpr std::uint32_t kSeatFlagActive = 1u << 0u;
constexpr std::uint32_t kSeatFlagUiLease = 1u << 1u;
constexpr std::uint32_t kSeatFlagGameLease = 1u << 2u;
constexpr std::uint32_t kSeatFlagProcess = 1u << 3u;
constexpr std::uint32_t kSeatFlagWindow = 1u << 4u;
constexpr std::uint32_t kSeatFlagController = 1u << 5u;
constexpr std::uint32_t kSeatFlagMask =
    kSeatFlagActive | kSeatFlagUiLease | kSeatFlagGameLease |
    kSeatFlagProcess | kSeatFlagWindow | kSeatFlagController;

std::uint32_t seatFlags(const SeatSnapshot& seat) noexcept {
    std::uint32_t flags = 0;
    if (seat.active) flags |= kSeatFlagActive;
    if (seat.uiLeaseActive) flags |= kSeatFlagUiLease;
    if (seat.gameLeaseActive) flags |= kSeatFlagGameLease;
    if (seat.processOwned) flags |= kSeatFlagProcess;
    if (seat.windowOwned) flags |= kSeatFlagWindow;
    if (seat.controllerBound) flags |= kSeatFlagController;
    return flags;
}

bool validSeatSnapshot(const SeatSnapshot& seat,
                       std::uint32_t expectedSeatId) noexcept {
    if (seat.seatId != expectedSeatId) return false;
    const bool leaseActive = seat.uiLeaseActive || seat.gameLeaseActive;
    if (seat.active != leaseActive) return false;
    if (!seat.active &&
        (seat.processOwned || seat.windowOwned || seat.controllerBound)) {
        return false;
    }
    if (seat.active && seat.generation == 0) return false;
    if (!seat.gameLeaseActive && (seat.processOwned || seat.windowOwned)) {
        return false;
    }
    if (seat.windowOwned && !seat.processOwned) return false;
    return true;
}

bool validControllerId(std::string_view value) noexcept {
    if (value.empty() || value.size() > kHostProtocolMaxControllerIdBytes) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (ch < 0x21u || ch > 0x7eu) return false;
    }
    return true;
}

bool validAudioEndpointId(std::string_view value) noexcept {
    if (value.empty() ||
        value.size() > kHostProtocolMaxAudioEndpointIdBytes) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (ch < 0x21u || ch > 0x7eu) return false;
    }
    return true;
}

bool validAudioMutationStatus(AudioMutationStatus status) noexcept {
    switch (status) {
    case AudioMutationStatus::Success:
    case AudioMutationStatus::InvalidProcess:
    case AudioMutationStatus::ProcessNotFound:
    case AudioMutationStatus::AudioSessionNotFound:
    case AudioMutationStatus::EndpointNotFound:
    case AudioMutationStatus::EndpointUnavailable:
    case AudioMutationStatus::IdentityMismatch:
    case AudioMutationStatus::RoutingFailed:
    case AudioMutationStatus::OsApiError:
        return true;
    }
    return false;
}

} // namespace

std::string_view messageTypeName(MessageType type) noexcept {
    switch (type) {
    case MessageType::Hello: return "Hello";
    case MessageType::HelloAck: return "HelloAck";
    case MessageType::GetSnapshot: return "GetSnapshot";
    case MessageType::Snapshot: return "Snapshot";
    case MessageType::Ping: return "Ping";
    case MessageType::Pong: return "Pong";
    case MessageType::Error: return "Error";
    case MessageType::AcquireUiLease: return "AcquireUiLease";
    case MessageType::AcquireUiLeaseResult: return "AcquireUiLeaseResult";
    case MessageType::ReleaseUiLease: return "ReleaseUiLease";
    case MessageType::ReleaseUiLeaseResult: return "ReleaseUiLeaseResult";
    case MessageType::PairController: return "PairController";
    case MessageType::PairControllerResult: return "PairControllerResult";
    case MessageType::RouteAudio: return "RouteAudio";
    case MessageType::RouteAudioResult: return "RouteAudioResult";
    case MessageType::ResetAudio: return "ResetAudio";
    case MessageType::ResetAudioResult: return "ResetAudioResult";
    }
    return "Unknown";
}

std::string_view errorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None: return "None";
    case ErrorCode::Malformed: return "Malformed";
    case ErrorCode::VersionMismatch: return "VersionMismatch";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::InternalError: return "InternalError";
    case ErrorCode::InvalidState: return "InvalidState";
    }
    return "Unknown";
}

std::vector<std::byte> encodeFrame(const Frame& frame) {
    if (!validMessageType(frame.type) || frame.correlationId == 0 ||
        frame.payload.size() > kHostProtocolMaxPayloadBytes ||
        frame.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }

    std::vector<std::byte> out;
    out.reserve(kHostProtocolHeaderBytes + frame.payload.size());
    appendInteger(out, kHostProtocolMagic);
    appendInteger(out, kHostProtocolVersion);
    appendInteger(out, static_cast<std::uint16_t>(frame.type));
    appendInteger(out, frame.correlationId);
    appendInteger(out, static_cast<std::uint32_t>(frame.payload.size()));
    appendInteger(out, std::uint32_t{0});
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return out;
}

std::optional<Frame> decodeFrame(
    std::span<const std::byte> bytes,
    DecodeResult* result) {
    if (result) *result = {};
    if (bytes.size() < kHostProtocolHeaderBytes) {
        setDecodeError(result, ErrorCode::Malformed, "frame shorter than header");
        return std::nullopt;
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t rawType = 0;
    std::uint64_t correlationId = 0;
    std::uint32_t payloadSize = 0;
    std::uint32_t reserved = 0;
    if (!readInteger(bytes, offset, magic) ||
        !readInteger(bytes, offset, version) ||
        !readInteger(bytes, offset, rawType) ||
        !readInteger(bytes, offset, correlationId) ||
        !readInteger(bytes, offset, payloadSize) ||
        !readInteger(bytes, offset, reserved)) {
        setDecodeError(result, ErrorCode::Malformed,
                       "frame header decode failed");
        return std::nullopt;
    }
    if (magic != kHostProtocolMagic) {
        setDecodeError(result, ErrorCode::Malformed, "frame magic mismatch");
        return std::nullopt;
    }
    if (version != kHostProtocolVersion) {
        setDecodeError(result, ErrorCode::VersionMismatch,
                       "host protocol version mismatch");
        return std::nullopt;
    }
    const auto type = static_cast<MessageType>(rawType);
    if (!validMessageType(type) || correlationId == 0 || reserved != 0) {
        setDecodeError(result, ErrorCode::Malformed, "invalid frame metadata");
        return std::nullopt;
    }
    if (payloadSize > kHostProtocolMaxPayloadBytes ||
        bytes.size() != kHostProtocolHeaderBytes + payloadSize) {
        setDecodeError(result, ErrorCode::Malformed,
                       "invalid frame payload length");
        return std::nullopt;
    }

    Frame frame;
    frame.type = type;
    frame.correlationId = correlationId;
    frame.payload.assign(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    return frame;
}

std::vector<std::byte> encodeHello(const Hello& value) {
    if (!validRole(value.role)) return {};
    std::vector<std::byte> out;
    out.reserve(8);
    appendInteger(out, static_cast<std::uint8_t>(value.role));
    for (int index = 0; index < 7; ++index) appendInteger(out, std::uint8_t{0});
    return out;
}

std::optional<Hello> decodeHello(std::span<const std::byte> payload) {
    if (payload.size() != 8) return std::nullopt;
    std::size_t offset = 0;
    std::uint8_t rawRole = 0;
    if (!readInteger(payload, offset, rawRole)) return std::nullopt;
    for (; offset < payload.size(); ++offset) {
        if (payload[offset] != std::byte{0}) return std::nullopt;
    }
    const auto role = static_cast<ClientRole>(rawRole);
    if (!validRole(role)) return std::nullopt;
    return Hello{role};
}

std::vector<std::byte> encodeHelloAck(const HelloAck& value) {
    if (!validRole(value.role) ||
        value.protocolVersion != kHostProtocolVersion ||
        value.seatCount != kHostSeatCount) {
        return {};
    }
    std::vector<std::byte> out;
    out.reserve(8);
    appendInteger(out, static_cast<std::uint8_t>(value.role));
    appendInteger(out, value.seatCount);
    appendInteger(out, value.protocolVersion);
    appendInteger(out, std::uint32_t{0});
    return out;
}

std::optional<HelloAck> decodeHelloAck(std::span<const std::byte> payload) {
    if (payload.size() != 8) return std::nullopt;
    std::size_t offset = 0;
    std::uint8_t rawRole = 0;
    std::uint8_t seatCount = 0;
    std::uint16_t version = 0;
    std::uint32_t reserved = 0;
    if (!readInteger(payload, offset, rawRole) ||
        !readInteger(payload, offset, seatCount) ||
        !readInteger(payload, offset, version) ||
        !readInteger(payload, offset, reserved)) {
        return std::nullopt;
    }
    const auto role = static_cast<ClientRole>(rawRole);
    if (!validRole(role) || seatCount != kHostSeatCount ||
        version != kHostProtocolVersion || reserved != 0) {
        return std::nullopt;
    }
    return HelloAck{role, version, seatCount};
}

std::vector<std::byte> encodeSnapshot(const HostSnapshot& snapshot) {
    if (snapshot.authorityRevision == 0 ||
        !validSeatSnapshot(snapshot.seats[0], 1) ||
        !validSeatSnapshot(snapshot.seats[1], 2)) {
        return {};
    }

    std::vector<std::byte> out;
    out.reserve(56);
    appendInteger(out, snapshot.authorityRevision);
    for (const auto& seat : snapshot.seats) {
        appendInteger(out, seat.seatId);
        appendInteger(out, std::uint32_t{0});
        appendInteger(out, seat.generation);
        appendInteger(out, seatFlags(seat));
        appendInteger(out, std::uint32_t{0});
    }
    return out;
}

std::optional<HostSnapshot> decodeSnapshot(
    std::span<const std::byte> payload) {
    if (payload.size() != 56) return std::nullopt;
    std::size_t offset = 0;
    HostSnapshot snapshot;
    if (!readInteger(payload, offset, snapshot.authorityRevision) ||
        snapshot.authorityRevision == 0) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < snapshot.seats.size(); ++index) {
        auto& seat = snapshot.seats[index];
        std::uint32_t reservedBefore = 0;
        std::uint32_t flags = 0;
        std::uint32_t reservedAfter = 0;
        if (!readInteger(payload, offset, seat.seatId) ||
            !readInteger(payload, offset, reservedBefore) ||
            !readInteger(payload, offset, seat.generation) ||
            !readInteger(payload, offset, flags) ||
            !readInteger(payload, offset, reservedAfter)) {
            return std::nullopt;
        }
        if (reservedBefore != 0 || reservedAfter != 0 ||
            (flags & ~kSeatFlagMask) != 0) {
            return std::nullopt;
        }

        seat.active = (flags & kSeatFlagActive) != 0;
        seat.uiLeaseActive = (flags & kSeatFlagUiLease) != 0;
        seat.gameLeaseActive = (flags & kSeatFlagGameLease) != 0;
        seat.processOwned = (flags & kSeatFlagProcess) != 0;
        seat.windowOwned = (flags & kSeatFlagWindow) != 0;
        seat.controllerBound = (flags & kSeatFlagController) != 0;
        if (!validSeatSnapshot(
                seat, static_cast<std::uint32_t>(index + 1))) {
            return std::nullopt;
        }
    }
    return snapshot;
}

std::vector<std::byte> encodeSeatRequest(const SeatRequest& request) {
    if (!validSeatId(request.seatId)) return {};
    std::vector<std::byte> out;
    out.reserve(8);
    appendInteger(out, request.seatId);
    appendInteger(out, std::uint32_t{0});
    return out;
}

std::optional<SeatRequest> decodeSeatRequest(
    std::span<const std::byte> payload) {
    if (payload.size() != 8) return std::nullopt;
    std::size_t offset = 0;
    SeatRequest request;
    std::uint32_t reserved = 0;
    if (!readInteger(payload, offset, request.seatId) ||
        !readInteger(payload, offset, reserved) ||
        reserved != 0 || !validSeatId(request.seatId)) {
        return std::nullopt;
    }
    return request;
}

std::vector<std::byte> encodeControllerPairRequest(
    const ControllerPairRequest& request) {
    if (!validSeatId(request.seatId) ||
        request.runtimeXInputSlot >= controller::kXInputSlotCount ||
        !validControllerId(request.persistentControllerId)) {
        return {};
    }

    std::vector<std::byte> out;
    out.reserve(12 + request.persistentControllerId.size());
    appendInteger(out, request.seatId);
    appendInteger(out, request.runtimeXInputSlot);
    appendInteger(out, std::uint8_t{0});
    appendInteger(out, std::uint8_t{0});
    appendInteger(out, std::uint8_t{0});
    appendInteger(
        out, static_cast<std::uint32_t>(request.persistentControllerId.size()));
    for (const char ch : request.persistentControllerId) {
        out.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(ch)));
    }
    return out;
}

std::optional<ControllerPairRequest> decodeControllerPairRequest(
    std::span<const std::byte> payload) {
    if (payload.size() < 12 ||
        payload.size() > 12 + kHostProtocolMaxControllerIdBytes) {
        return std::nullopt;
    }

    std::size_t offset = 0;
    ControllerPairRequest request;
    std::uint8_t reserved1 = 0;
    std::uint8_t reserved2 = 0;
    std::uint8_t reserved3 = 0;
    std::uint32_t length = 0;
    if (!readInteger(payload, offset, request.seatId) ||
        !readInteger(payload, offset, request.runtimeXInputSlot) ||
        !readInteger(payload, offset, reserved1) ||
        !readInteger(payload, offset, reserved2) ||
        !readInteger(payload, offset, reserved3) ||
        !readInteger(payload, offset, length)) {
        return std::nullopt;
    }
    if (!validSeatId(request.seatId) ||
        request.runtimeXInputSlot >= controller::kXInputSlotCount ||
        reserved1 != 0 || reserved2 != 0 || reserved3 != 0 ||
        length == 0 || length > kHostProtocolMaxControllerIdBytes ||
        payload.size() != offset + length) {
        return std::nullopt;
    }

    request.persistentControllerId.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        request.persistentControllerId.push_back(static_cast<char>(
            std::to_integer<unsigned char>(payload[offset + index])));
    }
    if (!validControllerId(request.persistentControllerId)) return std::nullopt;
    return request;
}

std::vector<std::byte> encodeProcessRequest(const ProcessRequest& request) {
    if (!validSeatId(request.seatId) || request.processId == 0 ||
        request.creationIdentity == 0) {
        return {};
    }
    std::vector<std::byte> out;
    out.reserve(16);
    appendInteger(out, request.seatId);
    appendInteger(out, request.processId);
    appendInteger(out, request.creationIdentity);
    return out;
}

std::optional<ProcessRequest> decodeProcessRequest(
    std::span<const std::byte> payload) {
    if (payload.size() != 16) return std::nullopt;
    std::size_t offset = 0;
    ProcessRequest request;
    if (!readInteger(payload, offset, request.seatId) ||
        !readInteger(payload, offset, request.processId) ||
        !readInteger(payload, offset, request.creationIdentity) ||
        !validSeatId(request.seatId) || request.processId == 0 ||
        request.creationIdentity == 0) {
        return std::nullopt;
    }
    return request;
}

std::vector<std::byte> encodeAudioRouteRequest(
    const AudioRouteRequest& request) {
    if (!validSeatId(request.process.seatId) ||
        request.process.processId == 0 ||
        request.process.creationIdentity == 0 ||
        !validAudioEndpointId(request.endpointId)) {
        return {};
    }

    std::vector<std::byte> out;
    out.reserve(20 + request.endpointId.size());
    appendInteger(out, request.process.seatId);
    appendInteger(out, request.process.processId);
    appendInteger(out, request.process.creationIdentity);
    appendInteger(out, static_cast<std::uint32_t>(request.endpointId.size()));
    for (const char ch : request.endpointId) {
        out.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(ch)));
    }
    return out;
}

std::optional<AudioRouteRequest> decodeAudioRouteRequest(
    std::span<const std::byte> payload) {
    if (payload.size() < 21 ||
        payload.size() > 20 + kHostProtocolMaxAudioEndpointIdBytes) {
        return std::nullopt;
    }

    std::size_t offset = 0;
    AudioRouteRequest request;
    std::uint32_t length = 0;
    if (!readInteger(payload, offset, request.process.seatId) ||
        !readInteger(payload, offset, request.process.processId) ||
        !readInteger(payload, offset, request.process.creationIdentity) ||
        !readInteger(payload, offset, length) ||
        !validSeatId(request.process.seatId) ||
        request.process.processId == 0 ||
        request.process.creationIdentity == 0 ||
        length == 0 || length > kHostProtocolMaxAudioEndpointIdBytes ||
        payload.size() != offset + length) {
        return std::nullopt;
    }

    request.endpointId.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        request.endpointId.push_back(static_cast<char>(
            std::to_integer<unsigned char>(payload[offset + index])));
    }
    if (!validAudioEndpointId(request.endpointId)) return std::nullopt;
    return request;
}

std::vector<std::byte> encodeAudioMutationResult(
    const AudioMutationResult& result) {
    if (!validAudioMutationStatus(result.status)) return {};
    std::vector<std::byte> out;
    out.reserve(8);
    appendInteger(out, static_cast<std::uint16_t>(result.status));
    appendInteger(out, std::uint16_t{0});
    appendInteger(out, std::uint32_t{0});
    return out;
}

std::optional<AudioMutationResult> decodeAudioMutationResult(
    std::span<const std::byte> payload) {
    if (payload.size() != 8) return std::nullopt;
    std::size_t offset = 0;
    std::uint16_t rawStatus = 0;
    std::uint16_t reserved16 = 0;
    std::uint32_t reserved32 = 0;
    if (!readInteger(payload, offset, rawStatus) ||
        !readInteger(payload, offset, reserved16) ||
        !readInteger(payload, offset, reserved32) ||
        reserved16 != 0 || reserved32 != 0) {
        return std::nullopt;
    }
    const auto status = static_cast<AudioMutationStatus>(rawStatus);
    if (!validAudioMutationStatus(status)) return std::nullopt;
    return AudioMutationResult{status};
}

std::vector<std::byte> encodePing(std::uint64_t nonce) {
    if (nonce == 0) return {};
    std::vector<std::byte> out;
    out.reserve(8);
    appendInteger(out, nonce);
    return out;
}

std::optional<std::uint64_t> decodePing(
    std::span<const std::byte> payload) {
    if (payload.size() != 8) return std::nullopt;
    std::size_t offset = 0;
    std::uint64_t nonce = 0;
    if (!readInteger(payload, offset, nonce) || nonce == 0) return std::nullopt;
    return nonce;
}

std::vector<std::byte> encodeError(const ErrorPayload& error) {
    if (!validError(error.code) || error.code == ErrorCode::None ||
        error.diagnostic.size() > kHostProtocolMaxDiagnosticBytes ||
        error.diagnostic.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }

    std::vector<std::byte> out;
    out.reserve(8 + error.diagnostic.size());
    appendInteger(out, static_cast<std::uint16_t>(error.code));
    appendInteger(out, std::uint16_t{0});
    appendInteger(out, static_cast<std::uint32_t>(error.diagnostic.size()));
    for (const char ch : error.diagnostic) {
        out.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(ch)));
    }
    return out;
}

std::optional<ErrorPayload> decodeError(
    std::span<const std::byte> payload) {
    if (payload.size() < 8 ||
        payload.size() > 8 + kHostProtocolMaxDiagnosticBytes) {
        return std::nullopt;
    }

    std::size_t offset = 0;
    std::uint16_t rawCode = 0;
    std::uint16_t reserved = 0;
    std::uint32_t length = 0;
    if (!readInteger(payload, offset, rawCode) ||
        !readInteger(payload, offset, reserved) ||
        !readInteger(payload, offset, length) ||
        reserved != 0 ||
        length > kHostProtocolMaxDiagnosticBytes ||
        payload.size() != offset + length) {
        return std::nullopt;
    }

    const auto code = static_cast<ErrorCode>(rawCode);
    if (!validError(code) || code == ErrorCode::None) return std::nullopt;

    ErrorPayload error;
    error.code = code;
    error.diagnostic.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        error.diagnostic.push_back(static_cast<char>(
            std::to_integer<unsigned char>(payload[offset + index])));
    }
    return error;
}

bool isMutatingRequest(MessageType type) noexcept {
    return type == MessageType::AcquireUiLease ||
           type == MessageType::ReleaseUiLease ||
           type == MessageType::PairController ||
           type == MessageType::RouteAudio ||
           type == MessageType::ResetAudio;
}

MessageType responseTypeFor(MessageType request) noexcept {
    switch (request) {
    case MessageType::Hello: return MessageType::HelloAck;
    case MessageType::GetSnapshot: return MessageType::Snapshot;
    case MessageType::Ping: return MessageType::Pong;
    case MessageType::AcquireUiLease: return MessageType::AcquireUiLeaseResult;
    case MessageType::ReleaseUiLease: return MessageType::ReleaseUiLeaseResult;
    case MessageType::PairController: return MessageType::PairControllerResult;
    case MessageType::RouteAudio: return MessageType::RouteAudioResult;
    case MessageType::ResetAudio: return MessageType::ResetAudioResult;
    default: return MessageType::Error;
    }
}

} // namespace hydra::hostipc
