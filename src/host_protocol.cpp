#include "hydra/host_protocol.hpp"

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
        const auto octet = static_cast<unsigned int>((raw >> (index * 8u)) & U{255});
        out.push_back(static_cast<std::byte>(octet));
    }
}

template <typename T>
bool readInteger(std::span<const std::byte> bytes, std::size_t& offset, T& value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;
    using U = Unsigned<T>;
    U raw = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        raw |= static_cast<U>(
            std::to_integer<unsigned int>(bytes[offset + index])) << (index * 8u);
    }
    offset += sizeof(T);
    value = static_cast<T>(raw);
    return true;
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
        return true;
    }
    return false;
}

void setDecodeError(DecodeResult* result, ErrorCode code, std::string diagnostic) {
    if (!result) return;
    result->error = code;
    result->diagnostic = std::move(diagnostic);
}

constexpr std::uint32_t kSeatFlagActive = 1u << 0u;
constexpr std::uint32_t kSeatFlagProcess = 1u << 1u;
constexpr std::uint32_t kSeatFlagWindow = 1u << 2u;
constexpr std::uint32_t kSeatFlagController = 1u << 3u;
constexpr std::uint32_t kSeatFlagMask =
    kSeatFlagActive | kSeatFlagProcess | kSeatFlagWindow | kSeatFlagController;

std::uint32_t seatFlags(const SeatSnapshot& seat) noexcept {
    std::uint32_t flags = 0;
    if (seat.active) flags |= kSeatFlagActive;
    if (seat.processOwned) flags |= kSeatFlagProcess;
    if (seat.windowOwned) flags |= kSeatFlagWindow;
    if (seat.controllerBound) flags |= kSeatFlagController;
    return flags;
}

bool validSeatSnapshot(const SeatSnapshot& seat, std::uint32_t expectedSeatId) noexcept {
    if (seat.seatId != expectedSeatId) return false;
    if (!seat.active && (seat.processOwned || seat.windowOwned || seat.controllerBound)) {
        return false;
    }
    if (seat.windowOwned && !seat.processOwned) return false;
    if (seat.active && seat.generation == 0) return false;
    return true;
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
        setDecodeError(result, ErrorCode::Malformed, "frame header decode failed");
        return std::nullopt;
    }
    if (magic != kHostProtocolMagic) {
        setDecodeError(result, ErrorCode::Malformed, "frame magic mismatch");
        return std::nullopt;
    }
    if (version != kHostProtocolVersion) {
        setDecodeError(result, ErrorCode::VersionMismatch, "host protocol version mismatch");
        return std::nullopt;
    }
    const auto type = static_cast<MessageType>(rawType);
    if (!validMessageType(type) || correlationId == 0 || reserved != 0) {
        setDecodeError(result, ErrorCode::Malformed, "invalid frame metadata");
        return std::nullopt;
    }
    if (payloadSize > kHostProtocolMaxPayloadBytes ||
        bytes.size() != kHostProtocolHeaderBytes + payloadSize) {
        setDecodeError(result, ErrorCode::Malformed, "invalid frame payload length");
        return std::nullopt;
    }

    Frame frame;
    frame.type = type;
    frame.correlationId = correlationId;
    frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    return frame;
}

std::vector<std::byte> encodeHello(const Hello& value) {
    if (!validRole(value.role)) return {};
    std::vector<std::byte> out;
    out.reserve(8);
    appendInteger(out, static_cast<std::uint8_t>(value.role));
    for (int index = 0; index < 7; ++index) {
        appendInteger(out, std::uint8_t{0});
    }
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
    if (!validRole(role) ||
        seatCount != kHostSeatCount ||
        version != kHostProtocolVersion ||
        reserved != 0) {
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

std::optional<HostSnapshot> decodeSnapshot(std::span<const std::byte> payload) {
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
        seat.processOwned = (flags & kSeatFlagProcess) != 0;
        seat.windowOwned = (flags & kSeatFlagWindow) != 0;
        seat.controllerBound = (flags & kSeatFlagController) != 0;
        if (!validSeatSnapshot(seat, static_cast<std::uint32_t>(index + 1))) {
            return std::nullopt;
        }
    }
    return snapshot;
}

std::vector<std::byte> encodePing(std::uint64_t nonce) {
    if (nonce == 0) return {};
    std::vector<std::byte> out;
    out.reserve(8);
    appendInteger(out, nonce);
    return out;
}

std::optional<std::uint64_t> decodePing(std::span<const std::byte> payload) {
    if (payload.size() != 8) return std::nullopt;
    std::size_t offset = 0;
    std::uint64_t nonce = 0;
    if (!readInteger(payload, offset, nonce) || nonce == 0) return std::nullopt;
    return nonce;
}

std::vector<std::byte> encodeError(const ErrorPayload& error) {
    if (!validError(error.code) ||
        error.code == ErrorCode::None ||
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
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::optional<ErrorPayload> decodeError(std::span<const std::byte> payload) {
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
        !readInteger(payload, offset, length)) {
        return std::nullopt;
    }

    const auto code = static_cast<ErrorCode>(rawCode);
    if (!validError(code) ||
        code == ErrorCode::None ||
        reserved != 0 ||
        length > kHostProtocolMaxDiagnosticBytes ||
        payload.size() != 8u + length) {
        return std::nullopt;
    }

    std::string diagnostic;
    diagnostic.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        diagnostic.push_back(static_cast<char>(
            std::to_integer<unsigned char>(payload[offset + index])));
    }
    return ErrorPayload{code, std::move(diagnostic)};
}

MessageType responseTypeFor(MessageType request) noexcept {
    switch (request) {
    case MessageType::Hello: return MessageType::HelloAck;
    case MessageType::GetSnapshot: return MessageType::Snapshot;
    case MessageType::Ping: return MessageType::Pong;
    default: return MessageType::Error;
    }
}

} // namespace hydra::hostipc
