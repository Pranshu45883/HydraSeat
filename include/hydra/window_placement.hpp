#pragma once

#include "hydra/window_policy.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hydra::windowing {

class WindowReacquisitionLease;

enum class WindowPlacementStatus : std::uint8_t {
    NoChange = 0,
    Applied = 1,
    Degraded = 2,
    Rejected = 3,
};

struct WindowRestoreState {
    WindowIdentity identity;
    display::DisplayRect outerRect;
    std::int64_t style{0};
    std::int64_t extendedStyle{0};
    bool wasVisible{false};
    bool wasIconic{false};
    bool wasZoomed{false};
    bool valid{false};
    // Copies intentionally share one idempotent event-driven lease so existing
    // runtime containers retain replacement-window placement ownership.
    std::shared_ptr<WindowReacquisitionLease> reacquisitionLease;
};

struct WindowPlacementResult {
    WindowPlacementStatus status{WindowPlacementStatus::Rejected};
    WindowPlacementPlan plan;
    display::DisplayRect requestedRect;
    display::DisplayRect observedRect;
    std::uint32_t attempts{0};
    bool fightingApplication{false};
    bool reacquisitionArmed{false};
    WindowRestoreState restoreState;
    std::vector<std::string> diagnostics;
};

class WindowPlacementEngine {
public:
    explicit WindowPlacementEngine(const WindowTracker& tracker) : tracker_(tracker) {}

    WindowPlacementResult apply(const TrackedWindow& window,
                                const display::SeatDisplayGroup& displayGroup,
                                const WindowPlacementPolicy& policy) const;

    bool rollback(const WindowRestoreState& state,
                  std::string* error = nullptr) const noexcept;

private:
    const WindowTracker& tracker_;
};

} // namespace hydra::windowing
