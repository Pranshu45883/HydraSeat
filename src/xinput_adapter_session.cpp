#include "hydra/xinput_adapter_session.hpp"

#include <array>
#include <limits>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace hydra::controller::adapter {
namespace {

template <typename T>
bool parseUnsigned(std::wstring_view text, T& value) noexcept {
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

#if defined(_WIN32)
std::optional<std::wstring> environmentValue(const wchar_t* name) noexcept {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetEnvironmentVariableW(
        name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::nullopt;
    return std::wstring(buffer.data(), length);
}
#endif

} // namespace

bool SessionConfig::valid() const noexcept {
    return !pipeEndpoint.empty() && (seatId == 1 || seatId == 2) &&
           activationGeneration != 0 && sourceGeneration != 0;
}

std::optional<SessionConfig> parseSessionConfig(
    std::wstring pipeEndpoint,
    std::wstring_view seatId,
    std::wstring_view activationGeneration,
    std::wstring_view sourceGeneration) noexcept {
    SessionConfig config;
    config.pipeEndpoint = std::move(pipeEndpoint);
    if (!parseUnsigned(seatId, config.seatId) ||
        !parseUnsigned(activationGeneration, config.activationGeneration) ||
        !parseUnsigned(sourceGeneration, config.sourceGeneration) ||
        !config.valid()) {
        return std::nullopt;
    }
    return config;
}

std::optional<SessionConfig> loadSessionConfigFromEnvironment() noexcept {
#if defined(_WIN32)
    const auto pipe = environmentValue(L"HYDRA_XINPUT_PIPE");
    const auto seat = environmentValue(L"HYDRA_XINPUT_SEAT_ID");
    const auto activation = environmentValue(L"HYDRA_XINPUT_ACTIVATION_GENERATION");
    const auto source = environmentValue(L"HYDRA_XINPUT_SOURCE_GENERATION");
    if (!pipe || !seat || !activation || !source) return std::nullopt;
    return parseSessionConfig(*pipe, *seat, *activation, *source);
#else
    return std::nullopt;
#endif
}

} // namespace hydra::controller::adapter
