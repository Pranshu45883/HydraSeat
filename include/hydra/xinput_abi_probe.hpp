#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace hydra::controller::abi_probe {

enum class ProbeMode : std::uint8_t {
    Snapshot = 1,
    Vibrate = 2,
};

struct ProbeOptions {
    std::wstring dllPath;
    ProbeMode mode{ProbeMode::Snapshot};
    std::uint16_t lowFrequencyMotor{0};
    std::uint16_t highFrequencyMotor{0};

    bool operator==(const ProbeOptions&) const = default;
};

std::optional<ProbeOptions> parseProbeArgs(
    std::span<const std::wstring> args) noexcept;

std::string formatCapabilitiesLine(
    std::uint32_t status,
    std::uint8_t type,
    std::uint8_t subtype);

std::string formatStateLine(
    std::uint32_t slot,
    std::uint32_t status,
    std::uint32_t packetNumber,
    std::uint16_t buttons,
    std::int16_t thumbLX);

std::string formatVibrationLine(std::uint32_t status);

} // namespace hydra::controller::abi_probe
