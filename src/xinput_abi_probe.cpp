#include "hydra/xinput_abi_probe.hpp"

#include <limits>
#include <type_traits>

namespace hydra::controller::abi_probe {
namespace {

template <typename T>
bool parseUnsigned(const std::wstring& text, T& value) noexcept {
    static_assert(std::is_unsigned_v<T>);
    if (text.empty()) return false;

    T parsed = 0;
    for (const wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
        const T digit = static_cast<T>(ch - L'0');
        if (parsed > ((std::numeric_limits<T>::max)() - digit) / 10) return false;
        parsed = static_cast<T>(parsed * 10 + digit);
    }
    value = parsed;
    return true;
}

} // namespace

std::optional<ProbeOptions> parseProbeArgs(
    std::span<const std::wstring> args) noexcept {
    ProbeOptions options;
    bool haveDll = false;
    bool haveMode = false;
    bool haveLow = false;
    bool haveHigh = false;

    for (std::size_t i = 0; i < args.size();) {
        if (i + 1 >= args.size()) return std::nullopt;
        const auto& key = args[i];
        const auto& value = args[i + 1];
        i += 2;

        if (key == L"--dll") {
            if (haveDll || value.empty()) return std::nullopt;
            options.dllPath = value;
            haveDll = true;
        } else if (key == L"--mode") {
            if (haveMode) return std::nullopt;
            if (value == L"snapshot") {
                options.mode = ProbeMode::Snapshot;
            } else if (value == L"vibrate") {
                options.mode = ProbeMode::Vibrate;
            } else {
                return std::nullopt;
            }
            haveMode = true;
        } else if (key == L"--low") {
            if (haveLow || !parseUnsigned(value, options.lowFrequencyMotor)) {
                return std::nullopt;
            }
            haveLow = true;
        } else if (key == L"--high") {
            if (haveHigh || !parseUnsigned(value, options.highFrequencyMotor)) {
                return std::nullopt;
            }
            haveHigh = true;
        } else {
            return std::nullopt;
        }
    }

    if (!haveDll || !haveMode) return std::nullopt;
    if (options.mode == ProbeMode::Vibrate) {
        if (!haveLow || !haveHigh) return std::nullopt;
    } else if (haveLow || haveHigh) {
        return std::nullopt;
    }
    return options;
}

std::string formatCapabilitiesLine(
    std::uint32_t status,
    std::uint8_t type,
    std::uint8_t subtype) {
    return "capabilities status=" + std::to_string(status) +
           " type=" + std::to_string(type) +
           " subtype=" + std::to_string(subtype);
}

std::string formatStateLine(
    std::uint32_t slot,
    std::uint32_t status,
    std::uint32_t packetNumber,
    std::uint16_t buttons,
    std::int16_t thumbLX) {
    std::string line = "slot=" + std::to_string(slot) +
                       " status=" + std::to_string(status);
    if (status == 0) {
        line += " packet=" + std::to_string(packetNumber);
        line += " buttons=" + std::to_string(buttons);
        line += " lx=" + std::to_string(thumbLX);
    }
    return line;
}

std::string formatVibrationLine(std::uint32_t status) {
    return "vibration status=" + std::to_string(status);
}

} // namespace hydra::controller::abi_probe
