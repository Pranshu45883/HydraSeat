#include "hydra/gate_c_cursor_focus_policy.hpp"

#include <algorithm>

namespace hydra::gatec {

bool validCursorClipRect(const CursorClipRect& rect) noexcept {
    return rect.right > rect.left && rect.bottom > rect.top;
}

CursorPoint clampCursorToClip(CursorPoint point,
                              const CursorClipRect& rect) noexcept {
    if (!validCursorClipRect(rect)) return point;
    point.x = std::clamp(
        point.x, rect.left, static_cast<std::int32_t>(rect.right - 1));
    point.y = std::clamp(
        point.y, rect.top, static_cast<std::int32_t>(rect.bottom - 1));
    return point;
}

} // namespace hydra::gatec
