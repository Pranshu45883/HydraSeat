#include "hydra/window_placement.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hydra::windowing {
namespace {

#ifdef _WIN32

std::string win32Error(const char* operation, DWORD code = GetLastError()) {
    return std::string(operation) + " failed (Win32=" + std::to_string(code) + ")";
}

display::DisplayRect queryOuterRect(HWND window) noexcept {
    RECT rect{};
    if (window == nullptr || GetWindowRect(window, &rect) == FALSE) return {};
    return {rect.left, rect.top, rect.right, rect.bottom};
}

bool rectNear(const display::DisplayRect& left, const display::DisplayRect& right,
              std::uint32_t tolerance) noexcept {
    const auto within = [tolerance](std::int32_t a, std::int32_t b) {
        const auto difference = static_cast<std::int64_t>(a) - static_cast<std::int64_t>(b);
        return std::llabs(difference) <= static_cast<std::int64_t>(tolerance);
    };
    return within(left.left, right.left) && within(left.top, right.top) &&
           within(left.right, right.right) && within(left.bottom, right.bottom);
}

bool queryWindowLong(HWND window, int index, std::int64_t& value,
                     std::string* error) noexcept {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR raw = GetWindowLongPtrW(window, index);
    const DWORD code = GetLastError();
    if (raw == 0 && code != ERROR_SUCCESS) {
        if (error) *error = win32Error("GetWindowLongPtrW", code);
        return false;
    }
    value = static_cast<std::int64_t>(raw);
    return true;
}

WindowRestoreState captureRestoreState(const WindowTracker& tracker,
                                       const WindowIdentity& identity,
                                       std::string* error) noexcept {
    WindowRestoreState state;
    state.identity = identity;
    if (!tracker.validateIdentity(identity)) {
        if (error) *error = "window identity became stale before restore-state capture";
        return state;
    }
    const auto window = reinterpret_cast<HWND>(identity.nativeHandle);
    state.outerRect = queryOuterRect(window);
    if (state.outerRect.width() <= 0 || state.outerRect.height() <= 0) {
        if (error) *error = "GetWindowRect returned an empty restore rectangle";
        return state;
    }
    if (!queryWindowLong(window, GWL_STYLE, state.style, error) ||
        !queryWindowLong(window, GWL_EXSTYLE, state.extendedStyle, error)) {
        return state;
    }
    state.wasVisible = IsWindowVisible(window) != FALSE;
    state.wasIconic = IsIconic(window) != FALSE;
    state.wasZoomed = IsZoomed(window) != FALSE;
    state.valid = true;
    return state;
}

bool setWindowLongChecked(HWND window, int index, std::int64_t value,
                          std::string* error) noexcept {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(window, index, static_cast<LONG_PTR>(value));
    const DWORD code = GetLastError();
    if (previous == 0 && code != ERROR_SUCCESS) {
        if (error) *error = win32Error("SetWindowLongPtrW", code);
        return false;
    }
    return true;
}

bool applyBorderlessStyle(HWND window, std::string* error) noexcept {
    std::int64_t styleValue = 0;
    if (!queryWindowLong(window, GWL_STYLE, styleValue, error)) return false;
    auto style = static_cast<LONG_PTR>(styleValue);
    style &= ~(static_cast<LONG_PTR>(WS_CAPTION) |
               static_cast<LONG_PTR>(WS_THICKFRAME) |
               static_cast<LONG_PTR>(WS_MINIMIZEBOX) |
               static_cast<LONG_PTR>(WS_MAXIMIZEBOX) |
               static_cast<LONG_PTR>(WS_SYSMENU));
    style |= static_cast<LONG_PTR>(WS_POPUP);
    return setWindowLongChecked(window, GWL_STYLE, static_cast<std::int64_t>(style), error);
}

bool setOuterRect(HWND window, const display::DisplayRect& rect,
                  bool frameChanged, std::string* error) noexcept {
    const std::int32_t width = rect.width();
    const std::int32_t height = rect.height();
    if (width <= 0 || height <= 0) {
        if (error) *error = "requested window placement rectangle has no area";
        return false;
    }
    UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER;
    if (frameChanged) flags |= SWP_FRAMECHANGED;
    if (SetWindowPos(window, nullptr, rect.left, rect.top, width, height, flags) == FALSE) {
        if (error) *error = win32Error("SetWindowPos");
        return false;
    }
    return true;
}

bool rollbackExactState(const WindowTracker& tracker,
                        const WindowRestoreState& state,
                        bool staleIsSafe,
                        std::string* error) noexcept {
    if (!state.valid) {
        if (error) *error = "window rollback state is invalid";
        return false;
    }
    if (!tracker.validateIdentity(state.identity)) {
        if (staleIsSafe) {
            if (error) error->clear();
            return true;
        }
        if (error) *error = "window rollback refused because identity is stale or unowned";
        return false;
    }
    const auto hwnd = reinterpret_cast<HWND>(state.identity.nativeHandle);
    std::string localError;
    if (!setWindowLongChecked(hwnd, GWL_STYLE, state.style, &localError) ||
        !setWindowLongChecked(hwnd, GWL_EXSTYLE, state.extendedStyle, &localError) ||
        !setOuterRect(hwnd, state.outerRect, true, &localError)) {
        if (error) *error = localError;
        return false;
    }

    int showCommand = SW_HIDE;
    if (state.wasIconic) {
        showCommand = SW_SHOWMINNOACTIVE;
    } else if (state.wasZoomed) {
        showCommand = SW_SHOWMAXIMIZED;
    } else if (state.wasVisible) {
        showCommand = SW_SHOWNOACTIVATE;
    }
    (void)ShowWindowAsync(hwnd, showCommand);
    if (!tracker.validateIdentity(state.identity)) {
        if (staleIsSafe) {
            if (error) error->clear();
            return true;
        }
        if (error) *error = "window identity became stale while rollback was completing";
        return false;
    }
    if (error) error->clear();
    return true;
}

#endif

WindowPlacementResult applyExactPlacement(
    const WindowTracker& tracker,
    const TrackedWindow& window,
    const display::SeatDisplayGroup& displayGroup,
    const WindowPlacementPolicy& policy) {
    WindowPlacementResult result;
    result.plan = computeWindowPlacementPlan(window, displayGroup, policy);
    result.diagnostics = result.plan.diagnostics;
    if (!result.plan.valid) {
        result.status = result.plan.degraded
            ? WindowPlacementStatus::Degraded : WindowPlacementStatus::Rejected;
        return result;
    }
    if (result.plan.leaveNative || !result.plan.actionable) {
        result.status = result.plan.degraded
            ? WindowPlacementStatus::Degraded : WindowPlacementStatus::NoChange;
        return result;
    }
    result.requestedRect = result.plan.desiredRect;

#ifdef _WIN32
    if (!tracker.validateIdentity(window.identity)) {
        result.status = WindowPlacementStatus::Rejected;
        result.diagnostics.push_back("window ownership/identity validation failed before placement");
        return result;
    }
    if (policy.initialDelayMs != 0u) {
        std::this_thread::sleep_for(std::chrono::milliseconds(policy.initialDelayMs));
        if (!tracker.validateIdentity(window.identity)) {
            result.status = WindowPlacementStatus::Rejected;
            result.diagnostics.push_back("window identity became stale during placement delay");
            return result;
        }
    }

    std::string error;
    result.restoreState = captureRestoreState(tracker, window.identity, &error);
    if (!result.restoreState.valid) {
        result.status = WindowPlacementStatus::Rejected;
        result.diagnostics.push_back(error.empty() ? "unable to capture rollback state" : error);
        return result;
    }

    const auto rollbackPartial = [&] {
        std::string rollbackError;
        if (!rollbackExactState(tracker, result.restoreState, true, &rollbackError) &&
            !rollbackError.empty()) {
            result.diagnostics.push_back("placement rollback failed: " + rollbackError);
        }
    };

    const auto hwnd = reinterpret_cast<HWND>(window.identity.nativeHandle);
    if (result.plan.borderless) {
        if (!tracker.validateIdentity(window.identity) ||
            !applyBorderlessStyle(hwnd, &error)) {
            result.status = WindowPlacementStatus::Rejected;
            result.diagnostics.push_back(error.empty()
                ? "window identity became stale before borderless style change" : error);
            rollbackPartial();
            return result;
        }
    }

    const std::uint32_t maxAttempts = policy.retryCount + 1u;
    for (std::uint32_t attempt = 1; attempt <= maxAttempts; ++attempt) {
        result.attempts = attempt;
        if (!tracker.validateIdentity(window.identity)) {
            result.status = WindowPlacementStatus::Rejected;
            result.diagnostics.push_back("window identity became stale during placement retry loop");
            rollbackPartial();
            return result;
        }
        if (!setOuterRect(hwnd, result.requestedRect,
                          result.plan.borderless && attempt == 1u, &error)) {
            result.status = WindowPlacementStatus::Rejected;
            result.diagnostics.push_back(error);
            rollbackPartial();
            return result;
        }

        // Give the target a bounded chance to process WM_WINDOWPOSCHANGED and
        // reveal whether it is actively fighting/reversing HydraSeat placement.
        const std::uint32_t settleDelay = std::max<std::uint32_t>(10u, policy.retryDelayMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(settleDelay));
        if (!tracker.validateIdentity(window.identity)) {
            result.status = WindowPlacementStatus::Rejected;
            result.diagnostics.push_back("window was recreated or destroyed after placement request");
            rollbackPartial();
            return result;
        }
        result.observedRect = queryOuterRect(hwnd);
        if (rectNear(result.requestedRect, result.observedRect,
                     policy.placementTolerancePixels)) {
            result.status = WindowPlacementStatus::Applied;
            return result;
        }
        if (attempt < maxAttempts && policy.retryDelayMs != 0u) {
            std::this_thread::sleep_for(std::chrono::milliseconds(policy.retryDelayMs));
        }
    }

    result.status = WindowPlacementStatus::Degraded;
    result.fightingApplication = true;
    result.diagnostics.push_back(
        "target did not converge to requested placement within bounded retries");
    return result;
#else
    (void)tracker;
    result.status = WindowPlacementStatus::Rejected;
    result.diagnostics.push_back("window placement is Windows-only");
    return result;
#endif
}

} // namespace

class WindowReacquisitionLease final
    : public WindowTargetObserver,
      public std::enable_shared_from_this<WindowReacquisitionLease> {
public:
    static std::shared_ptr<WindowReacquisitionLease> create(
        const WindowTracker& tracker,
        SeatId seatId,
        display::SeatDisplayGroup displayGroup,
        WindowPlacementPolicy policy,
        WindowRestoreState initialRestore,
        std::string& error) {
        if (seatId == 0 || !initialRestore.valid) {
            error = "reacquisition lease requires an exact initial restore state";
            return {};
        }
        initialRestore.reacquisitionLease.reset();
        auto lease = std::shared_ptr<WindowReacquisitionLease>(
            new WindowReacquisitionLease(tracker, seatId, std::move(displayGroup),
                                         std::move(policy), std::move(initialRestore)));
        lease->observerId_ = tracker.addTargetObserver(
            seatId, WindowTargetKind::Visual, lease);
        if (lease->observerId_ == 0) {
            error = "unable to register visual-target reacquisition observer";
            return {};
        }
        try {
            lease->worker_ = std::thread([raw = lease.get()] { raw->workerMain(); });
        } catch (const std::exception& exception) {
            tracker.removeTargetObserver(lease->observerId_);
            lease->observerId_ = 0;
            error = std::string("unable to start reacquisition worker: ") + exception.what();
            return {};
        } catch (...) {
            tracker.removeTargetObserver(lease->observerId_);
            lease->observerId_ = 0;
            error = "unable to start reacquisition worker";
            return {};
        }

        // Register first, then sample. A concurrent target transition either reaches
        // the observer or is present in this sample; the one-slot mailbox keeps the
        // latest authoritative generation without polling foreground state.
        if (const auto current = tracker.target(seatId, WindowTargetKind::Visual)) {
            lease->onWindowTargetChanged(*current);
        }
        error.clear();
        return lease;
    }

    ~WindowReacquisitionLease() override {
        std::string ignored;
        (void)stopAndRollback(&ignored);
    }

    void onWindowTargetChanged(const WindowTargetSnapshot& target) noexcept override {
        if (target.seatId != seatId_ || target.kind != WindowTargetKind::Visual) return;
        {
            std::lock_guard lock(mutex_);
            if (stopRequested_) return;
            if (!pending_ || target.bindingGeneration >= pending_->bindingGeneration) {
                pending_ = target;
            }
        }
        cv_.notify_one();
    }

    bool stopAndRollback(std::string* error) noexcept {
        if (observerId_ != 0) {
            tracker_.removeTargetObserver(observerId_);
            observerId_ = 0;
        }
        {
            std::lock_guard lock(mutex_);
            stopRequested_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();

        std::lock_guard lock(mutex_);
        if (rollbackCompleted_) {
            if (error) *error = rollbackError_;
            return rollbackError_.empty();
        }
        rollbackCompleted_ = true;
#ifdef _WIN32
        if (currentRestore_.valid) {
            std::string localError;
            if (!rollbackExactState(tracker_, currentRestore_, true, &localError)) {
                rollbackError_ = std::move(localError);
            }
        }
#else
        rollbackError_ = "window rollback is Windows-only";
#endif
        if (error) *error = rollbackError_;
        return rollbackError_.empty();
    }

private:
    WindowReacquisitionLease(const WindowTracker& tracker,
                             SeatId seatId,
                             display::SeatDisplayGroup displayGroup,
                             WindowPlacementPolicy policy,
                             WindowRestoreState initialRestore)
        : tracker_(tracker), seatId_(seatId), displayGroup_(std::move(displayGroup)),
          policy_(std::move(policy)), currentRestore_(std::move(initialRestore)),
          currentIdentity_(currentRestore_.identity) {
        policy_.followRecreatedWindow = false;
    }

    void failClosed(std::string diagnostic) noexcept {
        std::lock_guard lock(mutex_);
        terminalFailure_ = true;
        failureDiagnostic_ = std::move(diagnostic);
        currentIdentity_.reset();
    }

    bool handleBoundTarget(const WindowTargetSnapshot& target) {
        if (!target.window || target.window->seatId != seatId_) return true;
        if (!tracker_.validateIdentity(target.window->identity)) {
            // The callback itself raced a destroy/recreate. The tracker will publish
            // the authoritative Reacquiring/next-Bound transition; never guess here.
            return true;
        }

        {
            std::lock_guard lock(mutex_);
            if (terminalFailure_) return false;
            if (currentIdentity_ &&
                currentIdentity_->sameInstance(target.window->identity)) {
                return true;
            }
        }

#ifdef _WIN32
        WindowRestoreState previousRestore;
        {
            std::lock_guard lock(mutex_);
            previousRestore = currentRestore_;
        }
        if (previousRestore.valid) {
            std::string restoreError;
            if (!rollbackExactState(tracker_, previousRestore, true, &restoreError)) {
                failClosed(restoreError.empty()
                    ? "unable to restore previous visual target before reacquisition"
                    : std::move(restoreError));
                return false;
            }
        }
#endif

        auto placement = applyExactPlacement(tracker_, *target.window, displayGroup_, policy_);
        if (placement.status == WindowPlacementStatus::Rejected) {
            std::string diagnostic = "validated replacement window placement was rejected";
            if (!placement.diagnostics.empty()) diagnostic += ": " + placement.diagnostics.front();
            failClosed(std::move(diagnostic));
            return false;
        }

        std::lock_guard lock(mutex_);
        if (placement.restoreState.valid) {
            placement.restoreState.reacquisitionLease.reset();
            currentRestore_ = std::move(placement.restoreState);
        } else {
            currentRestore_ = {};
        }
        currentIdentity_ = target.window->identity;
        return true;
    }

    void workerMain() noexcept {
        for (;;) {
            WindowTargetSnapshot target;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stopRequested_ || pending_.has_value(); });
                if (stopRequested_) return;
                target = *pending_;
                pending_.reset();
                if (target.bindingGeneration < lastGeneration_) continue;
                lastGeneration_ = target.bindingGeneration;
                if (terminalFailure_) continue;
                if (target.status == WindowTargetStatus::Reacquiring ||
                    target.status == WindowTargetStatus::Unresolved) {
                    currentIdentity_.reset();
                    continue;
                }
                if (target.status == WindowTargetStatus::FailedClosed) {
                    terminalFailure_ = true;
                    failureDiagnostic_ =
                        "authoritative visual target exceeded bounded reacquisition deadline";
                    currentIdentity_.reset();
                    continue;
                }
            }
            if (target.status == WindowTargetStatus::Bound) {
                try {
                    (void)handleBoundTarget(target);
                } catch (const std::exception& exception) {
                    failClosed(std::string("reacquisition worker failed: ") + exception.what());
                } catch (...) {
                    failClosed("reacquisition worker failed unexpectedly");
                }
            }
        }
    }

    const WindowTracker& tracker_;
    SeatId seatId_{0};
    display::SeatDisplayGroup displayGroup_;
    WindowPlacementPolicy policy_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<WindowTargetSnapshot> pending_;
    std::thread worker_;
    std::uint64_t observerId_{0};
    std::uint64_t lastGeneration_{0};
    bool stopRequested_{false};
    bool terminalFailure_{false};
    bool rollbackCompleted_{false};
    std::string failureDiagnostic_;
    std::string rollbackError_;
    WindowRestoreState currentRestore_;
    std::optional<WindowIdentity> currentIdentity_;
};

WindowPlacementResult WindowPlacementEngine::apply(
    const TrackedWindow& window,
    const display::SeatDisplayGroup& displayGroup,
    const WindowPlacementPolicy& policy) const {
    const auto authoritative = tracker_.target(window.seatId, WindowTargetKind::Visual);
    if (!authoritative || authoritative->status != WindowTargetStatus::Bound ||
        !authoritative->window) {
        WindowPlacementResult result;
        result.plan = computeWindowPlacementPlan(window, displayGroup, policy);
        result.status = WindowPlacementStatus::Rejected;
        result.diagnostics = result.plan.diagnostics;
        result.diagnostics.push_back(
            authoritative && authoritative->status == WindowTargetStatus::FailedClosed
                ? "authoritative visual target is fail-closed after bounded reacquisition"
                : "authoritative visual target is not currently validated");
        return result;
    }
    if (!authoritative->window->identity.sameInstance(window.identity)) {
        WindowPlacementResult result;
        result.plan = computeWindowPlacementPlan(window, displayGroup, policy);
        result.status = WindowPlacementStatus::NoChange;
        result.diagnostics = result.plan.diagnostics;
        result.diagnostics.push_back(
            "non-authoritative owned window left untouched; placement follows visual target only");
        return result;
    }

    auto result = applyExactPlacement(tracker_, *authoritative->window, displayGroup, policy);
    if (!policy.followRecreatedWindow || !result.restoreState.valid ||
        (result.status != WindowPlacementStatus::Applied &&
         result.status != WindowPlacementStatus::Degraded)) {
        return result;
    }

    std::string error;
    auto lease = WindowReacquisitionLease::create(
        tracker_, window.seatId, displayGroup, policy, result.restoreState, error);
    if (!lease) {
#ifdef _WIN32
        std::string rollbackError;
        (void)rollbackExactState(tracker_, result.restoreState, true, &rollbackError);
        if (!rollbackError.empty()) {
            result.diagnostics.push_back("rollback after lease failure: " + rollbackError);
        }
#endif
        result.status = WindowPlacementStatus::Rejected;
        result.diagnostics.push_back(error.empty()
            ? "unable to arm replacement-window reacquisition" : std::move(error));
        return result;
    }
    result.restoreState.reacquisitionLease = std::move(lease);
    result.reacquisitionArmed = true;
    return result;
}

bool WindowPlacementEngine::rollback(const WindowRestoreState& state,
                                     std::string* error) const noexcept {
    if (!state.valid) {
        if (error) *error = "window rollback state is invalid";
        return false;
    }
    if (state.reacquisitionLease) {
        return state.reacquisitionLease->stopAndRollback(error);
    }
#ifdef _WIN32
    return rollbackExactState(tracker_, state, false, error);
#else
    if (error) *error = "window rollback is Windows-only";
    return false;
#endif
}

} // namespace hydra::windowing
