#pragma once

#include "hydra/gate_c_protocol.hpp"
#include "hydra/gate_c_shim_api.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hydra::gatec {

inline constexpr std::string_view kP3EGlfwProfileId =
    "glfw-3.5.1-cursor-test";
inline constexpr std::string_view kP3EGlfwVersion = "3.5.1";
inline constexpr std::string_view kP3EGlfwCommit =
    "d9d6f0f1f967807ffade6598ea9a631ebaf37a56";

// Measured from the MSVC x64 static GLFW 3.5.1 tests/cursor executable.
// GetKeyState; Get/SetCursorPos; ClipCursor; GetActiveWindow;
// Set/ReleaseCapture; RegisterRawInputDevices; GetRawInputData.
inline constexpr std::uint32_t kP3EGlfwRequiredApiMask = 0x0000b93au;

inline constexpr std::uint32_t kExternalBridgeConfigMagic = 0x31453350u; // P3E1
inline constexpr std::uint32_t kExternalBridgeConfigVersion = 1u;
inline constexpr std::size_t kExternalBridgePipeNameChars = 256u;
// The all-bits value is reserved as an external-bridge request to discover the
// target executable's actually imported supported APIs before installing the
// process-local shim. Explicit measured profiles continue to pass an exact mask.
inline constexpr std::uint32_t kExternalBridgeAutoDetectApiMask =
    HYDRA_GATE_C_SHIM_ALL_API_MASK;

constexpr bool validProfiledShimMask(std::uint32_t mask) noexcept {
    // A real game may consume keyboard/mouse solely through Raw Input and have
    // no GetAsyncKeyState/GetKeyState imports at all. Requiring a polling API
    // made those otherwise supported targets fail during auto-detection.
    constexpr std::uint32_t kInputApiMask =
        HYDRA_GATE_C_SHIM_POLLING_API_MASK |
        HYDRA_GATE_C_SHIM_RAW_INPUT_API_MASK;
    return mask != 0u &&
           (mask & kInputApiMask) != 0u &&
           (mask & ~HYDRA_GATE_C_SHIM_ALL_API_MASK) == 0u;
}

#ifdef _WIN32
#pragma pack(push, 1)
struct ExternalBridgeConfigV1 {
    std::uint32_t structSize{sizeof(ExternalBridgeConfigV1)};
    std::uint32_t magic{kExternalBridgeConfigMagic};
    std::uint32_t version{kExternalBridgeConfigVersion};
    std::uint32_t seatId{0};
    std::uint32_t requiredApiMask{0};
    std::uint32_t reserved0{0};
    SessionToken token{};
    wchar_t pipeName[kExternalBridgePipeNameChars]{};
};
#pragma pack(pop)

inline std::wstring externalBridgeMappingName(std::uint32_t processId) {
    return L"Local\\HydraSeat.P3E.Bridge." + std::to_wstring(processId);
}
#endif

} // namespace hydra::gatec
