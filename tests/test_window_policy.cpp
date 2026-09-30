#include "hydra/process_launcher.hpp"
#include "hydra/window_placement.hpp"
#include "hydra/window_policy.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace hydra;
using namespace hydra::display;
using namespace hydra::process;
using namespace hydra::windowing;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

TrackedWindow makeWindow(SeatId seatId = 1) {
    TrackedWindow window;
    window.identity.nativeHandle = 0x100u;
    window.identity.threadId = 7u;
    window.identity.trackerGeneration = 3u;
    window.identity.process.processId = 42u;
    window.identity.process.creationTime100ns = 99u;
    window.identity.process.executablePath = L"C:\\HydraSeatTest\\game.exe";
    window.seatId = seatId;
    window.role = WindowRole::PrimaryGame;
    window.visible = true;
    window.bounds = {100, 100, 900, 700};
    return window;
}

SeatDisplayOutput makeOutput(std::string id, DisplayRect bounds, std::uint32_t dpi = 96) {
    SeatDisplayOutput output;
    output.outputId = std::move(id);
    output.globalBounds = bounds;
    output.dpiX = dpi;
    output.dpiY = dpi;
    return output;
}

SeatDisplayGroup makeGroup() {
    SeatDisplayGroup group;
    group.seatId = 1;
    group.primaryOutputId = "display-left";
    group.primaryOriginX = -1920;
    group.primaryOriginY = 0;
    group.globalBounds = {-1920, 0, 2560, 1440};
    group.outputs = {
        makeOutput("display-left", {-1920, 0, 0, 1080}, 96),
        makeOutput("display-right", {0, 0, 2560, 1440}, 144),
    };
    return group;
}

bool sameRect(const WindowRect& window, const DisplayRect& display) noexcept {
    return window.left == display.left && window.top == display.top &&
           window.right == display.right && window.bottom == display.bottom;
}

#ifdef _WIN32

std::filesystem::path executableDirectory() {
    std::wstring buffer(32768u, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path fixtureExecutable() {
    return executableDirectory() / L"hydra_window_test_app.exe";
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

ProcessLaunchSpec fixtureSpec(SeatId seatId, std::wstring mode) {
    ProcessLaunchSpec spec;
    spec.seatId = seatId;
    spec.executablePath = fixtureExecutable().wstring();
    spec.workingDirectory = executableDirectory().wstring();
    spec.architecture = sizeof(void*) == 8u
        ? ProcessArchitecture::X64 : ProcessArchitecture::X86;
    spec.arguments = {L"--mode", std::move(mode)};
    return spec;
}

WindowProfileRules gameTargetRules() {
    WindowProfileRules rules;
    rules.defaultRole = WindowRole::Ignored;
    rules.visualTargetRole = WindowRole::PrimaryGame;
    WindowRule game;
    game.role = WindowRole::PrimaryGame;
    game.titleContains = L"Hydra Game";
    rules.overrides.push_back(std::move(game));
    return rules;
}

SeatDisplayGroup runtimeGroup(SeatId seatId, DisplayRect bounds) {
    SeatDisplayGroup group;
    group.seatId = seatId;
    group.primaryOutputId = "seat-output-" + std::to_string(seatId);
    group.primaryOriginX = bounds.left;
    group.primaryOriginY = bounds.top;
    group.globalBounds = bounds;
    group.outputs = {makeOutput(group.primaryOutputId, bounds, 96)};
    return group;
}

std::optional<WindowTargetSnapshot> waitVisualTarget(
    const WindowTracker& tracker, SeatId seatId, std::chrono::milliseconds timeout) {
    std::optional<WindowTargetSnapshot> target;
    if (!waitUntil([&] {
            target = tracker.target(seatId, WindowTargetKind::Visual);
            return target && target->status == WindowTargetStatus::Bound && target->window;
        }, timeout)) {
        return std::nullopt;
    }
    return target;
}

void stopFixtureGroup(std::unique_ptr<SeatProcessGroup>& group, std::string& error) {
    if (!group) return;
    ProcessStopPolicy cleanup;
    cleanup.gracefulTimeoutMs = 100;
    cleanup.forcedTimeoutMs = 2000;
    check(group->stop(cleanup, &error), "placement fixture process group stops cleanly");
}

#endif

void testPolicyValidationBounds() {
    WindowPlacementPolicy policy;
    std::string error;
    check(validateWindowPlacementPolicy(policy, &error), "default placement policy is valid");

    policy.retryCount = 9u;
    check(!validateWindowPlacementPolicy(policy, &error), "retry count is bounded");
    policy.retryCount = 2u;
    policy.initialDelayMs = 10001u;
    check(!validateWindowPlacementPolicy(policy, &error), "initial delay is bounded");
    policy.initialDelayMs = 0u;
    policy.retryDelayMs = 2001u;
    check(!validateWindowPlacementPolicy(policy, &error), "retry delay is bounded");
    policy.retryDelayMs = 50u;
    policy.placementTolerancePixels = 65u;
    check(!validateWindowPlacementPolicy(policy, &error), "placement tolerance is bounded");

    policy = {};
    policy.mode = WindowPlacementMode::BorderlessOnSelectedOutput;
    check(!validateWindowPlacementPolicy(policy, &error),
          "borderless policy requires selected stable output ID");

    policy = {};
    policy.mode = WindowPlacementMode::RestoreLastSeatLocalRect;
    check(!validateWindowPlacementPolicy(policy, &error),
          "restore-last policy requires a saved Seat-local rectangle");
    policy.lastSeatLocalRect = CoordinateRect{0.0, 0.0, 0.0, 20.0};
    check(!validateWindowPlacementPolicy(policy, &error),
          "restore-last policy rejects empty rectangles");
}

void testPlacementModes() {
    const auto window = makeWindow();
    const auto group = makeGroup();

    WindowPlacementPolicy policy;
    auto plan = computeWindowPlacementPlan(window, group, policy);
    check(plan.valid && plan.leaveNative && !plan.actionable,
          "leave-native produces a valid non-mutating plan");

    policy.mode = WindowPlacementMode::PlaceOnPrimaryOutput;
    plan = computeWindowPlacementPlan(window, group, policy);
    check(plan.valid && plan.actionable && plan.targetOutputId == "display-left" &&
              plan.desiredRect == DisplayRect{-1920, 0, -1120, 600},
          "primary-output placement preserves window size and uses Seat primary origin");

    policy.mode = WindowPlacementMode::SpanSeatGroup;
    plan = computeWindowPlacementPlan(window, group, policy);
    check(plan.valid && plan.actionable && plan.desiredRect == group.globalBounds,
          "span policy targets the complete Seat display union");

    policy.mode = WindowPlacementMode::BorderlessOnSelectedOutput;
    policy.selectedOutputId = "display-right";
    plan = computeWindowPlacementPlan(window, group, policy);
    check(plan.valid && plan.actionable && plan.borderless &&
              plan.desiredRect == DisplayRect{0, 0, 2560, 1440},
          "borderless policy targets only the selected Seat output");

    policy.selectedOutputId = "foreign-output";
    plan = computeWindowPlacementPlan(window, group, policy);
    check(!plan.valid && !plan.actionable,
          "borderless policy rejects outputs outside the Seat group");
}

void testRestoreRoleAndSeatIsolation() {
    const auto window = makeWindow();
    const auto group = makeGroup();

    WindowPlacementPolicy restore;
    restore.mode = WindowPlacementMode::RestoreLastSeatLocalRect;
    restore.lastSeatLocalRect = CoordinateRect{100.0, 40.0, 900.0, 640.0};
    auto plan = computeWindowPlacementPlan(window, group, restore);
    check(plan.valid && plan.actionable &&
              plan.desiredRect == DisplayRect{-1820, 40, -1020, 640},
          "Seat-local restore converts through the Seat primary origin");

    WindowPlacementPolicy rolePolicy;
    rolePolicy.mode = WindowPlacementMode::SpanSeatGroup;
    rolePolicy.stableWindowRole = WindowRole::Dialog;
    plan = computeWindowPlacementPlan(window, group, rolePolicy);
    check(plan.valid && plan.leaveNative && !plan.actionable,
          "stable-window selector leaves nonmatching roles untouched");

    auto otherSeat = makeWindow(2);
    plan = computeWindowPlacementPlan(otherSeat, group, rolePolicy);
    check(!plan.valid, "window from another Seat is never planned into this Seat display group");

    auto invalid = window;
    invalid.identity.trackerGeneration = 0;
    plan = computeWindowPlacementPlan(invalid, group, {});
    check(!plan.valid, "invalid/stale window identity is rejected before placement planning");
}

void testExclusiveFullscreenFailClosed() {
    const auto window = makeWindow();
    const auto group = makeGroup();

    WindowPlacementPolicy policy;
    policy.mode = WindowPlacementMode::ExclusiveFullscreen;
    policy.exclusiveFullscreen = ExclusiveFullscreenPolicy::Unsupported;
    auto plan = computeWindowPlacementPlan(window, group, policy);
    check(!plan.valid && plan.degraded,
          "unsupported exclusive fullscreen fails closed with degradation visible");

    policy.exclusiveFullscreen = ExclusiveFullscreenPolicy::Block;
    plan = computeWindowPlacementPlan(window, group, policy);
    check(!plan.valid && !plan.degraded,
          "explicitly blocked exclusive fullscreen is rejected");

    policy.exclusiveFullscreen = ExclusiveFullscreenPolicy::AllowNative;
    plan = computeWindowPlacementPlan(window, group, policy);
    check(plan.valid && plan.leaveNative && plan.degraded,
          "allowed native exclusive fullscreen remains an explicit degraded capability");
}

#ifdef _WIN32

void testReplacementMaintainsSeatPlacementIntent() {
    std::string error;
    auto launched = ProcessLauncher::launch(fixtureSpec(81u, L"recreate"), &error);
    check(launched.group != nullptr, "placement-recreate fixture launches");
    if (!launched.group) return;

    WindowTrackerOptions trackerOptions;
    trackerOptions.reacquisitionTimeoutMs = 2500u;
    WindowTracker tracker(trackerOptions);
    check(tracker.setProfileRules(gameTargetRules(), &error),
          "placement-recreate target rules are accepted");
    tracker.setProcessTrees({launched.group->snapshot()});
    check(tracker.start(&error), "placement-recreate tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto initial = waitVisualTarget(tracker, 81u, std::chrono::milliseconds(1200));
    check(initial && initial->window, "placement test obtains authoritative initial visual target");
    if (!initial || !initial->window) {
        tracker.stop();
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto group = runtimeGroup(81u, {360, 180, 1120, 660});
    WindowPlacementPolicy policy;
    policy.mode = WindowPlacementMode::SpanSeatGroup;
    policy.retryCount = 1u;
    policy.retryDelayMs = 20u;
    policy.placementTolerancePixels = 4u;
    WindowPlacementEngine engine(tracker);
    auto applied = engine.apply(*initial->window, group, policy);
    check(applied.status == WindowPlacementStatus::Applied && applied.reacquisitionArmed,
          "production placement arms an event-driven reacquisition lease");
    check(waitUntil([&] {
        const auto target = tracker.target(81u, WindowTargetKind::Visual);
        return target && target->window && sameRect(target->window->bounds, group.globalBounds);
    }, std::chrono::milliseconds(1200)),
          "initial visual target is placed inside the Seat display boundary");

    std::optional<WindowIdentity> replacementIdentity;
    check(waitUntil([&] {
        const auto target = tracker.target(81u, WindowTargetKind::Visual);
        if (!target || target->status != WindowTargetStatus::Bound || !target->window ||
            target->window->title != L"Hydra Game Replacement") {
            return false;
        }
        replacementIdentity = target->window->identity;
        return sameRect(target->window->bounds, group.globalBounds);
    }, std::chrono::milliseconds(4000)),
          "validated replacement HWND inherits the same Seat display placement intent");
    check(replacementIdentity &&
              replacementIdentity->process.sameInstance(launched.root) &&
              !replacementIdentity->sameInstance(initial->window->identity),
          "placement follows only the reacquired exact-process replacement identity");

    error.clear();
    check(engine.rollback(applied.restoreState, &error),
          "placement lease cleanup restores only the currently owned replacement when live");
    tracker.stop();
    stopFixtureGroup(launched.group, error);
}

void testSeatAReplacementDoesNotAlterSeatB() {
    std::string error;
    auto seatA = ProcessLauncher::launch(fixtureSpec(82u, L"recreate"), &error);
    auto seatB = ProcessLauncher::launch(fixtureSpec(83u, L"game"), &error);
    check(seatA.group && seatB.group, "two-Seat placement fixtures launch");
    if (!(seatA.group && seatB.group)) {
        stopFixtureGroup(seatA.group, error);
        stopFixtureGroup(seatB.group, error);
        return;
    }

    WindowTrackerOptions trackerOptions;
    trackerOptions.reacquisitionTimeoutMs = 2500u;
    WindowTracker tracker(trackerOptions);
    check(tracker.setProfileRules(gameTargetRules(), &error),
          "two-Seat placement target rules are accepted");
    tracker.setProcessTrees({seatA.group->snapshot(), seatB.group->snapshot()});
    check(tracker.start(&error), "two-Seat placement tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(seatA.group, error);
        stopFixtureGroup(seatB.group, error);
        return;
    }

    const auto initialA = waitVisualTarget(tracker, 82u, std::chrono::milliseconds(1200));
    const auto initialB = waitVisualTarget(tracker, 83u, std::chrono::milliseconds(1200));
    check(initialA && initialA->window && initialB && initialB->window,
          "both Seats expose independent authoritative visual targets");
    if (!(initialA && initialA->window && initialB && initialB->window)) {
        tracker.stop();
        stopFixtureGroup(seatA.group, error);
        stopFixtureGroup(seatB.group, error);
        return;
    }

    const auto seatBIdentity = initialB->window->identity;
    const auto seatBBounds = initialB->window->bounds;
    const auto groupA = runtimeGroup(82u, {420, 220, 1180, 700});
    WindowPlacementPolicy policy;
    policy.mode = WindowPlacementMode::SpanSeatGroup;
    policy.retryCount = 1u;
    policy.retryDelayMs = 20u;
    policy.placementTolerancePixels = 4u;
    WindowPlacementEngine engine(tracker);
    auto applied = engine.apply(*initialA->window, groupA, policy);
    check(applied.status == WindowPlacementStatus::Applied && applied.reacquisitionArmed,
          "Seat A placement lease arms independently");

    check(waitUntil([&] {
        const auto target = tracker.target(82u, WindowTargetKind::Visual);
        return target && target->status == WindowTargetStatus::Bound && target->window &&
               target->window->title == L"Hydra Game Replacement" &&
               sameRect(target->window->bounds, groupA.globalBounds);
    }, std::chrono::milliseconds(4000)),
          "Seat A replacement is reacquired and re-placed");

    const auto finalB = tracker.target(83u, WindowTargetKind::Visual);
    check(finalB && finalB->status == WindowTargetStatus::Bound && finalB->window &&
              finalB->window->identity.sameInstance(seatBIdentity) &&
              finalB->window->bounds == seatBBounds,
          "Seat A HWND replacement leaves Seat B identity and geometry untouched");
    check(tracker.validateIdentity(seatBIdentity),
          "Seat B remains a valid exact-owned target throughout Seat A reacquisition");

    error.clear();
    check(engine.rollback(applied.restoreState, &error),
          "Seat A cleanup does not require or mutate Seat B's window");
    const auto afterCleanupB = tracker.target(83u, WindowTargetKind::Visual);
    check(afterCleanupB && afterCleanupB->window &&
              afterCleanupB->window->identity.sameInstance(seatBIdentity) &&
              afterCleanupB->window->bounds == seatBBounds,
          "Seat A cleanup/recovery cannot operate on Seat B's HWND");

    tracker.stop();
    stopFixtureGroup(seatA.group, error);
    stopFixtureGroup(seatB.group, error);
}

#endif

} // namespace

int main() {
    testPolicyValidationBounds();
    testPlacementModes();
    testRestoreRoleAndSeatIsolation();
    testExclusiveFullscreenFailClosed();
#ifdef _WIN32
    testReplacementMaintainsSeatPlacementIntent();
    testSeatAReplacementDoesNotAlterSeatB();
#endif

    if (failures != 0) {
        std::cerr << failures << " window-policy test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "window policy tests passed\n";
    return EXIT_SUCCESS;
}
