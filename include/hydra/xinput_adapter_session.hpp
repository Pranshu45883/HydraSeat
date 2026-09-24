#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hydra::controller::adapter {

struct SessionConfig {
    std::wstring pipeEndpoint;
    std::uint32_t seatId{0};
    std::uint64_t activationGeneration{0};
    std::uint64_t sourceGeneration{0};

    bool valid() const noexcept;
    bool operator==(const SessionConfig&) const = default;
};

std::optional<SessionConfig> parseSessionConfig(
    std::wstring pipeEndpoint,
    std::wstring_view seatId,
    std::wstring_view activationGeneration,
    std::wstring_view sourceGeneration) noexcept;

std::optional<SessionConfig> loadSessionConfigFromEnvironment() noexcept;

} // namespace hydra::controller::adapter
