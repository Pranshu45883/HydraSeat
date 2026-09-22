#pragma once

#include <cstdint>

namespace hydra::hid {

inline constexpr std::uint16_t kDigitizerTouchpadUsage = 0x05u;

enum class CollectionKind {
    Other,
    Mouse,
    Touchpad,
    Keyboard,
    Joystick,
    Gamepad
};

constexpr CollectionKind classifyCollection(uint16_t usagePage, uint16_t usage) {
    constexpr uint16_t kGenericDesktopPage = 0x01;
    constexpr uint16_t kDigitizersPage = 0x0D;

    if (usagePage == kGenericDesktopPage) {
        switch (usage) {
        case 0x02:
            return CollectionKind::Mouse;
        case 0x04:
            return CollectionKind::Joystick;
        case 0x05:
            return CollectionKind::Gamepad;
        case 0x06:
            return CollectionKind::Keyboard;
        default:
            break;
        }
    }

    if (usagePage == kDigitizersPage && usage == kDigitizerTouchpadUsage) {
        return CollectionKind::Touchpad;
    }

    return CollectionKind::Other;
}

constexpr bool isMouseLikeCollection(uint16_t usagePage, uint16_t usage) {
    const auto kind = classifyCollection(usagePage, usage);
    return kind == CollectionKind::Mouse || kind == CollectionKind::Touchpad;
}

} // namespace hydra::hid
