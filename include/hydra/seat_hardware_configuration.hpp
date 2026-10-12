#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace hydra::runtime {

struct SeatHardwareConfiguration {
    std::uint32_t seatId{0};
    std::wstring displayId;
    std::wstring keyboardId;
    std::wstring mouseId;
    std::wstring controllerId;

    bool operator==(const SeatHardwareConfiguration&) const = default;
};

using SeatHardwareConfigurations =
    std::array<SeatHardwareConfiguration, 2>;

} // namespace hydra::runtime
