#include "hydra/seat_runtime_policy.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::display;
using namespace hydra::runtime_policy;
using namespace hydra::windowing;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class FakeExecutor final : public RuntimePolicyExecutor {
public:
    RuntimePolicyActionResult execute(const RuntimePolicyAction& action) override {
        actions.push_back(action);
        if (failuresBeforeSuccess != 0u) {
            --failuresBeforeSuccess;
            return {false, retryableFailure, "injected action failure"};
        }
        if (permanentFailure) return {false, false, "permanent action failure"};
        return {true, false, {}};
    }

    std::vector<RuntimePolicyAction> actions;
    std::uint32_t failuresBeforeSuccess{0};
    bool retryableFailure{true};
    bool permanentFailure{false};
};

TrackedWindow makeWindow(SeatId seatId, std::uintptr_t handle = 0x100u) {
    TrackedWindow window;
    window.identity.nativeHandle = handle;
    window.identity.threadId = 11u;
    window.identity.trackerGeneration = 4u;
    window.identity.process.processId = static_cast<std::uint32_t>(100u + seatId);
    window.identity.process.creationTime100ns = 500u + seatId;
    window.identity.process.executablePath = L"C:\\HydraSeatTest\\game.exe";
    window.seatId = seatId;
    window.role = WindowRole::PrimaryGame;
    window.visible = true;
    window.bounds = {100, 100, 900, 700};
    return window;
}

SeatDisplayGroup makeGroup(SeatId seatId, bool degraded = false) {
    SeatDisplayOutput output;
    output.outputId = "seat-output-" + std::to_string(seatId);
    output.globalBounds = {0, 0, 1920, 1080};
    SeatDisplayGroup group;
    group.seatId = seatId;
    group.outputs.push_back(output);
    group.primaryOutputId = output.outputId;
    group.globalBounds = output.globalBounds;
    group.degraded = degraded;
    return group;
}

WindowPlacementPolicy primaryPolicy() {
    WindowPlacementPolicy policy;
    policy.mode = WindowPlacementMode::PlaceOnPrimaryOutput;
    return policy;
}

void testDeterministicIdempotentReplay() {
    FakeExecutor executor;
    SeatRuntimePolicyCoordinator coordinator(executor);
    std::string error;
    check(coordinator.setWindowPolicy(1, primaryPolicy(), &error), "Seat policy is accepted");

    RuntimePolicyEvent displayEvent{1, RuntimePolicyEventKind::DisplayLayoutChanged, 1};
    displayEvent.displayGroup = makeGroup(1);
    check(coordinator.consume(displayEvent, &error), "display event reconciles");

    RuntimePolicyEvent windowEvent{2, RuntimePolicyEventKind::WindowChanged, 1};
    windowEvent.window = makeWindow(1);
    check(coordinator.consume(windowEvent, &error), "window event applies desired placement");
    check(executor.actions.size() == 1u, "first desired placement emits exactly one action");

    RuntimePolicyEvent duplicate = windowEvent;
    check(coordinator.consume(duplicate, &error), "duplicate sequence is accepted idempotently");
    RuntimePolicyEvent reconcile{3, RuntimePolicyEventKind::Reconcile, 1};
    check(coordinator.consume(reconcile, &error), "explicit reconcile succeeds");
    check(executor.actions.size() == 1u,
          "duplicate/reconcile with unchanged desired state does not duplicate mutation");

    const auto snapshot = coordinator.snapshot(1);
    check(snapshot.health == RuntimePolicyHealth::Healthy && snapshot.trackedWindows == 1u,
          "healthy Seat snapshot reflects tracked window");
}

void testDisplayDegradationAndRecovery() {
    FakeExecutor executor;
    SeatRuntimePolicyCoordinator coordinator(executor);
    std::string error;
    check(coordinator.setWindowPolicy(2, primaryPolicy(), &error), "Seat 2 policy is accepted");

    RuntimePolicyEvent windowEvent{1, RuntimePolicyEventKind::WindowChanged, 2};
    windowEvent.window = makeWindow(2, 0x200u);
    check(coordinator.consume(windowEvent, &error), "window may arrive before display layout");
    check(coordinator.snapshot(2).health == RuntimePolicyHealth::Degraded,
          "missing display layout degrades rather than inventing a fallback");
    check(executor.actions.empty(), "no placement action is emitted without a display group");

    RuntimePolicyEvent degraded{2, RuntimePolicyEventKind::DisplayLayoutChanged, 2};
    degraded.displayGroup = makeGroup(2, true);
    check(coordinator.consume(degraded, &error), "degraded display layout is consumable");
    check(coordinator.snapshot(2).health == RuntimePolicyHealth::Degraded,
          "degraded display remains visible in policy health");

    RuntimePolicyEvent healthy{3, RuntimePolicyEventKind::DisplayLayoutChanged, 2};
    healthy.displayGroup = makeGroup(2, false);
    check(coordinator.consume(healthy, &error), "healthy display return reconciles");
    check(coordinator.snapshot(2).health == RuntimePolicyHealth::Healthy,
          "healthy display return clears nonfatal degradation");
}

void testBoundedRetryAndFailureVisibility() {
    FakeExecutor executor;
    executor.failuresBeforeSuccess = 2u;
    SeatRuntimePolicyOptions options;
    options.maxRetryCount = 2u;
    SeatRuntimePolicyCoordinator coordinator(executor, options);
    std::string error;
    check(coordinator.setWindowPolicy(3, primaryPolicy(), &error), "retry Seat policy is accepted");

    RuntimePolicyEvent displayEvent{1, RuntimePolicyEventKind::DisplayLayoutChanged, 3};
    displayEvent.displayGroup = makeGroup(3);
    check(coordinator.consume(displayEvent, &error), "retry Seat display resolves");
    RuntimePolicyEvent windowEvent{2, RuntimePolicyEventKind::WindowChanged, 3};
    windowEvent.window = makeWindow(3, 0x300u);
    check(coordinator.consume(windowEvent, &error), "retryable action eventually succeeds");
    check(executor.actions.size() == 3u,
          "retryable failure is retried only within declared bounded count");

    FakeExecutor permanent;
    permanent.permanentFailure = true;
    SeatRuntimePolicyCoordinator failed(permanent);
    check(failed.setWindowPolicy(4, primaryPolicy(), &error), "failure Seat policy is accepted");
    RuntimePolicyEvent d{1, RuntimePolicyEventKind::DisplayLayoutChanged, 4};
    d.displayGroup = makeGroup(4);
    check(failed.consume(d, &error), "failure Seat display resolves");
    RuntimePolicyEvent w{2, RuntimePolicyEventKind::WindowChanged, 4};
    w.window = makeWindow(4, 0x400u);
    check(!failed.consume(w, &error), "nonretryable mutation failure fails reconciliation");
    const auto snapshot = failed.snapshot(4);
    check(snapshot.health == RuntimePolicyHealth::RecoveryRequired &&
              snapshot.actionFailures == 1u && !snapshot.diagnostics.empty(),
          "nonretryable action failure is visible as RecoveryRequired");
}

void testCrossSeatAndRemovalSafety() {
    FakeExecutor executor;
    SeatRuntimePolicyCoordinator coordinator(executor);
    std::string error;
    check(coordinator.setWindowPolicy(5, primaryPolicy(), &error), "Seat 5 policy is accepted");

    RuntimePolicyEvent cross{1, RuntimePolicyEventKind::WindowChanged, 5};
    cross.window = makeWindow(6, 0x500u);
    check(!coordinator.consume(cross, &error), "cross-Seat window event is rejected");
    check(executor.actions.empty(), "cross-Seat window can never cause a mutation");

    FakeExecutor cleanExecutor;
    SeatRuntimePolicyCoordinator clean(cleanExecutor);
    check(clean.setWindowPolicy(7, primaryPolicy(), &error), "Seat 7 policy is accepted");
    RuntimePolicyEvent d{1, RuntimePolicyEventKind::DisplayLayoutChanged, 7};
    d.displayGroup = makeGroup(7);
    check(clean.consume(d, &error), "Seat 7 display resolves");
    auto tracked = makeWindow(7, 0x700u);
    RuntimePolicyEvent w{2, RuntimePolicyEventKind::WindowChanged, 7};
    w.window = tracked;
    check(clean.consume(w, &error), "Seat 7 window is tracked");
    RuntimePolicyEvent removed{3, RuntimePolicyEventKind::WindowRemoved, 7};
    removed.window = tracked;
    check(clean.consume(removed, &error), "window removal is idempotently reconciled");
    check(clean.snapshot(7).trackedWindows == 0u, "removed window leaves no owned policy state");
}

void testStaleWindowIdentityAndInvalidSequenceDoNotPoisonState() {
    FakeExecutor executor;
    SeatRuntimePolicyCoordinator coordinator(executor);
    std::string error;
    check(coordinator.setWindowPolicy(8, primaryPolicy(), &error),
          "stale-window Seat policy is accepted");

    RuntimePolicyEvent display{1, RuntimePolicyEventKind::DisplayLayoutChanged, 8};
    display.displayGroup = makeGroup(8);
    check(coordinator.consume(display, &error), "stale-window Seat display resolves");

    auto first = makeWindow(8, 0x800u);
    first.identity.trackerGeneration = 10u;
    RuntimePolicyEvent firstChanged{2, RuntimePolicyEventKind::WindowChanged, 8};
    firstChanged.window = first;
    check(coordinator.consume(firstChanged, &error), "first exact HWND instance is tracked");

    auto replacement = first;
    replacement.identity.trackerGeneration = 11u;
    replacement.identity.threadId = 12u;
    replacement.identity.process.processId += 1000u;
    replacement.identity.process.creationTime100ns += 1000u;
    RuntimePolicyEvent replacementChanged{3, RuntimePolicyEventKind::WindowChanged, 8};
    replacementChanged.window = replacement;
    check(coordinator.consume(replacementChanged, &error),
          "newer exact HWND generation replaces stale placement identity");
    const auto actionsAfterReplacement = executor.actions.size();

    RuntimePolicyEvent staleRemove{4, RuntimePolicyEventKind::WindowRemoved, 8};
    staleRemove.window = first;
    check(coordinator.consume(staleRemove, &error),
          "late remove for the old HWND generation is ignored safely");
    check(coordinator.snapshot(8).trackedWindows == 1u,
          "stale remove cannot erase the replacement HWND instance");

    RuntimePolicyEvent staleChange{5, RuntimePolicyEventKind::WindowChanged, 8};
    staleChange.window = first;
    check(coordinator.consume(staleChange, &error),
          "late change for the old HWND generation is ignored safely");
    check(executor.actions.size() == actionsAfterReplacement,
          "stale HWND change cannot replay placement for an old process identity");

    RuntimePolicyEvent malformed{1000, RuntimePolicyEventKind::WindowChanged, 8};
    malformed.window = makeWindow(9, 0x900u);
    check(!coordinator.consume(malformed, &error),
          "malformed high-sequence cross-Seat event is rejected");
    check(coordinator.snapshot(8).lastEventSequence == 5u,
          "rejected payload cannot poison the accepted event-sequence watermark");

    RuntimePolicyEvent validAfterMalformed{6, RuntimePolicyEventKind::Reconcile, 8};
    check(coordinator.consume(validAfterMalformed, &error) &&
              coordinator.snapshot(8).lastEventSequence == 6u,
          "valid event after rejected high sequence is not misclassified as stale");

    FakeExecutor conflictExecutor;
    SeatRuntimePolicyCoordinator conflict(conflictExecutor);
    check(conflict.setWindowPolicy(10, primaryPolicy(), &error),
          "same-generation conflict fixture accepts policy");
    RuntimePolicyEvent conflictDisplay{1, RuntimePolicyEventKind::DisplayLayoutChanged, 10};
    conflictDisplay.displayGroup = makeGroup(10);
    check(conflict.consume(conflictDisplay, &error),
          "same-generation conflict fixture resolves display");
    auto owned = makeWindow(10, 0xA00u);
    owned.identity.trackerGeneration = 20u;
    RuntimePolicyEvent ownedChanged{2, RuntimePolicyEventKind::WindowChanged, 10};
    ownedChanged.window = owned;
    check(conflict.consume(ownedChanged, &error),
          "same-generation conflict fixture tracks exact owner");
    auto impossible = owned;
    impossible.identity.process.processId += 2000u;
    impossible.identity.process.creationTime100ns += 2000u;
    RuntimePolicyEvent conflictingChanged{3, RuntimePolicyEventKind::WindowChanged, 10};
    conflictingChanged.window = impossible;
    check(!conflict.consume(conflictingChanged, &error) &&
              conflict.snapshot(10).health == RuntimePolicyHealth::RecoveryRequired &&
              conflict.snapshot(10).trackedWindows == 1u,
          "different exact owner at the same tracker generation fails closed without replacing ownership");
}

} // namespace

int main() {
    testDeterministicIdempotentReplay();
    testDisplayDegradationAndRecovery();
    testBoundedRetryAndFailureVisibility();
    testCrossSeatAndRemovalSafety();
    testStaleWindowIdentityAndInvalidSequenceDoNotPoisonState();

    if (failures != 0) {
        std::cerr << failures << " runtime-policy test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Seat runtime policy tests passed\n";
    return EXIT_SUCCESS;
}
