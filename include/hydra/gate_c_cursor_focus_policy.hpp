#pragma once

#include <cstdint>
#include <limits>

namespace hydra::gatec {

// P3-API-03 v1 uses caller-provided 32-bit logical screen coordinates.
// No physical-pixel, client-coordinate, or per-monitor DPI conversion is
// inferred by the controlled shim.
struct CursorPoint {
    std::int32_t x{0};
    std::int32_t y{0};

    bool operator==(const CursorPoint&) const = default;
};

struct CursorClipRect {
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t right{0};
    std::int32_t bottom{0};

    bool operator==(const CursorClipRect&) const = default;
};

inline constexpr CursorClipRect kUnclippedLogicalCoordinateDomain{
    (std::numeric_limits<std::int32_t>::min)(),
    (std::numeric_limits<std::int32_t>::min)(),
    (std::numeric_limits<std::int32_t>::max)(),
    (std::numeric_limits<std::int32_t>::max)()};

bool validCursorClipRect(const CursorClipRect& rect) noexcept;
CursorPoint clampCursorToClip(CursorPoint point,
                              const CursorClipRect& rect) noexcept;

} // namespace hydra::gatec
