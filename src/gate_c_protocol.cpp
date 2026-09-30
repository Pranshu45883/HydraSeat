#include "hydra/gate_c_protocol.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace hydra::gatec {
namespace {

class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::byte> bytes) : m_bytes(bytes) {}

    bool readU8(std::uint8_t& value) {
        if (!require(1)) return false;
        value = std::to_integer<std::uint8_t>(m_bytes[m_offset++]);
        return true;
    }

    bool readBool(bool& value) {
        std::uint8_t raw = 0;
        if (!readU8(raw)) return false;
        if (raw > 1) {
            m_error = "boolean field is not 0 or 1";
            return false;
        }
        value = raw != 0;
        return true;
    }

    bool readU16(std::uint16_t& value) {
        if (!require(2)) return false;
        value = static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(m_bytes[m_offset])) |
                static_cast<std::uint16_t>(
                    std::to_integer<std::uint8_t>(m_bytes[m_offset + 1])) << 8;
        m_offset += 2;
        return true;
    }

    bool readI16(std::int16_t& value) {
        std::uint16_t raw = 0;
        if (!readU16(raw)) return false;
        value = std::bit_cast<std::int16_t>(raw);
        return true;
    }

    bool readU32(std::uint32_t& value) {
        if (!require(4)) return false;
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(
                         std::to_integer<std::uint8_t>(
                             m_bytes[m_offset + shift / 8]))
                     << shift;
        }
        m_offset += 4;
        return true;
    }

    bool readI32(std::int32_t& value) {
        std::uint32_t raw = 0;
        if (!readU32(raw)) return false;
        value = std::bit_cast<std::int32_t>(raw);
        return true;
    }

    bool readU64(std::uint64_t& value) {
        if (!require(8)) return false;
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(
                         std::to_integer<std::uint8_t>(
                             m_bytes[m_offset + shift / 8]))
                     << shift;
        }
        m_offset += 8;
        return true;
    }

    bool readI64(std::int64_t& value) {
        std::uint64_t raw = 0;
        if (!readU64(raw)) return false;
        value = std::bit_cast<std::int64_t>(raw);
        return true;
    }

    template <std::size_t Size>
    bool readArray(std::array<std::uint8_t, Size>& value) {
        if (!require(Size)) return false;
        for (std::size_t index = 0; index < Size; ++index) {
            value[index] =
                std::to_integer<std::uint8_t>(m_bytes[m_offset + index]);
        }
        m_offset += Size;
        return true;
    }

    bool readPadding(std::size_t bytes) {
        if (!require(bytes)) return false;
        for (std::size_t index = 0; index < bytes; ++index) {
            if (m_bytes[m_offset + index] != std::byte{0}) {
                m_error = "reserved field is not zero";
                return false;
            }
        }
        m_offset += bytes;
        return true;
    }

    bool finished() const noexcept { return m_offset == m_bytes.size(); }
    const std::string& error() const noexcept { return m_error; }

private:
    bool require(std::size_t bytes) {
        if (bytes > m_bytes.size() - m_offset) {
            m_error = "payload is truncated";
            return false;
        }
        return true;
    }

    std::span<const std::byte> m_bytes;
    std::size_t m_offset{0};
    std::string m_error;
};

void appendU8(std::vector<std::byte>& output, std::uint8_t value) {
    output.push_back(static_cast<std::byte>(value));
}

void appendBool(std::vector<std::byte>& output, bool value) {
    appendU8(output, value ? 1u : 0u);
}

void appendU16(std::vector<std::byte>& output, std::uint16_t value) {
    for (unsigned shift = 0; shift < 16; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

void appendI16(std::vector<std::byte>& output, std::int16_t value) {
    appendU16(output, static_cast<std::uint16_t>(value));
}

void appendU32(std::vector<std::byte>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

void appendI32(std::vector<std::byte>& output, std::int32_t value) {
    appendU32(output, static_cast<std::uint32_t>(value));
}

void appendU64(std::vector<std::byte>& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

void appendI64(std::vector<std::byte>& output, std::int64_t value) {
    appendU64(output, static_cast<std::uint64_t>(value));
}

template <std::size_t Size>
void appendArray(std::vector<std::byte>& output,
                 const std::array<std::uint8_t, Size>& value) {
    for (const auto byte : value) {
        appendU8(output, byte);
    }
}

void appendPadding(std::vector<std::byte>& output, std::size_t bytes) {
    output.insert(output.end(), bytes, std::byte{0});
}

void appendGamepad(std::vector<std::byte>& output,
                   const NormalizedXInputGamepad& value) {
    appendU16(output, value.buttons);
    appendU8(output, value.leftTrigger);
    appendU8(output, value.rightTrigger);
    appendI16(output, value.thumbLX);
    appendI16(output, value.thumbLY);
    appendI16(output, value.thumbRX);
    appendI16(output, value.thumbRY);
}

bool readGamepad(PayloadReader& reader, NormalizedXInputGamepad& value) {
    return reader.readU16(value.buttons) &&
           reader.readU8(value.leftTrigger) &&
           reader.readU8(value.rightTrigger) &&
           reader.readI16(value.thumbLX) &&
           reader.readI16(value.thumbLY) &&
           reader.readI16(value.thumbRX) &&
           reader.readI16(value.thumbRY);
}

void appendCapabilities(std::vector<std::byte>& output,
                        const NormalizedXInputCapabilities& value) {
    appendU8(output, static_cast<std::uint8_t>(value.type));
    appendU8(output, value.subtype);
    appendU16(output, value.flags);
    appendGamepad(output, value.gamepad);
    appendBool(output, value.vibrationSupported);
    appendPadding(output, 3);
    appendU16(output, value.leftMotorMaximum);
    appendU16(output, value.rightMotorMaximum);
}

bool readCapabilities(PayloadReader& reader,
                      NormalizedXInputCapabilities& value) {
    std::uint8_t type = 0;
    if (!reader.readU8(type) || !reader.readU8(value.subtype) ||
        !reader.readU16(value.flags) ||
        !readGamepad(reader, value.gamepad) ||
        !reader.readBool(value.vibrationSupported) ||
        !reader.readPadding(3) ||
        !reader.readU16(value.leftMotorMaximum) ||
        !reader.readU16(value.rightMotorMaximum)) {
        return false;
    }
    value.type = static_cast<XInputCapabilityType>(type);
    return true;
}

void appendBattery(std::vector<std::byte>& output,
                   const NormalizedXInputBattery& value) {
    appendBool(output, value.available);
    appendU8(output, static_cast<std::uint8_t>(value.deviceType));
    appendU8(output, static_cast<std::uint8_t>(value.batteryType));
    appendU8(output, static_cast<std::uint8_t>(value.batteryLevel));
}

bool readBattery(PayloadReader& reader, NormalizedXInputBattery& value) {
    std::uint8_t deviceType = 0;
    std::uint8_t batteryType = 0;
    std::uint8_t batteryLevel = 0;
    if (!reader.readBool(value.available) ||
        !reader.readU8(deviceType) || !reader.readU8(batteryType) ||
        !reader.readU8(batteryLevel)) {
        return false;
    }
    value.deviceType = static_cast<XInputBatteryDeviceType>(deviceType);
    value.batteryType = static_cast<XInputBatteryType>(batteryType);
    value.batteryLevel = static_cast<XInputBatteryLevel>(batteryLevel);
    return true;
}

void appendSource(std::vector<std::byte>& output,
                  const ControllerSourceIdentity& source) {
    appendU8(output, static_cast<std::uint8_t>(source.kind));
    appendU8(output, source.runtimeXInputSlotHint);
    appendPadding(output, 2);
    appendU64(output, source.sourceKey);
}

bool readSource(PayloadReader& reader, ControllerSourceIdentity& source) {
    std::uint8_t kind = 0;
    if (!reader.readU8(kind) ||
        !reader.readU8(source.runtimeXInputSlotHint) ||
        !reader.readPadding(2) || !reader.readU64(source.sourceKey)) {
        return false;
    }
    source.kind = static_cast<ControllerSourceKind>(kind);
    return true;
}

void appendMapping(std::vector<std::byte>& output,
                   const VirtualXInputMapping& mapping) {
    appendSource(output, mapping.source);
    appendU64(output, mapping.sourceGeneration);
    appendU64(output, mapping.mappingGeneration);
}

bool readMapping(PayloadReader& reader, std::uint8_t logicalSlot,
                 VirtualXInputMapping& mapping) {
    mapping.logicalSlot = logicalSlot;
    return readSource(reader, mapping.source) &&
           reader.readU64(mapping.sourceGeneration) &&
           reader.readU64(mapping.mappingGeneration);
}

bool validVirtualXInputResult(std::uint32_t raw) noexcept {
    return raw <= static_cast<std::uint32_t>(
                      VirtualXInputResult::GenerationOverflow);
}

bool zeroGamepad(const NormalizedXInputGamepad& value) noexcept {
    return value == NormalizedXInputGamepad{};
}

bool canonicalEmptyMapping(const VirtualXInputMapping& mapping,
                           std::uint8_t logicalSlot) noexcept {
    VirtualXInputMapping empty;
    empty.logicalSlot = logicalSlot;
    return mapping == empty;
}

bool validSnapshotMapping(const VirtualXInputMapping& mapping) noexcept {
    return validControllerSourceIdentity(mapping.source) &&
           mapping.sourceGeneration != 0 &&
           mapping.mappingGeneration != 0;
}

bool validateControllerUpdateMessage(const ControllerUpdateMessage& message,
                                     std::string* error) {
    const auto fail = [error](const char* text) {
        if (error != nullptr) *error = text;
        return false;
    };
    if (message.seatId == 0) return fail("controller Seat ID is zero");
    if (message.logicalSlot >= kVirtualXInputSlotCount) {
        return fail("controller logical slot is out of range");
    }
    const auto rawKind = static_cast<std::uint8_t>(message.kind);
    if (rawKind < static_cast<std::uint8_t>(ControllerUpdateKind::Map) ||
        rawKind > static_cast<std::uint8_t>(
                      ControllerUpdateKind::Disconnect)) {
        return fail("unknown controller update kind");
    }
    const bool sourceOperation =
        message.kind != ControllerUpdateKind::Unmap;
    if (sourceOperation) {
        if (!validControllerSourceIdentity(message.source) ||
            message.sourceGeneration == 0) {
            return fail("controller source identity or generation is invalid");
        }
    } else if (message.source != ControllerSourceIdentity{} ||
               message.sourceGeneration != 0) {
        return fail("controller unmap contains a source identity");
    }

    const auto defaultCapabilities = NormalizedXInputCapabilities{};
    const auto defaultBattery = NormalizedXInputBattery{};
    switch (message.kind) {
    case ControllerUpdateKind::Map:
    case ControllerUpdateKind::Disconnect:
        if (!zeroGamepad(message.gamepad) ||
            message.capabilities != defaultCapabilities ||
            message.battery != defaultBattery) {
            return fail("controller mapping/disconnect contains state data");
        }
        break;
    case ControllerUpdateKind::Unmap:
        if (!zeroGamepad(message.gamepad) ||
            message.capabilities != defaultCapabilities ||
            message.battery != defaultBattery) {
            return fail("controller unmap contains state data");
        }
        break;
    case ControllerUpdateKind::State:
        if (message.capabilities != defaultCapabilities ||
            message.battery != defaultBattery) {
            return fail("controller state contains unrelated metadata");
        }
        break;
    case ControllerUpdateKind::Capabilities:
        if (!zeroGamepad(message.gamepad) ||
            !validXInputCapabilities(message.capabilities) ||
            message.battery != defaultBattery) {
            return fail("controller capabilities payload is invalid");
        }
        break;
    case ControllerUpdateKind::Battery:
        if (!zeroGamepad(message.gamepad) ||
            message.capabilities != defaultCapabilities ||
            !validXInputBattery(message.battery)) {
            return fail("controller battery payload is invalid");
        }
        break;
    }
    return true;
}

bool validateControllerQueryMessage(const ControllerQueryMessage& message,
                                    std::string* error) {
    const auto fail = [error](const char* text) {
        if (error != nullptr) *error = text;
        return false;
    };
    if (message.seatId == 0) return fail("controller Seat ID is zero");
    if (message.logicalSlot >= kVirtualXInputSlotCount) {
        return fail("controller logical slot is out of range");
    }
    if (message.kind == ControllerQueryKind::Snapshot) {
        if (message.expectedMappingGeneration != 0 ||
            message.expectedSourceGeneration != 0 ||
            message.leftMotor != 0 || message.rightMotor != 0) {
            return fail("controller snapshot query contains vibration data");
        }
        return true;
    }
    if (message.kind != ControllerQueryKind::Vibration) {
        return fail("unknown controller query kind");
    }
    if (message.expectedMappingGeneration == 0 ||
        message.expectedSourceGeneration == 0) {
        return fail("controller vibration generation is zero");
    }
    return true;
}

bool knownMessageType(std::uint16_t raw) noexcept {
    return raw >= static_cast<std::uint16_t>(MessageType::Hello) &&
           raw <= static_cast<std::uint16_t>(MessageType::ControllerSnapshot);
}

bool finishDecode(const DecodedFrame& frame, MessageType expected,
                  const PayloadReader& reader, std::string* error) {
    if (frame.type != expected) {
        if (error != nullptr) {
            *error = "unexpected message type";
        }
        return false;
    }
    if (!reader.error().empty()) {
        if (error != nullptr) {
            *error = reader.error();
        }
        return false;
    }
    if (!reader.finished()) {
        if (error != nullptr) {
            *error = "payload contains trailing bytes";
        }
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::vector<std::byte> wrap(MessageType type, std::uint64_t sequence,
                            std::vector<std::byte> payload) {
    return encodeFrame(type, sequence, payload);
}

} // namespace

std::vector<std::byte> encodeFrame(MessageType type,
                                   std::uint64_t sequence,
                                   std::span<const std::byte> payload) {
    if (payload.size() > kMaximumPayloadBytes ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }

    std::vector<std::byte> output;
    output.reserve(kFrameHeaderBytes + payload.size());
    appendU32(output, kProtocolMagic);
    appendU16(output, kProtocolVersion);
    appendU16(output, static_cast<std::uint16_t>(type));
    appendU32(output, static_cast<std::uint32_t>(payload.size()));
    appendU64(output, sequence);
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

FrameDecodeResult decodeFrame(std::span<const std::byte> bytes) {
    FrameDecodeResult result;
    if (bytes.size() < kFrameHeaderBytes) {
        result.error = "frame header is truncated";
        return result;
    }
    if (bytes.size() > kMaximumFrameBytes) {
        result.error = "frame exceeds maximum size";
        return result;
    }

    PayloadReader reader(bytes.first(kFrameHeaderBytes));
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t rawType = 0;
    std::uint32_t payloadSize = 0;
    std::uint64_t sequence = 0;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        !reader.readU16(rawType) || !reader.readU32(payloadSize) ||
        !reader.readU64(sequence)) {
        result.error = reader.error();
        return result;
    }
    if (magic != kProtocolMagic) {
        result.error = "protocol magic mismatch";
        return result;
    }
    if (version != kProtocolVersion) {
        result.error = "unsupported protocol version";
        return result;
    }
    if (!knownMessageType(rawType)) {
        result.error = "unknown message type";
        return result;
    }
    if (payloadSize > kMaximumPayloadBytes) {
        result.error = "payload exceeds maximum size";
        return result;
    }
    if (bytes.size() != kFrameHeaderBytes + payloadSize) {
        result.error = "frame size does not match payload size";
        return result;
    }

    DecodedFrame frame;
    frame.type = static_cast<MessageType>(rawType);
    frame.sequence = sequence;
    frame.payload.assign(bytes.begin() +
                             static_cast<std::ptrdiff_t>(kFrameHeaderBytes),
                         bytes.end());
    result.frame = std::move(frame);
    return result;
}

std::vector<std::byte> encodeHello(std::uint64_t sequence,
                                   const HelloMessage& message) {
    std::vector<std::byte> payload;
    payload.reserve(36);
    appendArray(payload, message.token);
    appendU32(payload, message.seatId);
    appendU32(payload, message.processId);
    appendU16(payload, message.architectureBits);
    appendPadding(payload, 2);
    appendU64(payload, message.targetWindow);
    return wrap(MessageType::Hello, sequence, std::move(payload));
}

std::vector<std::byte> encodeHelloAck(std::uint64_t sequence,
                                      const HelloAckMessage& message) {
    std::vector<std::byte> payload;
    payload.reserve(20);
    appendBool(payload, message.accepted);
    appendPadding(payload, 3);
    appendU32(payload, message.serverProcessId);
    appendU64(payload, message.grantedCapabilities);
    appendU32(payload, message.errorCode);
    return wrap(MessageType::HelloAck, sequence, std::move(payload));
}

std::vector<std::byte> encodeInputEvent(std::uint64_t sequence,
                                        const InputEventMessage& message) {
    std::vector<std::byte> payload;
    payload.reserve(32);
    appendU8(payload, static_cast<std::uint8_t>(message.kind));
    appendU8(payload, static_cast<std::uint8_t>(message.keyTransition));
    appendBool(payload, message.isTouchpad);
    appendPadding(payload, 1);
    appendU64(payload, message.timestampMicros);
    appendU32(payload, message.vkey);
    appendU16(payload, message.scanCode);
    appendU16(payload, message.keyboardFlags);
    appendI32(payload, message.deltaX);
    appendI32(payload, message.deltaY);
    appendU16(payload, message.mouseButtonFlags);
    appendI16(payload, message.wheelDelta);
    return wrap(MessageType::InputEvent, sequence, std::move(payload));
}

std::vector<std::byte> encodeControlState(
    std::uint64_t sequence, const ControlStateMessage& message) {
    std::vector<std::byte> payload;
    payload.reserve(28);
    appendI32(payload, message.cursorX);
    appendI32(payload, message.cursorY);
    appendBool(payload, message.clipEnabled);
    appendBool(payload, message.virtualForeground);
    appendBool(payload, message.virtualCapture);
    appendPadding(payload, 1);
    appendI32(payload, message.clipLeft);
    appendI32(payload, message.clipTop);
    appendI32(payload, message.clipRight);
    appendI32(payload, message.clipBottom);
    return wrap(MessageType::ControlState, sequence, std::move(payload));
}

std::vector<std::byte> encodeQuerySnapshot(
    std::uint64_t sequence, const QuerySnapshotMessage& message) {
    std::vector<std::byte> payload;
    payload.reserve(4);
    appendU16(payload, message.probeVkey);
    appendPadding(payload, 2);
    return wrap(MessageType::QuerySnapshot, sequence, std::move(payload));
}

std::vector<std::byte> encodeStateSnapshot(
    std::uint64_t sequence, const StateSnapshotMessage& message) {
    std::vector<std::byte> payload;
    payload.reserve(120);
    appendU64(payload, message.lastAppliedSequence);
    appendArray(payload, message.keyDownBits);
    appendArray(payload, message.keyPressedEdgeBits);
    appendU32(payload, message.mouseButtonsDown);
    appendI64(payload, message.wheelAccumulator);
    appendU16(payload, message.probeVkey);
    appendU16(payload, message.asyncKeyStateValue);
    appendU8(payload, message.keyboardStateByte);
    appendPadding(payload, 3);
    appendI32(payload, message.cursorX);
    appendI32(payload, message.cursorY);
    appendBool(payload, message.clipEnabled);
    appendBool(payload, message.virtualForeground);
    appendBool(payload, message.virtualCapture);
    appendPadding(payload, 1);
    appendI32(payload, message.clipLeft);
    appendI32(payload, message.clipTop);
    appendI32(payload, message.clipRight);
    appendI32(payload, message.clipBottom);
    return wrap(MessageType::StateSnapshot, sequence, std::move(payload));
}

std::vector<std::byte> encodeShutdown(std::uint64_t sequence) {
    return encodeFrame(MessageType::Shutdown, sequence, {});
}

std::vector<std::byte> encodeError(std::uint64_t sequence,
                                   const ErrorMessage& message) {
    std::vector<std::byte> payload;
    payload.reserve(4);
    appendU32(payload, message.errorCode);
    return wrap(MessageType::Error, sequence, std::move(payload));
}

std::vector<std::byte> encodeControllerUpdate(
    std::uint64_t sequence, const ControllerUpdateMessage& message) {
    std::vector<std::byte> payload;
    payload.reserve(68);
    appendU32(payload, message.seatId);
    appendU8(payload, static_cast<std::uint8_t>(message.kind));
    appendU8(payload, message.logicalSlot);
    appendPadding(payload, 2);
    appendSource(payload, message.source);
    appendU64(payload, message.sourceGeneration);
    appendGamepad(payload, message.gamepad);
    appendCapabilities(payload, message.capabilities);
    appendBattery(payload, message.battery);
    return wrap(MessageType::ControllerUpdate, sequence,
                std::move(payload));
}

std::vector<std::byte> encodeControllerQuery(
    std::uint64_t sequence, const ControllerQueryMessage& message) {
    std::vector<std::byte> payload;
    payload.reserve(32);
    appendU32(payload, message.seatId);
    appendU8(payload, static_cast<std::uint8_t>(message.kind));
    appendU8(payload, message.logicalSlot);
    appendPadding(payload, 2);
    appendU64(payload, message.expectedMappingGeneration);
    appendU64(payload, message.expectedSourceGeneration);
    appendU16(payload, message.leftMotor);
    appendU16(payload, message.rightMotor);
    appendPadding(payload, 4);
    return wrap(MessageType::ControllerQuery, sequence,
                std::move(payload));
}

std::vector<std::byte> encodeControllerSnapshot(
    std::uint64_t sequence, const ControllerSnapshotMessage& message) {
    std::vector<std::byte> payload;
    payload.reserve(208);
    appendU32(payload, message.seatId);
    appendU8(payload, message.logicalSlot);
    appendPadding(payload, 3);
    appendU32(payload, static_cast<std::uint32_t>(message.stateResult));
    appendU32(payload,
              static_cast<std::uint32_t>(message.capabilitiesResult));
    appendU32(payload, static_cast<std::uint32_t>(message.batteryResult));
    appendU32(payload, static_cast<std::uint32_t>(message.vibrationResult));

    appendMapping(payload, message.state.mapping);
    appendBool(payload, message.state.connected);
    appendPadding(payload, 3);
    appendU32(payload, message.state.packetNumber);
    appendGamepad(payload, message.state.gamepad);

    appendMapping(payload, message.capabilities.mapping);
    appendCapabilities(payload, message.capabilities.capabilities);

    appendMapping(payload, message.battery.mapping);
    appendBattery(payload, message.battery.battery);

    appendSource(payload, message.vibration.source);
    appendU64(payload, message.vibration.sourceGeneration);
    appendU64(payload, message.vibration.mappingGeneration);
    appendU64(payload, message.vibration.commandSequence);
    appendU64(payload, message.vibration.routeCount);
    appendU16(payload, message.vibration.leftMotor);
    appendU16(payload, message.vibration.rightMotor);
    appendPadding(payload, 4);
    return wrap(MessageType::ControllerSnapshot, sequence,
                std::move(payload));
}

bool decodeHello(const DecodedFrame& frame, HelloMessage& message,
                 std::string* error) {
    PayloadReader reader(frame.payload);
    std::uint16_t architecture = 0;
    if (!reader.readArray(message.token) || !reader.readU32(message.seatId) ||
        !reader.readU32(message.processId) ||
        !reader.readU16(architecture) || !reader.readPadding(2) ||
        !reader.readU64(message.targetWindow)) {
        if (error != nullptr) *error = reader.error();
        return false;
    }
    if (architecture != 32 && architecture != 64) {
        if (error != nullptr) *error = "architecture must be 32 or 64 bits";
        return false;
    }
    message.architectureBits = architecture;
    return finishDecode(frame, MessageType::Hello, reader, error);
}

bool decodeHelloAck(const DecodedFrame& frame, HelloAckMessage& message,
                    std::string* error) {
    PayloadReader reader(frame.payload);
    if (!reader.readBool(message.accepted) || !reader.readPadding(3) ||
        !reader.readU32(message.serverProcessId) ||
        !reader.readU64(message.grantedCapabilities) ||
        !reader.readU32(message.errorCode)) {
        if (error != nullptr) *error = reader.error();
        return false;
    }
    return finishDecode(frame, MessageType::HelloAck, reader, error);
}

bool decodeInputEvent(const DecodedFrame& frame, InputEventMessage& message,
                      std::string* error) {
    PayloadReader reader(frame.payload);
    std::uint8_t rawKind = 0;
    std::uint8_t rawTransition = 0;
    if (!reader.readU8(rawKind) || !reader.readU8(rawTransition) ||
        !reader.readBool(message.isTouchpad) || !reader.readPadding(1) ||
        !reader.readU64(message.timestampMicros) ||
        !reader.readU32(message.vkey) || !reader.readU16(message.scanCode) ||
        !reader.readU16(message.keyboardFlags) ||
        !reader.readI32(message.deltaX) || !reader.readI32(message.deltaY) ||
        !reader.readU16(message.mouseButtonFlags) ||
        !reader.readI16(message.wheelDelta)) {
        if (error != nullptr) *error = reader.error();
        return false;
    }
    if (rawKind < static_cast<std::uint8_t>(InputKind::Keyboard) ||
        rawKind > static_cast<std::uint8_t>(InputKind::Mouse)) {
        if (error != nullptr) *error = "unknown input kind";
        return false;
    }
    if (rawTransition > static_cast<std::uint8_t>(KeyTransition::Up)) {
        if (error != nullptr) *error = "unknown key transition";
        return false;
    }
    message.kind = static_cast<InputKind>(rawKind);
    message.keyTransition = static_cast<KeyTransition>(rawTransition);
    if (message.kind == InputKind::Keyboard) {
        if (message.vkey >= 256) {
            if (error != nullptr) *error = "keyboard virtual key is out of range";
            return false;
        }
        if (message.keyTransition != KeyTransition::Down &&
            message.keyTransition != KeyTransition::Up) {
            if (error != nullptr) *error = "keyboard event requires a down/up transition";
            return false;
        }
    } else if (message.keyTransition != KeyTransition::None) {
        if (error != nullptr) *error = "mouse event must not contain a key transition";
        return false;
    }
    return finishDecode(frame, MessageType::InputEvent, reader, error);
}

bool decodeControlState(const DecodedFrame& frame,
                        ControlStateMessage& message,
                        std::string* error) {
    PayloadReader reader(frame.payload);
    if (!reader.readI32(message.cursorX) || !reader.readI32(message.cursorY) ||
        !reader.readBool(message.clipEnabled) ||
        !reader.readBool(message.virtualForeground) ||
        !reader.readBool(message.virtualCapture) || !reader.readPadding(1) ||
        !reader.readI32(message.clipLeft) ||
        !reader.readI32(message.clipTop) ||
        !reader.readI32(message.clipRight) ||
        !reader.readI32(message.clipBottom)) {
        if (error != nullptr) *error = reader.error();
        return false;
    }
    if (message.clipEnabled &&
        (message.clipRight <= message.clipLeft ||
         message.clipBottom <= message.clipTop)) {
        if (error != nullptr) *error = "clip rectangle is invalid";
        return false;
    }
    return finishDecode(frame, MessageType::ControlState, reader, error);
}

bool decodeQuerySnapshot(const DecodedFrame& frame,
                         QuerySnapshotMessage& message,
                         std::string* error) {
    PayloadReader reader(frame.payload);
    if (!reader.readU16(message.probeVkey) || !reader.readPadding(2)) {
        if (error != nullptr) *error = reader.error();
        return false;
    }
    if (message.probeVkey != 0xffffu && message.probeVkey >= 256u) {
        if (error != nullptr) *error = "snapshot probe virtual key is out of range";
        return false;
    }
    return finishDecode(frame, MessageType::QuerySnapshot, reader, error);
}

bool decodeStateSnapshot(const DecodedFrame& frame,
                         StateSnapshotMessage& message,
                         std::string* error) {
    PayloadReader reader(frame.payload);
    if (!reader.readU64(message.lastAppliedSequence) ||
        !reader.readArray(message.keyDownBits) ||
        !reader.readArray(message.keyPressedEdgeBits) ||
        !reader.readU32(message.mouseButtonsDown) ||
        !reader.readI64(message.wheelAccumulator) ||
        !reader.readU16(message.probeVkey) ||
        !reader.readU16(message.asyncKeyStateValue) ||
        !reader.readU8(message.keyboardStateByte) ||
        !reader.readPadding(3) ||
        !reader.readI32(message.cursorX) || !reader.readI32(message.cursorY) ||
        !reader.readBool(message.clipEnabled) ||
        !reader.readBool(message.virtualForeground) ||
        !reader.readBool(message.virtualCapture) || !reader.readPadding(1) ||
        !reader.readI32(message.clipLeft) ||
        !reader.readI32(message.clipTop) ||
        !reader.readI32(message.clipRight) ||
        !reader.readI32(message.clipBottom)) {
        if (error != nullptr) *error = reader.error();
        return false;
    }
    if (message.probeVkey != 0xffffu && message.probeVkey >= 256u) {
        if (error != nullptr) *error = "snapshot probe virtual key is out of range";
        return false;
    }
    if (message.clipEnabled &&
        (message.clipRight <= message.clipLeft ||
         message.clipBottom <= message.clipTop)) {
        if (error != nullptr) *error = "snapshot clip rectangle is invalid";
        return false;
    }
    return finishDecode(frame, MessageType::StateSnapshot, reader, error);
}

bool decodeShutdown(const DecodedFrame& frame, std::string* error) {
    PayloadReader reader(frame.payload);
    return finishDecode(frame, MessageType::Shutdown, reader, error);
}

bool decodeError(const DecodedFrame& frame, ErrorMessage& message,
                 std::string* error) {
    PayloadReader reader(frame.payload);
    if (!reader.readU32(message.errorCode)) {
        if (error != nullptr) *error = reader.error();
        return false;
    }
    return finishDecode(frame, MessageType::Error, reader, error);
}

bool decodeControllerUpdate(const DecodedFrame& frame,
                            ControllerUpdateMessage& message,
                            std::string* error) {
    PayloadReader reader(frame.payload);
    std::uint8_t kind = 0;
    if (!reader.readU32(message.seatId) || !reader.readU8(kind) ||
        !reader.readU8(message.logicalSlot) || !reader.readPadding(2) ||
        !readSource(reader, message.source) ||
        !reader.readU64(message.sourceGeneration) ||
        !readGamepad(reader, message.gamepad) ||
        !readCapabilities(reader, message.capabilities) ||
        !readBattery(reader, message.battery)) {
        if (error != nullptr) *error = reader.error();
        return false;
    }
    message.kind = static_cast<ControllerUpdateKind>(kind);
    if (!validateControllerUpdateMessage(message, error)) return false;
    return finishDecode(frame, MessageType::ControllerUpdate, reader,
                        error);
}

bool decodeControllerQuery(const DecodedFrame& frame,
                           ControllerQueryMessage& message,
                           std::string* error) {
    PayloadReader reader(frame.payload);
    std::uint8_t kind = 0;
    if (!reader.readU32(message.seatId) || !reader.readU8(kind) ||
        !reader.readU8(message.logicalSlot) || !reader.readPadding(2) ||
        !reader.readU64(message.expectedMappingGeneration) ||
        !reader.readU64(message.expectedSourceGeneration) ||
        !reader.readU16(message.leftMotor) ||
        !reader.readU16(message.rightMotor) || !reader.readPadding(4)) {
        if (error != nullptr) *error = reader.error();
        return false;
    }
    message.kind = static_cast<ControllerQueryKind>(kind);
    if (!validateControllerQueryMessage(message, error)) return false;
    return finishDecode(frame, MessageType::ControllerQuery, reader, error);
}

bool decodeControllerSnapshot(const DecodedFrame& frame,
                              ControllerSnapshotMessage& message,
                              std::string* error) {
    PayloadReader reader(frame.payload);
    std::uint32_t stateResult = 0;
    std::uint32_t capabilitiesResult = 0;
    std::uint32_t batteryResult = 0;
    std::uint32_t vibrationResult = 0;
    if (!reader.readU32(message.seatId) ||
        !reader.readU8(message.logicalSlot) || !reader.readPadding(3) ||
        !reader.readU32(stateResult) ||
        !reader.readU32(capabilitiesResult) ||
        !reader.readU32(batteryResult) ||
        !reader.readU32(vibrationResult) ||
        !readMapping(reader, message.logicalSlot, message.state.mapping) ||
        !reader.readBool(message.state.connected) ||
        !reader.readPadding(3) ||
        !reader.readU32(message.state.packetNumber) ||
        !readGamepad(reader, message.state.gamepad) ||
        !readMapping(reader, message.logicalSlot,
                     message.capabilities.mapping) ||
        !readCapabilities(reader, message.capabilities.capabilities) ||
        !readMapping(reader, message.logicalSlot, message.battery.mapping) ||
        !readBattery(reader, message.battery.battery) ||
        !readSource(reader, message.vibration.source) ||
        !reader.readU64(message.vibration.sourceGeneration) ||
        !reader.readU64(message.vibration.mappingGeneration) ||
        !reader.readU64(message.vibration.commandSequence) ||
        !reader.readU64(message.vibration.routeCount) ||
        !reader.readU16(message.vibration.leftMotor) ||
        !reader.readU16(message.vibration.rightMotor) ||
        !reader.readPadding(4)) {
        if (error != nullptr) *error = reader.error();
        return false;
    }
    if (!validVirtualXInputResult(stateResult) ||
        !validVirtualXInputResult(capabilitiesResult) ||
        !validVirtualXInputResult(batteryResult) ||
        !validVirtualXInputResult(vibrationResult)) {
        if (error != nullptr) *error = "unknown controller result";
        return false;
    }
    message.stateResult = static_cast<VirtualXInputResult>(stateResult);
    message.capabilitiesResult =
        static_cast<VirtualXInputResult>(capabilitiesResult);
    message.batteryResult =
        static_cast<VirtualXInputResult>(batteryResult);
    message.vibrationResult =
        static_cast<VirtualXInputResult>(vibrationResult);
    message.vibration.logicalSlot = message.logicalSlot;

    if (message.seatId == 0 ||
        message.logicalSlot >= kVirtualXInputSlotCount) {
        if (error != nullptr) *error = "controller snapshot authority or slot is invalid";
        return false;
    }
    switch (message.stateResult) {
    case VirtualXInputResult::Success:
        if (!message.state.connected ||
            !validSnapshotMapping(message.state.mapping)) {
            if (error != nullptr) {
                *error = "connected controller snapshot state is invalid";
            }
            return false;
        }
        break;
    case VirtualXInputResult::Disconnected:
        if (message.state.connected || !zeroGamepad(message.state.gamepad) ||
            !validSnapshotMapping(message.state.mapping)) {
            if (error != nullptr) {
                *error = "disconnected controller snapshot state is invalid";
            }
            return false;
        }
        break;
    case VirtualXInputResult::NotMapped:
        if (message.state.connected || message.state.packetNumber != 0 ||
            !zeroGamepad(message.state.gamepad) ||
            !canonicalEmptyMapping(message.state.mapping,
                                   message.logicalSlot)) {
            if (error != nullptr) {
                *error = "not-mapped controller snapshot state is not empty";
            }
            return false;
        }
        break;
    default:
        if (error != nullptr) {
            *error = "controller snapshot has an invalid state result";
        }
        return false;
    }

    if (message.capabilitiesResult == VirtualXInputResult::Success) {
        if (message.stateResult != VirtualXInputResult::Success ||
            !validXInputCapabilities(message.capabilities.capabilities) ||
            message.capabilities.mapping != message.state.mapping) {
            if (error != nullptr) {
                *error = "controller snapshot capabilities are inconsistent";
            }
            return false;
        }
    } else if (!canonicalEmptyMapping(message.capabilities.mapping,
                                      message.logicalSlot) ||
               message.capabilities.capabilities !=
                   NormalizedXInputCapabilities{}) {
        if (error != nullptr) {
            *error = "failed controller capabilities retain metadata";
        }
        return false;
    }

    if (message.batteryResult == VirtualXInputResult::Success) {
        if (message.stateResult != VirtualXInputResult::Success ||
            !validXInputBattery(message.battery.battery) ||
            message.battery.mapping != message.state.mapping) {
            if (error != nullptr) {
                *error = "controller snapshot battery is inconsistent";
            }
            return false;
        }
    } else if (!canonicalEmptyMapping(message.battery.mapping,
                                      message.logicalSlot) ||
               message.battery.battery != NormalizedXInputBattery{}) {
        if (error != nullptr) {
            *error = "failed controller battery retains metadata";
        }
        return false;
    }
    if (message.vibrationResult == VirtualXInputResult::Success &&
        (message.stateResult != VirtualXInputResult::Success ||
         !validControllerSourceIdentity(message.vibration.source) ||
         message.vibration.sourceGeneration == 0 ||
         message.vibration.mappingGeneration == 0 ||
         message.vibration.commandSequence == 0 ||
         message.vibration.routeCount == 0 ||
         (message.stateResult == VirtualXInputResult::Success &&
          (message.vibration.source != message.state.mapping.source ||
           message.vibration.sourceGeneration !=
               message.state.mapping.sourceGeneration ||
           message.vibration.mappingGeneration !=
               message.state.mapping.mappingGeneration)))) {
        if (error != nullptr) *error = "controller snapshot vibration route is inconsistent";
        return false;
    }
    if (message.vibrationResult != VirtualXInputResult::Success &&
        (validControllerSourceIdentity(message.vibration.source) ||
         message.vibration.sourceGeneration != 0 ||
         message.vibration.mappingGeneration != 0 ||
         message.vibration.commandSequence != 0 ||
         message.vibration.routeCount != 0 ||
         message.vibration.leftMotor != 0 ||
         message.vibration.rightMotor != 0)) {
        if (error != nullptr) *error = "failed controller vibration retains a route";
        return false;
    }
    return finishDecode(frame, MessageType::ControllerSnapshot, reader,
                        error);
}

bool controllerSeatAuthorityMatches(std::uint32_t expectedSeatId,
                                    std::uint32_t messageSeatId) noexcept {
    return expectedSeatId != 0 && expectedSeatId == messageSeatId;
}

std::string tokenToHex(const SessionToken& token) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(token.size() * 2);
    for (const auto byte : token) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

std::optional<SessionToken> tokenFromHex(std::string_view text) {
    if (text.size() != SessionToken{}.size() * 2) {
        return std::nullopt;
    }
    const auto nibble = [](char value) -> std::optional<std::uint8_t> {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint8_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<std::uint8_t>(value - 'a' + 10);
        }
        if (value >= 'A' && value <= 'F') {
            return static_cast<std::uint8_t>(value - 'A' + 10);
        }
        return std::nullopt;
    };

    SessionToken token{};
    for (std::size_t index = 0; index < token.size(); ++index) {
        const auto high = nibble(text[index * 2]);
        const auto low = nibble(text[index * 2 + 1]);
        if (!high || !low) {
            return std::nullopt;
        }
        token[index] = static_cast<std::uint8_t>((*high << 4) | *low);
    }
    return token;
}

std::string_view messageTypeName(MessageType type) noexcept {
    switch (type) {
    case MessageType::Hello: return "Hello";
    case MessageType::HelloAck: return "HelloAck";
    case MessageType::InputEvent: return "InputEvent";
    case MessageType::ControlState: return "ControlState";
    case MessageType::QuerySnapshot: return "QuerySnapshot";
    case MessageType::StateSnapshot: return "StateSnapshot";
    case MessageType::Shutdown: return "Shutdown";
    case MessageType::Error: return "Error";
    case MessageType::ProbeSnapshot: return "ProbeSnapshot";
    case MessageType::ControllerUpdate: return "ControllerUpdate";
    case MessageType::ControllerQuery: return "ControllerQuery";
    case MessageType::ControllerSnapshot: return "ControllerSnapshot";
    }
    return "Unknown";
}

} // namespace hydra::gatec
