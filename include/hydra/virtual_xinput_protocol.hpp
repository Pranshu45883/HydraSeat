#pragma once

#include "hydra/controller_io.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace hydra::controller::ipc {

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kEncodedRequestSize = 28;
inline constexpr std::size_t kEncodedResponseSize = 20;

enum class ProtocolOpcode : std::uint8_t {
    Ping = 1,
    GetState = 2,
    SetVibration = 3,
};

enum class ProtocolStatus : std::uint8_t {
    Ok = 0,
    InvalidRequest = 1,
    UnsupportedVersion = 2,
    InvalidMapping = 3,
    Disconnected = 4,
    StaleBinding = 5,
    BackendFailure = 6,
};

struct VirtualXInputRequest {
    ProtocolOpcode opcode{ProtocolOpcode::Ping};
    std::uint32_t seatId{0};
    std::uint64_t activationGeneration{0};
    std::uint64_t sourceGeneration{0};
    std::uint8_t logicalSlot{0};
    std::uint16_t lowFrequencyMotor{0};
    std::uint16_t highFrequencyMotor{0};

    bool operator==(const VirtualXInputRequest&) const = default;
};

struct VirtualXInputResponse {
    ProtocolStatus status{ProtocolStatus::InvalidRequest};
    GamepadState state{};
    bool hasState{false};

    bool operator==(const VirtualXInputResponse&) const = default;
};

std::vector<std::uint8_t> encodeRequest(const VirtualXInputRequest& request);
std::optional<VirtualXInputRequest> decodeRequest(
    const std::vector<std::uint8_t>& bytes) noexcept;

std::vector<std::uint8_t> encodeResponse(const VirtualXInputResponse& response);
std::optional<VirtualXInputResponse> decodeResponse(
    const std::vector<std::uint8_t>& bytes) noexcept;

} // namespace hydra::controller::ipc
