#include "hydra/virtual_xinput_protocol.hpp"

namespace hydra::controller::ipc {
namespace {

void writeU16(std::vector<std::uint8_t>& out, std::size_t offset,
              std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
}

void writeU32(std::vector<std::uint8_t>& out, std::size_t offset,
              std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu);
    }
}

void writeU64(std::vector<std::uint8_t>& out, std::size_t offset,
              std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu);
    }
}

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8u);
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8u);
    }
    return value;
}

std::uint64_t readU64(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8u);
    }
    return value;
}

bool validOpcode(std::uint8_t raw) noexcept {
    return raw == static_cast<std::uint8_t>(ProtocolOpcode::Ping) ||
           raw == static_cast<std::uint8_t>(ProtocolOpcode::GetState) ||
           raw == static_cast<std::uint8_t>(ProtocolOpcode::SetVibration);
}

bool validStatus(std::uint8_t raw) noexcept {
    return raw <= static_cast<std::uint8_t>(ProtocolStatus::BackendFailure);
}

} // namespace

std::vector<std::uint8_t> encodeRequest(const VirtualXInputRequest& request) {
    std::vector<std::uint8_t> out(kEncodedRequestSize, 0);
    writeU16(out, 0, kProtocolVersion);
    out[2] = static_cast<std::uint8_t>(request.opcode);
    writeU32(out, 3, request.seatId);
    writeU64(out, 7, request.activationGeneration);
    writeU64(out, 15, request.sourceGeneration);
    out[23] = request.logicalSlot;
    writeU16(out, 24, request.lowFrequencyMotor);
    writeU16(out, 26, request.highFrequencyMotor);
    return out;
}

std::optional<VirtualXInputRequest> decodeRequest(
    const std::vector<std::uint8_t>& bytes) noexcept {
    if (bytes.size() != kEncodedRequestSize || readU16(bytes, 0) != kProtocolVersion ||
        !validOpcode(bytes[2])) {
        return std::nullopt;
    }

    VirtualXInputRequest request;
    request.opcode = static_cast<ProtocolOpcode>(bytes[2]);
    request.seatId = readU32(bytes, 3);
    request.activationGeneration = readU64(bytes, 7);
    request.sourceGeneration = readU64(bytes, 15);
    request.logicalSlot = bytes[23];
    request.lowFrequencyMotor = readU16(bytes, 24);
    request.highFrequencyMotor = readU16(bytes, 26);

    if (request.logicalSlot > 3) return std::nullopt;
    if (request.opcode != ProtocolOpcode::Ping &&
        (request.seatId != 1 && request.seatId != 2)) {
        return std::nullopt;
    }
    return request;
}

std::vector<std::uint8_t> encodeResponse(const VirtualXInputResponse& response) {
    std::vector<std::uint8_t> out(kEncodedResponseSize, 0);
    writeU16(out, 0, kProtocolVersion);
    out[2] = static_cast<std::uint8_t>(response.status);
    out[3] = response.hasState ? 1u : 0u;
    writeU32(out, 4, response.state.packetNumber);
    writeU16(out, 8, response.state.buttons);
    out[10] = response.state.leftTrigger;
    out[11] = response.state.rightTrigger;
    writeU16(out, 12, static_cast<std::uint16_t>(response.state.thumbLX));
    writeU16(out, 14, static_cast<std::uint16_t>(response.state.thumbLY));
    writeU16(out, 16, static_cast<std::uint16_t>(response.state.thumbRX));
    writeU16(out, 18, static_cast<std::uint16_t>(response.state.thumbRY));
    return out;
}

std::optional<VirtualXInputResponse> decodeResponse(
    const std::vector<std::uint8_t>& bytes) noexcept {
    if (bytes.size() != kEncodedResponseSize || readU16(bytes, 0) != kProtocolVersion ||
        !validStatus(bytes[2]) || bytes[3] > 1u) {
        return std::nullopt;
    }

    VirtualXInputResponse response;
    response.status = static_cast<ProtocolStatus>(bytes[2]);
    response.hasState = bytes[3] != 0;
    response.state.packetNumber = readU32(bytes, 4);
    response.state.buttons = readU16(bytes, 8);
    response.state.leftTrigger = bytes[10];
    response.state.rightTrigger = bytes[11];
    response.state.thumbLX = static_cast<std::int16_t>(readU16(bytes, 12));
    response.state.thumbLY = static_cast<std::int16_t>(readU16(bytes, 14));
    response.state.thumbRX = static_cast<std::int16_t>(readU16(bytes, 16));
    response.state.thumbRY = static_cast<std::int16_t>(readU16(bytes, 18));
    return response;
}

} // namespace hydra::controller::ipc
