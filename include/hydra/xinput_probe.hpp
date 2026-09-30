#pragma once

#include "hydra/virtual_xinput_pipe.hpp"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>

namespace hydra::controller::probe {

enum class ProbeMode : std::uint8_t {
    Snapshot = 1,
    Vibrate = 2,
};

struct ProbeOptions {
    std::wstring pipeEndpoint;
    std::uint32_t seatId{0};
    std::uint64_t activationGeneration{0};
    std::uint64_t sourceGeneration{0};
    ProbeMode mode{ProbeMode::Snapshot};
    std::uint16_t lowFrequencyMotor{0};
    std::uint16_t highFrequencyMotor{0};

    bool operator==(const ProbeOptions&) const = default;
};

std::optional<ProbeOptions> parseProbeArgs(
    std::span<const std::wstring> args) noexcept;

std::string protocolStatusName(ipc::ProtocolStatus status);
std::string formatSnapshotLine(
    std::uint8_t logicalSlot,
    const ipc::VirtualXInputResponse& response);
std::string formatVibrationLine(
    const ipc::VirtualXInputResponse& response);

int runProbe(const ProbeOptions& options,
             std::ostream& output,
             std::uint32_t timeoutMs = 2000) noexcept;

} // namespace hydra::controller::probe
