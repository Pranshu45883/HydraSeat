#include "hydra/seat_runtime_policy.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace hydra::runtime_policy {

namespace {

constexpr std::uint32_t kMaxPolicyRetries = 8u;
constexpr std::size_t kMaxDiagnostics = 32u;

template <typename State>
void addDiagnostic(State& state, std::string value) {
    if (value.empty()) return;
    if (state.diagnostics.size() == kMaxDiagnostics) {
        state.diagnostics.erase(state.diagnostics.begin());
    }
    state.diagnostics.push_back(std::move(value));
}

bool samePlan(const windowing::WindowPlacementPlan& left,
              const windowing::WindowPlacementPlan& right) noexcept {
    return left.valid == right.valid &&
           left.actionable == right.actionable &&
           left.borderless == right.borderless &&
           left.leaveNative == right.leaveNative &&
           left.degraded == right.degraded &&
           left.coordinateSpace == right.coordinateSpace &&
           left.desiredRect == right.desiredRect &&
           left.targetOutputId == right.targetOutputId;
}

} // namespace

SeatRuntimePolicyCoordinator::SeatRuntimePolicyCoordinator(
    RuntimePolicyExecutor& executor, SeatRuntimePolicyOptions options)
    : executor_(executor), options_(options) {
    if (options_.maxRetryCount > kMaxPolicyRetries) {
        options_.maxRetryCount = kMaxPolicyRetries;
    }
}

bool SeatRuntimePolicyCoordinator::setWindowPolicy(
    SeatId seatId, windowing::WindowPlacementPolicy policy, std::string* error) {
    if (seatId == 0) {
        if (error) *error = "runtime policy requires a nonzero Seat ID";
        return false;
    }
    std::string validationError;
    if (!windowing::validateWindowPlacementPolicy(policy, &validationError)) {
        if (error) *error = validationError;
        return false;
    }
    auto& state = seats_[seatId];
    state.seatId = seatId;
    state.windowPolicy = std::move(policy);
    state.policyConfigured = true;
    return true;
}

bool SeatRuntimePolicyCoordinator::consume(const RuntimePolicyEvent& event,
                                           std::string* error) {
    if (event.seatId == 0 || event.sequence == 0) {
        if (error) *error = "runtime policy event requires nonzero Seat and sequence";
        return false;
    }
    auto& state = seats_[event.seatId];
    state.seatId = event.seatId;
    if (event.sequence <= state.lastEventSequence) {
        return true;
    }

    switch (event.kind) {
        case RuntimePolicyEventKind::WindowChanged:
            if (!event.window || event.window->seatId != event.seatId ||
                !event.window->identity.valid()) {
                if (error) *error = "window-change event has invalid or cross-Seat identity";
                state.health = RuntimePolicyHealth::RecoveryRequired;
                addDiagnostic(state, error ? *error : "invalid window-change event");
                return false;
            }
            if (const auto existing = state.windows.find(
                    event.window->identity.nativeHandle);
                existing != state.windows.end() &&
                !existing->second.identity.sameInstance(event.window->identity)) {
                if (event.window->identity.trackerGeneration <
                    existing->second.identity.trackerGeneration) {
                    addDiagnostic(state,
                                  "stale window-change identity ignored after HWND reuse");
                    state.lastEventSequence = event.sequence;
                    return true;
                }
                if (event.window->identity.trackerGeneration ==
                    existing->second.identity.trackerGeneration) {
                    if (error) {
                        *error = "window-change identity conflicts within one tracker generation";
                    }
                    state.health = RuntimePolicyHealth::RecoveryRequired;
                    addDiagnostic(state, error ? *error
                                               : "conflicting window-change identity");
                    return false;
                }
                // A newer tracker generation is authoritative evidence that the
                // native handle was reused for a different exact window. Never
                // carry a placement cache across the identity boundary.
                state.lastAppliedPlans.erase(event.window->identity.nativeHandle);
            }
            state.windows[event.window->identity.nativeHandle] = *event.window;
            break;
        case RuntimePolicyEventKind::WindowRemoved:
            if (!event.window || event.window->seatId != event.seatId ||
                !event.window->identity.valid()) {
                if (error) *error = "window-remove event has invalid or cross-Seat identity";
                state.health = RuntimePolicyHealth::RecoveryRequired;
                addDiagnostic(state, error ? *error : "invalid window-remove event");
                return false;
            }
            if (const auto existing = state.windows.find(
                    event.window->identity.nativeHandle);
                existing != state.windows.end()) {
                if (!existing->second.identity.sameInstance(event.window->identity)) {
                    if (event.window->identity.trackerGeneration <
                        existing->second.identity.trackerGeneration) {
                        addDiagnostic(state,
                                      "stale window-remove identity ignored after HWND reuse");
                        state.lastEventSequence = event.sequence;
                        return true;
                    }
                    if (error) {
                        *error = "window-remove identity conflicts with current HWND ownership";
                    }
                    state.health = RuntimePolicyHealth::RecoveryRequired;
                    addDiagnostic(state, error ? *error
                                               : "conflicting window-remove identity");
                    return false;
                }
                state.windows.erase(existing);
                state.lastAppliedPlans.erase(event.window->identity.nativeHandle);
            }
            break;
        case RuntimePolicyEventKind::DisplayLayoutChanged:
            if (!event.displayGroup || event.displayGroup->seatId != event.seatId) {
                if (error) *error = "display-layout event has invalid or cross-Seat group";
                state.health = RuntimePolicyHealth::RecoveryRequired;
                addDiagnostic(state, error ? *error : "invalid display-layout event");
                return false;
            }
            state.displayGroup = *event.displayGroup;
            break;
        case RuntimePolicyEventKind::ProcessTreeChanged:
        case RuntimePolicyEventKind::Reconcile:
            break;
    }
    // Invalid payloads above must not be able to poison the sequence watermark
    // and make a later valid event look stale. Once the event has been accepted
    // into Seat state, consume its sequence even if reconciliation later fails.
    state.lastEventSequence = event.sequence;
    return reconcile(state, error);
}

bool SeatRuntimePolicyCoordinator::reconcile(SeatState& state, std::string* error) {
    if (!state.displayGroup) {
        state.health = RuntimePolicyHealth::Degraded;
        addDiagnostic(state, "Seat display group is unresolved");
        return true;
    }
    if (state.displayGroup->outputs.empty() || state.displayGroup->degraded) {
        state.health = RuntimePolicyHealth::Degraded;
        addDiagnostic(state, "Seat display group is degraded");
    } else if (state.health != RuntimePolicyHealth::RecoveryRequired) {
        state.health = RuntimePolicyHealth::Healthy;
    }
    if (!state.policyConfigured) return true;

    for (const auto& [handle, window] : state.windows) {
        const auto plan = windowing::computeWindowPlacementPlan(
            window, *state.displayGroup, state.windowPolicy);
        if (!plan.valid) {
            state.health = RuntimePolicyHealth::RecoveryRequired;
            addDiagnostic(state, plan.diagnostics.empty()
                                     ? "window placement plan rejected"
                                     : plan.diagnostics.front());
            if (error) *error = state.diagnostics.back();
            return false;
        }
        if (plan.degraded && state.health == RuntimePolicyHealth::Healthy) {
            state.health = RuntimePolicyHealth::Degraded;
        }
        if (!plan.actionable || plan.leaveNative) continue;

        const auto previous = state.lastAppliedPlans.find(handle);
        if (previous != state.lastAppliedPlans.end() && samePlan(previous->second, plan)) {
            continue;
        }

        RuntimePolicyAction action;
        action.correlationId = nextCorrelationId_++;
        action.seatId = state.seatId;
        action.windowIdentity = window.identity;
        action.placement = plan;
        state.lastActionCorrelation = action.correlationId;

        RuntimePolicyActionResult result;
        for (std::uint32_t attempt = 0; attempt <= options_.maxRetryCount; ++attempt) {
            result = executor_.execute(action);
            if (result.succeeded) break;
            if (!result.retryable) break;
        }
        if (!result.succeeded) {
            if (state.actionFailures != std::numeric_limits<std::uint32_t>::max()) {
                ++state.actionFailures;
            }
            state.health = result.retryable
                ? RuntimePolicyHealth::Degraded
                : RuntimePolicyHealth::RecoveryRequired;
            addDiagnostic(state, result.diagnostic.empty()
                                     ? "runtime policy action failed"
                                     : result.diagnostic);
            if (error) *error = state.diagnostics.back();
            return result.retryable;
        }
        state.lastAppliedPlans[handle] = plan;
    }
    return true;
}

SeatRuntimePolicySnapshot SeatRuntimePolicyCoordinator::snapshot(SeatId seatId) const {
    SeatRuntimePolicySnapshot result;
    result.seatId = seatId;
    const auto found = seats_.find(seatId);
    if (found == seats_.end()) return result;
    const auto& state = found->second;
    result.health = state.health;
    result.lastEventSequence = state.lastEventSequence;
    result.lastActionCorrelation = state.lastActionCorrelation;
    result.actionFailures = state.actionFailures;
    result.displayResolved = state.displayGroup.has_value();
    result.trackedWindows = state.windows.size();
    result.diagnostics = state.diagnostics;
    return result;
}

std::vector<SeatRuntimePolicySnapshot> SeatRuntimePolicyCoordinator::snapshots() const {
    std::vector<SeatRuntimePolicySnapshot> result;
    result.reserve(seats_.size());
    for (const auto& [seatId, state] : seats_) {
        (void)state;
        result.push_back(snapshot(seatId));
    }
    return result;
}

} // namespace hydra::runtime_policy
