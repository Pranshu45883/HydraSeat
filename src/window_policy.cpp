#include "hydra/window_policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hydra::windowing {
namespace {

constexpr std::uint32_t kMaxInitialDelayMs = 10000u;
constexpr std::uint32_t kMaxRetryCount = 8u;
constexpr std::uint32_t kMaxRetryDelayMs = 2000u;
constexpr std::uint32_t kMaxTolerancePixels = 64u;

const display::SeatDisplayOutput* findOutput(
    const display::SeatDisplayGroup& group, const std::string& outputId) {
    const auto found = std::find_if(group.outputs.begin(), group.outputs.end(),
                                    [&](const display::SeatDisplayOutput& output) {
                                        return output.outputId == outputId;
                                    });
    return found == group.outputs.end() ? nullptr : &*found;
}

std::int32_t roundedCoordinate(double value) noexcept {
    const double lower = static_cast<double>(std::numeric_limits<std::int32_t>::min());
    const double upper = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    value = std::clamp(value, lower, upper);
    return static_cast<std::int32_t>(std::llround(value));
}

display::DisplayRect currentWindowRect(const TrackedWindow& window) noexcept {
    return {window.bounds.left, window.bounds.top, window.bounds.right, window.bounds.bottom};
}

display::DisplayRect fitWindowAtOutputOrigin(const TrackedWindow& window,
                                              const display::SeatDisplayOutput& output) noexcept {
    const auto current = currentWindowRect(window);
    const std::int32_t currentWidth = std::max<std::int32_t>(1, current.width());
    const std::int32_t currentHeight = std::max<std::int32_t>(1, current.height());
    const std::int32_t width = std::min(currentWidth, output.globalBounds.width());
    const std::int32_t height = std::min(currentHeight, output.globalBounds.height());
    return {output.globalBounds.left, output.globalBounds.top,
            output.globalBounds.left + width, output.globalBounds.top + height};
}

} // namespace

bool validateWindowPlacementPolicy(const WindowPlacementPolicy& policy,
                                   std::string* error) noexcept {
    if (policy.initialDelayMs > kMaxInitialDelayMs) {
        if (error) *error = "window placement initial delay exceeds 10 seconds";
        return false;
    }
    if (policy.retryCount > kMaxRetryCount) {
        if (error) *error = "window placement retry count exceeds bounded limit of 8";
        return false;
    }
    if (policy.retryDelayMs > kMaxRetryDelayMs) {
        if (error) *error = "window placement retry delay exceeds 2 seconds";
        return false;
    }
    if (policy.placementTolerancePixels > kMaxTolerancePixels) {
        if (error) *error = "window placement tolerance exceeds 64 pixels";
        return false;
    }
    if (policy.mode == WindowPlacementMode::BorderlessOnSelectedOutput &&
        policy.selectedOutputId.empty()) {
        if (error) *error = "borderless placement requires a selected stable output ID";
        return false;
    }
    if (policy.mode == WindowPlacementMode::RestoreLastSeatLocalRect &&
        !policy.lastSeatLocalRect) {
        if (error) *error = "restore-last placement requires a Seat-local rectangle";
        return false;
    }
    if (policy.lastSeatLocalRect &&
        (policy.lastSeatLocalRect->right <= policy.lastSeatLocalRect->left ||
         policy.lastSeatLocalRect->bottom <= policy.lastSeatLocalRect->top)) {
        if (error) *error = "restore-last Seat-local rectangle has no area";
        return false;
    }
    return true;
}

WindowPlacementPlan computeWindowPlacementPlan(
    const TrackedWindow& window,
    const display::SeatDisplayGroup& displayGroup,
    const WindowPlacementPolicy& policy) {
    WindowPlacementPlan plan;
    std::string error;
    if (!validateWindowPlacementPolicy(policy, &error)) {
        plan.diagnostics.push_back(error);
        return plan;
    }
    if (!window.identity.valid()) {
        plan.diagnostics.push_back("window identity is invalid or incomplete");
        return plan;
    }
    if (displayGroup.seatId == 0 || window.seatId != displayGroup.seatId) {
        plan.diagnostics.push_back("window Seat does not match target display group");
        return plan;
    }
    if (displayGroup.outputs.empty()) {
        plan.diagnostics.push_back("Seat display group has no resolved outputs");
        return plan;
    }
    if (policy.stableWindowRole && window.role != *policy.stableWindowRole) {
        plan.valid = true;
        plan.leaveNative = true;
        plan.diagnostics.push_back("window does not match stable-window role selector");
        return plan;
    }

    plan.valid = true;
    switch (policy.mode) {
        case WindowPlacementMode::LeaveNative:
            plan.leaveNative = true;
            return plan;
        case WindowPlacementMode::PlaceOnPrimaryOutput: {
            const auto* output = findOutput(displayGroup, displayGroup.primaryOutputId);
            if (output == nullptr) {
                plan.valid = false;
                plan.diagnostics.push_back("Seat primary output is unresolved");
                return plan;
            }
            plan.actionable = true;
            plan.targetOutputId = output->outputId;
            plan.desiredRect = fitWindowAtOutputOrigin(window, *output);
            return plan;
        }
        case WindowPlacementMode::SpanSeatGroup:
            plan.actionable = true;
            plan.desiredRect = displayGroup.globalBounds;
            return plan;
        case WindowPlacementMode::BorderlessOnSelectedOutput: {
            const auto* output = findOutput(displayGroup, policy.selectedOutputId);
            if (output == nullptr) {
                plan.valid = false;
                plan.diagnostics.push_back("selected borderless output is not part of Seat display group");
                return plan;
            }
            plan.actionable = true;
            plan.borderless = true;
            plan.targetOutputId = output->outputId;
            plan.desiredRect = output->globalBounds;
            return plan;
        }
        case WindowPlacementMode::RestoreLastSeatLocalRect: {
            display::CoordinateTransform transform(displayGroup);
            const auto& local = *policy.lastSeatLocalRect;
            const auto topLeft = transform.seatToGlobal({local.left, local.top});
            const auto bottomRight = transform.seatToGlobal({local.right, local.bottom});
            plan.actionable = true;
            plan.desiredRect = {
                roundedCoordinate(topLeft.x), roundedCoordinate(topLeft.y),
                roundedCoordinate(bottomRight.x), roundedCoordinate(bottomRight.y)};
            if (plan.desiredRect.width() <= 0 || plan.desiredRect.height() <= 0) {
                plan.valid = false;
                plan.actionable = false;
                plan.diagnostics.push_back("restored Seat-local rectangle overflows physical desktop coordinates");
            }
            return plan;
        }
        case WindowPlacementMode::ExclusiveFullscreen:
            switch (policy.exclusiveFullscreen) {
                case ExclusiveFullscreenPolicy::AllowNative:
                    plan.leaveNative = true;
                    plan.degraded = true;
                    plan.diagnostics.push_back(
                        "exclusive fullscreen is left native; HydraSeat does not claim deterministic control");
                    return plan;
                case ExclusiveFullscreenPolicy::Block:
                    plan.valid = false;
                    plan.diagnostics.push_back("exclusive fullscreen is blocked by profile policy");
                    return plan;
                case ExclusiveFullscreenPolicy::Unsupported:
                    plan.valid = false;
                    plan.degraded = true;
                    plan.diagnostics.push_back("exclusive fullscreen control is unsupported for this profile");
                    return plan;
            }
            break;
    }
    plan.valid = false;
    plan.diagnostics.push_back("unknown window placement policy mode");
    return plan;
}

} // namespace hydra::windowing
