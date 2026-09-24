#pragma once

#include "hydra/seat_display_layout.hpp"
#include "hydra/window_tracker.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hydra::windowing {

enum class WindowPlacementMode : std::uint8_t {
    LeaveNative = 0,
    PlaceOnPrimaryOutput = 1,
    SpanSeatGroup = 2,
    BorderlessOnSelectedOutput = 3,
    RestoreLastSeatLocalRect = 4,
    ExclusiveFullscreen = 5,
};

enum class ExclusiveFullscreenPolicy : std::uint8_t {
    AllowNative = 0,
    Block = 1,
    Unsupported = 2,
};

enum class DpiCoordinateSpace : std::uint8_t {
    PhysicalDesktopPixels = 0,
};

struct WindowPlacementPolicy {
    WindowPlacementMode mode{WindowPlacementMode::LeaveNative};
    std::string selectedOutputId;
    std::optional<display::CoordinateRect> lastSeatLocalRect;
    std::optional<WindowRole> stableWindowRole;
    ExclusiveFullscreenPolicy exclusiveFullscreen{ExclusiveFullscreenPolicy::Unsupported};
    std::uint32_t initialDelayMs{0};
    std::uint32_t retryCount{2};
    std::uint32_t retryDelayMs{50};
    std::uint32_t placementTolerancePixels{2};
    // Production placement follows the Seat's authoritative visual target across
    // validated HWND recreation. The tracker owns the bounded reacquisition
    // deadline; disabling this is reserved for one-shot diagnostics/tests.
    bool followRecreatedWindow{true};
};

struct WindowPlacementPlan {
    bool valid{false};
    bool actionable{false};
    bool borderless{false};
    bool leaveNative{false};
    bool degraded{false};
    DpiCoordinateSpace coordinateSpace{DpiCoordinateSpace::PhysicalDesktopPixels};
    display::DisplayRect desiredRect;
    std::string targetOutputId;
    std::vector<std::string> diagnostics;
};

bool validateWindowPlacementPolicy(const WindowPlacementPolicy& policy,
                                   std::string* error = nullptr) noexcept;

WindowPlacementPlan computeWindowPlacementPlan(
    const TrackedWindow& window,
    const display::SeatDisplayGroup& displayGroup,
    const WindowPlacementPolicy& policy);

} // namespace hydra::windowing
