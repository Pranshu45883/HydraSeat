#pragma once

#include "hydra/seat_display_layout.hpp"
#include "hydra/window_policy.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hydra::runtime_policy {

enum class RuntimePolicyEventKind : std::uint8_t {
    ProcessTreeChanged = 0,
    WindowChanged = 1,
    WindowRemoved = 2,
    DisplayLayoutChanged = 3,
    Reconcile = 4,
};

enum class RuntimePolicyHealth : std::uint8_t {
    Healthy = 0,
    Degraded = 1,
    RecoveryRequired = 2,
};

struct RuntimePolicyEvent {
    RuntimePolicyEvent() = default;
    RuntimePolicyEvent(std::uint64_t eventSequence, RuntimePolicyEventKind eventKind,
                       SeatId eventSeatId) noexcept
        : sequence(eventSequence), kind(eventKind), seatId(eventSeatId) {}

    std::uint64_t sequence{0};
    RuntimePolicyEventKind kind{RuntimePolicyEventKind::Reconcile};
    SeatId seatId{0};
    std::optional<windowing::TrackedWindow> window;
    std::optional<display::SeatDisplayGroup> displayGroup;
};

struct RuntimePolicyAction {
    std::uint64_t correlationId{0};
    SeatId seatId{0};
    windowing::WindowIdentity windowIdentity;
    windowing::WindowPlacementPlan placement;

    friend bool operator==(const RuntimePolicyAction&, const RuntimePolicyAction&) = default;
};

struct RuntimePolicyActionResult {
    bool succeeded{false};
    bool retryable{false};
    std::string diagnostic;
};

class RuntimePolicyExecutor {
public:
    virtual ~RuntimePolicyExecutor() = default;
    virtual RuntimePolicyActionResult execute(const RuntimePolicyAction& action) = 0;
};

struct SeatRuntimePolicySnapshot {
    SeatId seatId{0};
    RuntimePolicyHealth health{RuntimePolicyHealth::Healthy};
    std::uint64_t lastEventSequence{0};
    std::uint64_t lastActionCorrelation{0};
    std::uint32_t actionFailures{0};
    bool displayResolved{false};
    std::size_t trackedWindows{0};
    std::vector<std::string> diagnostics;
};

struct SeatRuntimePolicyOptions {
    std::uint32_t maxRetryCount{2};
};

class SeatRuntimePolicyCoordinator {
public:
    explicit SeatRuntimePolicyCoordinator(RuntimePolicyExecutor& executor,
                                          SeatRuntimePolicyOptions options = {});

    bool setWindowPolicy(SeatId seatId, windowing::WindowPlacementPolicy policy,
                         std::string* error = nullptr);
    bool consume(const RuntimePolicyEvent& event, std::string* error = nullptr);
    SeatRuntimePolicySnapshot snapshot(SeatId seatId) const;
    std::vector<SeatRuntimePolicySnapshot> snapshots() const;

private:
    struct SeatState {
        SeatId seatId{0};
        RuntimePolicyHealth health{RuntimePolicyHealth::Healthy};
        std::uint64_t lastEventSequence{0};
        std::uint64_t lastActionCorrelation{0};
        std::uint32_t actionFailures{0};
        std::optional<display::SeatDisplayGroup> displayGroup;
        windowing::WindowPlacementPolicy windowPolicy;
        bool policyConfigured{false};
        std::map<std::uintptr_t, windowing::TrackedWindow> windows;
        std::map<std::uintptr_t, windowing::WindowPlacementPlan> lastAppliedPlans;
        std::vector<std::string> diagnostics;
    };

    bool reconcile(SeatState& state, std::string* error);

    RuntimePolicyExecutor& executor_;
    SeatRuntimePolicyOptions options_;
    std::map<SeatId, SeatState> seats_;
    std::uint64_t nextCorrelationId_{1};
};

} // namespace hydra::runtime_policy
