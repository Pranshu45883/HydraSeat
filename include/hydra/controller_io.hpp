#pragma once

#include "hydra/controller_identity.hpp"

#include <cstdint>
#include <optional>

namespace hydra::controller {

enum class IoStatus : std::uint8_t {
    Ok = 0,
    InvalidBinding = 1,
    UnsupportedApi = 2,
    Disconnected = 3,
    PlatformUnavailable = 4,
    NativeFailure = 5,
};

struct GamepadState {
    std::uint32_t packetNumber{0};
    std::uint16_t buttons{0};
    std::uint8_t leftTrigger{0};
    std::uint8_t rightTrigger{0};
    std::int16_t thumbLX{0};
    std::int16_t thumbLY{0};
    std::int16_t thumbRX{0};
    std::int16_t thumbRY{0};

    bool operator==(const GamepadState&) const = default;
};

struct PollResult {
    IoStatus status{IoStatus::InvalidBinding};
    std::optional<GamepadState> state;
};

// Executes only the runtime control surface declared by the binding. Stable
// GameInput/DirectInput bindings never silently degrade to an XInput slot.
PollResult pollBoundController(const SeatBinding& binding) noexcept;
IoStatus setBoundControllerVibration(const SeatBinding& binding,
                                     std::uint16_t leftMotor,
                                     std::uint16_t rightMotor) noexcept;

} // namespace hydra::controller
