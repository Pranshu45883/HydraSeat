#include "hydra/process_launcher.hpp"
#include "hydra/window_tracker.hpp"

#include <algorithm>
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

using namespace hydra::process;
using namespace hydra::windowing;

int failures = 0;

[[maybe_unused]] void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
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

std::optional<TrackedWindow> findTitle(const WindowTrackerSnapshot& snapshot,
                                       std::wstring_view title) {
    const auto found = std::find_if(snapshot.windows.begin(), snapshot.windows.end(),
                                    [title](const TrackedWindow& window) {
                                        return window.title == title;
                                    });
    if (found == snapshot.windows.end()) return std::nullopt;
    return *found;
}

struct FindWindowContext {
    std::uint32_t processId{0};
    std::wstring title;
    HWND result{nullptr};
};

BOOL CALLBACK findFixtureWindowCallback(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<FindWindowContext*>(parameter);
    DWORD processId = 0;
    (void)GetWindowThreadProcessId(window, &processId);
    if (processId != context->processId) return TRUE;
    std::wstring title(256u, L'\0');
    const int copied = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    if (copied <= 0) return TRUE;
    title.resize(static_cast<std::size_t>(copied));
    if (title == context->title) {
        context->result = window;
        return FALSE;
    }
    return TRUE;
}

HWND findFixtureWindow(std::uint32_t processId, std::wstring title) {
    FindWindowContext context{processId, std::move(title), nullptr};
    (void)EnumWindows(findFixtureWindowCallback, reinterpret_cast<LPARAM>(&context));
    return context.result;
}

ProcessLaunchSpec fixtureSpec(hydra::SeatId seatId, std::wstring mode) {
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

WindowProfileRules visualOnlyTargetRules() {
    WindowProfileRules rules;
    rules.defaultRole = WindowRole::Ignored;
    rules.visualTargetRole = WindowRole::PrimaryGame;
    WindowRule visual;
    visual.role = WindowRole::PrimaryGame;
    visual.titleContains = L"Hydra Visual";
    rules.overrides.push_back(std::move(visual));
    return rules;
}

WindowProfileRules secondaryInputTargetRules(bool includeSplash = false) {
    auto rules = visualOnlyTargetRules();
    rules.inputTargetRole = WindowRole::InputTarget;
    WindowRule input;
    input.role = WindowRole::InputTarget;
    input.titleContains = L"Hydra Input Sink";
    rules.overrides.push_back(std::move(input));
    if (includeSplash) {
        WindowRule splash;
        splash.role = WindowRole::Launcher;
        splash.titleContains = L"Hydra Splash";
        rules.overrides.push_back(std::move(splash));
    }
    return rules;
}

std::optional<WindowTargetSnapshot> waitTarget(
    const WindowTracker& tracker, hydra::SeatId seatId, WindowTargetKind kind,
    WindowTargetStatus status, std::chrono::milliseconds timeout) {
    std::optional<WindowTargetSnapshot> result;
    const bool reached = waitUntil([&] {
        result = tracker.target(seatId, kind);
        return result && result->status == status;
    }, timeout);
    return reached ? result : std::nullopt;
}

void stopFixtureGroup(std::unique_ptr<SeatProcessGroup>& group, std::string& error) {
    if (!group) return;
    ProcessStopPolicy cleanup;
    cleanup.gracefulTimeoutMs = 100;
    cleanup.forcedTimeoutMs = 2000;
    check(group->stop(cleanup, &error), "fixture process group stops without orphan windows");
}

void testQueueOverflowIsVisible() {
    WindowTrackerOptions options;
    options.callbackQueueCapacity = 1;
    options.eventHistoryCapacity = 4;
    WindowTracker tracker(options);
    check(tracker.notifyWindowChange(101u, WindowChangeHint::Created),
          "first bounded callback event is accepted");
    check(!tracker.notifyWindowChange(202u, WindowChangeHint::Shown),
          "callback queue overflow rejects excess work instead of growing unbounded");
    check(tracker.snapshot().droppedCallbackEvents == 1u,
          "callback queue overflow is visible in tracker snapshot");
}

void testProfileDiagnostics() {
    WindowTracker tracker;
    WindowProfileRules invalid;
    invalid.overrides.push_back(WindowRule{});
    std::string error;
    check(!tracker.setProfileRules(invalid, &error) && !error.empty(),
          "empty typed window override is rejected with diagnostic");
}

void testRealWindowTracking() {
    std::string error;
    auto controlledSpec = fixtureSpec(21, L"launcher");
    controlledSpec.arguments.push_back(L"--spawn-child");
    auto controlled = ProcessLauncher::launch(controlledSpec, &error);
    check(controlled.group != nullptr, "controlled launcher fixture starts in Seat process group");
    if (!controlled.group) return;

    auto unrelatedSpec = fixtureSpec(99, L"unowned");
    auto unrelated = ProcessLauncher::launch(unrelatedSpec, &error);
    check(unrelated.group != nullptr, "unrelated fixture starts outside tracked Seat set");
    if (!unrelated.group) return;

    check(waitUntil([&] { return controlled.group->snapshot().processes.size() >= 2u; },
                    std::chrono::milliseconds(2000)),
          "launcher-spawned game process is captured by Seat Job Object");

    const auto unrelatedRoot = unrelated.group->rootIdentity();
    HWND unrelatedWindow = nullptr;
    check(waitUntil([&] {
        unrelatedWindow = findFixtureWindow(unrelatedRoot.processId, L"Hydra Unowned");
        return unrelatedWindow != nullptr;
    }, std::chrono::milliseconds(2000)),
          "unrelated top-level window exists for non-adoption proof");

    WindowTrackerOptions trackerOptions;
    trackerOptions.callbackQueueCapacity = 16;
    trackerOptions.eventHistoryCapacity = 256;
    WindowTracker tracker(trackerOptions);
    WindowProfileRules rules;
    rules.defaultRole = WindowRole::Ignored;

    WindowRule popupRule;
    popupRule.role = WindowRole::ChildOwnedPopup;
    popupRule.titleContains = L"Hydra Game Popup";
    rules.overrides.push_back(std::move(popupRule));
    WindowRule launcherRule;
    launcherRule.role = WindowRole::Launcher;
    launcherRule.titleContains = L"Hydra Launcher";
    launcherRule.rootProcessOnly = true;
    rules.overrides.push_back(std::move(launcherRule));

    WindowRule gameRule;
    gameRule.role = WindowRole::PrimaryGame;
    gameRule.titleContains = L"Hydra Game";
    rules.overrides.push_back(std::move(gameRule));
    check(tracker.setProfileRules(std::move(rules), &error),
          "typed launcher classification rule is accepted");
    tracker.setProcessTrees({controlled.group->snapshot()});
    check(tracker.start(&error), "WinEvent window tracker starts on dedicated pump thread");
    if (!tracker.running()) return;

    check(waitUntil([&] { return tracker.snapshot().windows.size() >= 4u; },
                    std::chrono::milliseconds(2500)),
          "initial enumeration attributes multiple launcher/game windows");
    auto initial = tracker.snapshot();
    check(initial.windows.size() == 4u,
          "only four controlled fixture windows are adopted");
    check(std::all_of(initial.windows.begin(), initial.windows.end(),
                      [](const TrackedWindow& window) { return window.seatId == 21u; }),
          "every adopted window is attributed to the controlled Seat");
    check(!findTitle(initial, L"Hydra Unowned").has_value(),
          "unowned process window is never adopted");

    const auto launcher = findTitle(initial, L"Hydra Launcher");
    const auto launcherExtra = findTitle(initial, L"Hydra Launcher Extra");
    const auto game = findTitle(initial, L"Hydra Game");
    const auto popup = findTitle(initial, L"Hydra Game Popup");
    check(launcher && launcherExtra && game && popup,
          "controlled multi-window titles are all present");
    if (!(launcher && launcherExtra && game && popup)) {
        tracker.stop();
        return;
    }

    check(launcher->role == WindowRole::Launcher &&
              launcherExtra->role == WindowRole::Launcher,
          "profile rule classifies root launcher windows deterministically");
    check(game->role == WindowRole::PrimaryGame,
          "game child window receives default primary-game role");
    check(popup->role == WindowRole::ChildOwnedPopup && popup->ownerHandle != 0,
          "owned popup is classified without being confused with unrelated windows");
    check(launcher->identity.process.processId == controlled.root.processId &&
              game->identity.process.processId != controlled.root.processId,
          "launcher-spawned child window is attributed through process-tree identity");
    const auto authoritativeGame = waitTarget(
        tracker, 21u, WindowTargetKind::Visual, WindowTargetStatus::Bound,
        std::chrono::milliseconds(1000));
    check(authoritativeGame && authoritativeGame->window &&
              authoritativeGame->window->identity.process.sameInstance(game->identity.process) &&
              !authoritativeGame->window->rootProcess,
          "exact owned descendant is accepted as the authoritative visual target");

    const auto launcherPid = launcher->identity.process.processId;
    const auto gamePid = game->identity.process.processId;
    const auto launcherWindowCount = std::count_if(
        initial.windows.begin(), initial.windows.end(), [launcherPid](const TrackedWindow& window) {
            return window.identity.process.processId == launcherPid;
        });
    const auto gameWindowCount = std::count_if(
        initial.windows.begin(), initial.windows.end(), [gamePid](const TrackedWindow& window) {
            return window.identity.process.processId == gamePid;
        });
    check(launcherWindowCount == 2 && gameWindowCount == 2,
          "multiple top-level/owned windows from each process remain distinct");

    check(tracker.validateIdentity(game->identity),
          "live HWND validates against exact process creation identity");
    auto staleGeneration = game->identity;
    ++staleGeneration.trackerGeneration;
    check(!tracker.validateIdentity(staleGeneration),
          "tracker generation rejects stale/reused HWND identity in the same process");
    auto stale = game->identity;
    ++stale.process.creationTime100ns;
    check(!tracker.validateIdentity(stale),
          "stale/reused HWND candidate with wrong process creation identity is rejected");
    WindowIdentity fake = game->identity;
    fake.nativeHandle = 1u;
    check(!tracker.validateIdentity(fake), "fake HWND is rejected before any action");

    const auto gameHwnd = reinterpret_cast<HWND>(game->identity.nativeHandle);
    check(SetWindowTextW(gameHwnd, L"Hydra Game Renamed") != FALSE,
          "fixture title mutation request succeeds");
    check(waitUntil([&] {
        return findTitle(tracker.snapshot(), L"Hydra Game Renamed").has_value();
    }, std::chrono::milliseconds(2000)),
          "EVENT_OBJECT_NAMECHANGE updates authoritative title snapshot");

    check(SetWindowPos(gameHwnd, nullptr, 333, 222, 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "fixture location mutation request succeeds");
    check(waitUntil([&] {
        const auto renamed = findTitle(tracker.snapshot(), L"Hydra Game Renamed");
        return renamed && renamed->bounds.left == 333 && renamed->bounds.top == 222;
    }, std::chrono::milliseconds(2000)),
          "EVENT_OBJECT_LOCATIONCHANGE updates window bounds");

    check(ShowWindowAsync(gameHwnd, SW_HIDE) != FALSE,
          "fixture hide request is accepted");
    check(waitUntil([&] {
        const auto renamed = findTitle(tracker.snapshot(), L"Hydra Game Renamed");
        return renamed && !renamed->visible;
    }, std::chrono::milliseconds(2000)),
          "EVENT_OBJECT_HIDE updates visibility");
    check(ShowWindowAsync(gameHwnd, SW_SHOWNA) != FALSE,
          "fixture show request is accepted");
    check(waitUntil([&] {
        const auto renamed = findTitle(tracker.snapshot(), L"Hydra Game Renamed");
        return renamed && renamed->visible;
    }, std::chrono::milliseconds(2000)),
          "EVENT_OBJECT_SHOW restores visibility");

    const auto popupHandle = reinterpret_cast<HWND>(popup->identity.nativeHandle);
    check(PostMessageW(popupHandle, WM_CLOSE, 0, 0) != FALSE,
          "fixture popup close request is posted");
    check(waitUntil([&] {
        return !findTitle(tracker.snapshot(), L"Hydra Game Popup").has_value();
    }, std::chrono::milliseconds(2000)),
          "destroy event removes tracked window idempotently");
    (void)tracker.notifyWindowChange(popup->identity.nativeHandle, WindowChangeHint::Destroyed);
    (void)tracker.notifyWindowChange(popup->identity.nativeHandle, WindowChangeHint::Destroyed);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(!findTitle(tracker.snapshot(), L"Hydra Game Popup").has_value(),
          "duplicate destroy observations do not recreate or corrupt state");

    check(unrelatedWindow != nullptr && IsWindow(unrelatedWindow) != FALSE,
          "unrelated window remains alive after controlled-window updates");
    std::wstring unrelatedTitle(64u, L'\0');
    const int copied = GetWindowTextW(unrelatedWindow, unrelatedTitle.data(),
                                      static_cast<int>(unrelatedTitle.size()));
    unrelatedTitle.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0u);
    check(unrelatedTitle == L"Hydra Unowned",
          "unrelated window title/state is untouched by tracker");

    bool historyOverflow = false;
    const auto events = tracker.eventsAfter(initial.sequence, 128u, historyOverflow);
    const auto hasHint = [&](WindowChangeHint hint) {
        return std::any_of(events.begin(), events.end(),
                           [hint](const WindowTrackerEvent& event) { return event.hint == hint; });
    };
    const bool completeHistory = !historyOverflow && hasHint(WindowChangeHint::TitleChanged) &&
              hasHint(WindowChangeHint::LocationChanged) &&
              hasHint(WindowChangeHint::Hidden) &&
              hasHint(WindowChangeHint::Shown) &&
              hasHint(WindowChangeHint::Destroyed);
    if (!completeHistory) {
        std::cerr << "Window history diagnostic: overflow=" << historyOverflow
                  << " events=" << events.size() << " hints=";
        for (const auto& event : events) {
            std::cerr << static_cast<int>(event.hint) << ',';
        }
        std::cerr << '\n';
    }
    check(completeHistory,
          "bounded host-facing event history exposes title/location/show/hide/destroy changes");

    const auto beforeOverflow = findTitle(tracker.snapshot(), L"Hydra Game Renamed");
    check(beforeOverflow.has_value(), "live controlled window exists before overflow recovery");
    tracker.stop();
    for (std::uintptr_t handle = 100001u; handle <= 100016u; ++handle) {
        check(tracker.notifyWindowChange(handle, WindowChangeHint::Created),
              "bounded callback queue accepts entries up to its configured capacity");
    }
    check(!tracker.notifyWindowChange(100017u, WindowChangeHint::Created),
          "callback queue overflow is visible and rejects excess work");
    error.clear();
    check(tracker.start(&error), "window tracker restarts after queued overflow");
    check(waitUntil([&] {
        const auto refreshed = findTitle(tracker.snapshot(), L"Hydra Game Renamed");
        const auto refreshedSnapshot = tracker.snapshot();
        return refreshed && refreshedSnapshot.droppedCallbackEvents >= 1u &&
               (!beforeOverflow || refreshed->identity.trackerGeneration !=
                                      beforeOverflow->identity.trackerGeneration);
    }, std::chrono::milliseconds(2000)),
          "overflow invalidates prior identities and full rescan issues a fresh generation");
    const auto afterOverflow = findTitle(tracker.snapshot(), L"Hydra Game Renamed");
    if (beforeOverflow && afterOverflow) {
        check(!tracker.validateIdentity(beforeOverflow->identity),
              "pre-overflow window identity fails closed after rescan");
        check(tracker.validateIdentity(afterOverflow->identity),
              "post-overflow window identity validates after authoritative rescan");
    }
    tracker.stop();
    ProcessStopPolicy cleanup;
    cleanup.gracefulTimeoutMs = 100;
    cleanup.forcedTimeoutMs = 2000;
    check(controlled.group->stop(cleanup, &error),
          "controlled launcher/game tree exits without orphan fixture processes");
    check(unrelated.group->stop(cleanup, &error),
          "unrelated fixture is cleaned only by its own process owner");
}

void testStableTargetAndRestartInvalidation() {
    std::string error;
    auto launched = ProcessLauncher::launch(fixtureSpec(31u, L"game"), &error);
    check(launched.group != nullptr, "stable-window fixture launches");
    if (!launched.group) return;

    WindowTracker tracker;
    check(tracker.setProfileRules(gameTargetRules(), &error),
          "stable-window target rules are accepted");
    tracker.setProcessTrees({launched.group->snapshot()});
    check(tracker.start(&error), "stable-window tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto initial = waitTarget(tracker, 31u, WindowTargetKind::Visual,
                                    WindowTargetStatus::Bound,
                                    std::chrono::milliseconds(1500));
    check(initial && initial->window, "initial stable HWND becomes authoritative");
    if (initial && initial->window) {
        const auto identity = initial->window->identity;
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        const auto stable = tracker.target(31u, WindowTargetKind::Visual);
        check(stable && stable->status == WindowTargetStatus::Bound && stable->window &&
                  stable->window->identity.sameInstance(identity),
              "stable HWND remains bound without generation churn");

        tracker.stop();
        const auto stopped = tracker.target(31u, WindowTargetKind::Visual);
        check(stopped && stopped->status == WindowTargetStatus::Unresolved && !stopped->window,
              "tracker stop clears the authoritative HWND instead of retaining stale state");
        check(!tracker.validateIdentity(identity),
              "pre-stop HWND identity is invalid after tracker stop");

        error.clear();
        check(tracker.start(&error), "tracker restarts against the still-owned process tree");
        const auto rebound = waitTarget(tracker, 31u, WindowTargetKind::Visual,
                                        WindowTargetStatus::Bound,
                                        std::chrono::milliseconds(1500));
        check(rebound && rebound->window &&
                  rebound->window->identity.process.sameInstance(identity.process) &&
                  rebound->window->identity.trackerGeneration != identity.trackerGeneration,
              "restart reacquires the live HWND with a fresh tracker identity generation");
        check(!tracker.validateIdentity(identity),
              "old generation stays invalid after restart reacquisition");
    }

    tracker.stop();
    stopFixtureGroup(launched.group, error);
}

void testDestroyedWindowReacquiresExactReplacement() {
    std::string error;
    auto launched = ProcessLauncher::launch(fixtureSpec(41u, L"recreate"), &error);
    check(launched.group != nullptr, "recreate fixture launches");
    if (!launched.group) return;

    WindowTrackerOptions options;
    options.reacquisitionTimeoutMs = 2500u;
    WindowTracker tracker(options);
    check(tracker.setProfileRules(gameTargetRules(), &error),
          "recreate target rules are accepted");
    tracker.setProcessTrees({launched.group->snapshot()});
    check(tracker.start(&error), "recreate tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto initial = waitTarget(tracker, 41u, WindowTargetKind::Visual,
                                    WindowTargetStatus::Bound,
                                    std::chrono::milliseconds(1200));
    check(initial && initial->window && initial->window->title == L"Hydra Game",
          "initial game HWND is bound before recreation");
    if (initial && initial->window) {
        const auto staleIdentity = initial->window->identity;
        const auto gap = waitTarget(tracker, 41u, WindowTargetKind::Visual,
                                    WindowTargetStatus::Reacquiring,
                                    std::chrono::milliseconds(2500));
        check(gap && !gap->window,
              "destroyed authoritative HWND enters a temporary zero-window reacquisition interval");
        check(!tracker.validateIdentity(staleIdentity),
              "destroyed HWND cannot remain a valid visual/input identity during the gap");

        const auto replacement = waitTarget(tracker, 41u, WindowTargetKind::Visual,
                                            WindowTargetStatus::Bound,
                                            std::chrono::milliseconds(2500));
        check(replacement && replacement->window &&
                  replacement->window->title == L"Hydra Game Replacement" &&
                  replacement->window->identity.process.sameInstance(staleIdentity.process) &&
                  !replacement->window->identity.sameInstance(staleIdentity),
              "replacement HWND is accepted only for the exact authoritative Seat process");
        if (replacement && replacement->window) {
            check(replacement->window->identity.trackerGeneration !=
                      staleIdentity.trackerGeneration,
                  "recreated HWND receives a fresh logical identity even if Win32 reuses its value");
            check(tracker.validateIdentity(replacement->window->identity),
                  "validated replacement identity is live after reacquisition");
        }
    }

    tracker.stop();
    stopFixtureGroup(launched.group, error);
}

void testSplashPromotesToRealGameWindow() {
    std::string error;
    auto launched = ProcessLauncher::launch(fixtureSpec(42u, L"transition"), &error);
    check(launched.group != nullptr, "splash-transition fixture launches");
    if (!launched.group) return;

    WindowProfileRules rules;
    rules.defaultRole = WindowRole::Ignored;
    rules.visualTargetRole = WindowRole::PrimaryGame;
    WindowRule game;
    game.role = WindowRole::PrimaryGame;
    game.titleContains = L"Hydra Game";
    rules.overrides.push_back(std::move(game));
    WindowRule splash;
    splash.role = WindowRole::Launcher;
    splash.titleContains = L"Hydra Splash";
    rules.overrides.push_back(std::move(splash));

    WindowTrackerOptions options;
    options.reacquisitionTimeoutMs = 2500u;
    WindowTracker tracker(options);
    check(tracker.setProfileRules(std::move(rules), &error),
          "splash/game role rules are accepted");
    tracker.setProcessTrees({launched.group->snapshot()});
    check(tracker.start(&error), "splash-transition tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto splashTarget = waitTarget(tracker, 42u, WindowTargetKind::Visual,
                                         WindowTargetStatus::Bound,
                                         std::chrono::milliseconds(1200));
    check(splashTarget && splashTarget->window &&
              splashTarget->window->role == WindowRole::Launcher &&
              splashTarget->window->title == L"Hydra Splash",
          "owned launcher/splash is a bounded bootstrap visual target");
    const auto realGame = waitUntil([&] {
        const auto target = tracker.target(42u, WindowTargetKind::Visual);
        return target && target->status == WindowTargetStatus::Bound && target->window &&
               target->window->role == WindowRole::PrimaryGame &&
               target->window->title == L"Hydra Game";
    }, std::chrono::milliseconds(3500));
    check(realGame, "splash target deterministically promotes to the real game HWND");
    const auto finalTarget = tracker.target(42u, WindowTargetKind::Visual);
    check(finalTarget && finalTarget->window &&
              finalTarget->window->identity.process.sameInstance(launched.root),
          "splash-to-game promotion never leaves the authoritative process instance");

    tracker.stop();
    stopFixtureGroup(launched.group, error);
}

void testSameTitleAndOtherSeatAreRejectedAsOwnershipAuthority() {
    std::string error;
    auto seatA = ProcessLauncher::launch(fixtureSpec(51u, L"game"), &error);
    auto seatB = ProcessLauncher::launch(fixtureSpec(52u, L"same-title"), &error);
    auto unrelated = ProcessLauncher::launch(fixtureSpec(99u, L"same-title"), &error);
    check(seatA.group && seatB.group && unrelated.group,
          "two-Seat plus unrelated same-title fixtures launch");
    if (!(seatA.group && seatB.group && unrelated.group)) {
        stopFixtureGroup(seatA.group, error);
        stopFixtureGroup(seatB.group, error);
        stopFixtureGroup(unrelated.group, error);
        return;
    }

    WindowTracker tracker;
    check(tracker.setProfileRules(gameTargetRules(), &error),
          "same-title ownership rules are accepted");
    tracker.setProcessTrees({seatA.group->snapshot(), seatB.group->snapshot()});
    check(tracker.start(&error), "two-Seat ownership tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(seatA.group, error);
        stopFixtureGroup(seatB.group, error);
        stopFixtureGroup(unrelated.group, error);
        return;
    }

    const auto targetA = waitTarget(tracker, 51u, WindowTargetKind::Visual,
                                    WindowTargetStatus::Bound,
                                    std::chrono::milliseconds(1500));
    const auto targetB = waitTarget(tracker, 52u, WindowTargetKind::Visual,
                                    WindowTargetStatus::Bound,
                                    std::chrono::milliseconds(1500));
    check(targetA && targetA->window &&
              targetA->window->identity.process.sameInstance(seatA.root) &&
              !targetA->window->identity.process.sameInstance(seatB.root),
          "Seat A target cannot adopt Seat B's same-title HWND");
    check(targetB && targetB->window &&
              targetB->window->identity.process.sameInstance(seatB.root) &&
              !targetB->window->identity.process.sameInstance(seatA.root),
          "Seat B target remains bound to Seat B's exact process identity");

    const auto snapshot = tracker.snapshot();
    check(std::none_of(snapshot.windows.begin(), snapshot.windows.end(),
                       [&](const TrackedWindow& window) {
                           return window.identity.process.sameInstance(unrelated.root);
                       }),
          "arbitrary same-title/same-class process is never adopted without Seat authority");

    if (targetA && targetA->window && targetB && targetB->window) {
        auto reusedByOtherSeat = targetA->window->identity;
        reusedByOtherSeat.nativeHandle = targetB->window->identity.nativeHandle;
        reusedByOtherSeat.threadId = targetB->window->identity.threadId;
        reusedByOtherSeat.trackerGeneration = targetB->window->identity.trackerGeneration;
        check(!tracker.validateIdentity(reusedByOtherSeat),
              "HWND value now owned by another process cannot validate with stale Seat A process identity");

        const auto revokedSeatAIdentity = targetA->window->identity;
        tracker.setProcessTrees({seatB.group->snapshot()});
        check(!tracker.validateIdentity(revokedSeatAIdentity),
              "live HWND is invalid immediately after its Seat process-tree authority is revoked");
        const auto revokedTarget = tracker.target(51u, WindowTargetKind::Visual);
        check(revokedTarget && revokedTarget->status == WindowTargetStatus::Unresolved &&
                  !revokedTarget->window,
              "revoked Seat target clears before cleanup can touch its former HWND");
    }

    tracker.stop();
    stopFixtureGroup(seatA.group, error);
    stopFixtureGroup(seatB.group, error);
    stopFixtureGroup(unrelated.group, error);
}

void testConflictingExactProcessTreeAuthorityFailsClosed() {
    std::string error;
    auto launched = ProcessLauncher::launch(fixtureSpec(53u, L"game"), &error);
    check(launched.group != nullptr, "conflicting-authority fixture launches");
    if (!launched.group) return;

    WindowTracker tracker;
    check(tracker.setProfileRules(gameTargetRules(), &error),
          "conflicting-authority target rules are accepted");
    const auto validTree = launched.group->snapshot();
    tracker.setProcessTrees({validTree});
    check(tracker.start(&error), "conflicting-authority tracker starts from one exact Seat owner");
    if (!tracker.running()) {
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto initial = waitTarget(tracker, 53u, WindowTargetKind::Visual,
                                    WindowTargetStatus::Bound,
                                    std::chrono::milliseconds(1500));
    check(initial && initial->window && tracker.validateIdentity(initial->window->identity),
          "single exact process-tree owner initially validates its HWND");
    if (initial && initial->window) {
        const auto previouslyValid = initial->window->identity;
        auto conflictingTree = validTree;
        conflictingTree.seatId = 54u;
        tracker.setProcessTrees({validTree, conflictingTree});

        check(!tracker.validateIdentity(previouslyValid),
              "same exact process instance claimed by two Seats revokes prior HWND authority immediately");
        const auto seatA = tracker.target(53u, WindowTargetKind::Visual);
        const auto seatB = tracker.target(54u, WindowTargetKind::Visual);
        check((!seatA || seatA->status != WindowTargetStatus::Bound || !seatA->window) &&
                  (!seatB || seatB->status != WindowTargetStatus::Bound || !seatB->window),
              "ambiguous cross-Seat process ownership never chooses a first matching Seat");
        check(waitUntil([&] {
                  const auto snapshot = tracker.snapshot();
                  return std::none_of(snapshot.windows.begin(), snapshot.windows.end(),
                                      [&](const TrackedWindow& window) {
                                          return window.identity.process.sameInstance(launched.root);
                                      });
              }, std::chrono::milliseconds(1500)),
              "ambiguous exact process authority removes its HWNDs from tracked ownership");
    }

    tracker.stop();
    stopFixtureGroup(launched.group, error);
}

void testNoCandidateFailsClosedWithinBound() {
    std::string error;
    auto launched = ProcessLauncher::launch(fixtureSpec(61u, L"unowned"), &error);
    check(launched.group != nullptr, "fail-closed fixture launches");
    if (!launched.group) return;

    WindowTrackerOptions options;
    options.reacquisitionTimeoutMs = 200u;
    WindowTracker tracker(options);
    WindowProfileRules rules;
    rules.defaultRole = WindowRole::Ignored;
    rules.visualTargetRole = WindowRole::PrimaryGame;
    check(tracker.setProfileRules(std::move(rules), &error),
          "fail-closed profile accepts an explicit empty whitelist");
    tracker.setProcessTrees({launched.group->snapshot()});
    const auto startedAt = std::chrono::steady_clock::now();
    check(tracker.start(&error), "fail-closed tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto failed = waitTarget(tracker, 61u, WindowTargetKind::Visual,
                                   WindowTargetStatus::FailedClosed,
                                   std::chrono::milliseconds(1500));
    const auto elapsed = std::chrono::steady_clock::now() - startedAt;
    check(failed && !failed->window,
          "no proven candidate reaches a terminal fail-closed target state");
    check(elapsed < std::chrono::seconds(2),
          "reacquisition deadline is bounded and does not poll forever");
    check(tracker.snapshot().windows.empty(),
          "ignored helper window is not promoted to escape fail-closed state");

    tracker.stop();
    stopFixtureGroup(launched.group, error);
}

void testInputDefaultsToValidatedVisualTarget() {
    std::string error;
    auto launched = ProcessLauncher::launch(fixtureSpec(70u, L"input-stable"), &error);
    check(launched.group != nullptr, "default-mirror secondary-window fixture launches");
    if (!launched.group) return;

    WindowTracker tracker;
    check(tracker.setProfileRules(visualOnlyTargetRules(), &error),
          "visual-only rules leave inputTargetRole unset");
    tracker.setProcessTrees({launched.group->snapshot()});
    check(tracker.start(&error), "default-mirror tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto visual = waitTarget(tracker, 70u, WindowTargetKind::Visual,
                                   WindowTargetStatus::Bound,
                                   std::chrono::milliseconds(1500));
    const auto input = waitTarget(tracker, 70u, WindowTargetKind::Input,
                                  WindowTargetStatus::Bound,
                                  std::chrono::milliseconds(1500));
    check(visual && visual->window && input && input->window &&
              visual->window->identity.sameInstance(input->window->identity) &&
              visual->bindingGeneration == input->bindingGeneration,
          "without inputTargetRole the input target mirrors the exact validated visual target");
    const auto snapshot = tracker.snapshot();
    check(std::none_of(snapshot.windows.begin(), snapshot.windows.end(),
                       [](const TrackedWindow& window) {
                           return window.title == L"Hydra Input Sink";
                       }),
          "an unconfigured secondary HWND is not invented as a second input authority");

    tracker.stop();
    const auto stoppedVisual = tracker.target(70u, WindowTargetKind::Visual);
    const auto stoppedInput = tracker.target(70u, WindowTargetKind::Input);
    check(stoppedVisual && stoppedInput &&
              stoppedVisual->status == WindowTargetStatus::Unresolved && !stoppedVisual->window &&
              stoppedInput->status == WindowTargetStatus::Unresolved && !stoppedInput->window,
          "tracker stop clears both mirrored target snapshots without retaining stale HWND authority");
    stopFixtureGroup(launched.group, error);
}

void testDistinctInputTargetReacquiresOnlyValidatedSink() {
    std::string error;
    auto launched = ProcessLauncher::launch(fixtureSpec(71u, L"input-target"), &error);
    check(launched.group != nullptr, "distinct-input fixture launches");
    if (!launched.group) return;

    WindowTrackerOptions options;
    options.reacquisitionTimeoutMs = 2500u;
    WindowTracker tracker(options);
    check(tracker.setProfileRules(secondaryInputTargetRules(), &error),
          "distinct visual/input target rules are accepted");
    tracker.setProcessTrees({launched.group->snapshot()});
    check(tracker.start(&error), "distinct-input tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto initialVisual = waitTarget(tracker, 71u, WindowTargetKind::Visual,
                                          WindowTargetStatus::Bound,
                                          std::chrono::milliseconds(1200));
    const auto initialInput = waitTarget(tracker, 71u, WindowTargetKind::Input,
                                         WindowTargetStatus::Bound,
                                         std::chrono::milliseconds(1200));
    check(initialVisual && initialVisual->window && initialInput && initialInput->window &&
              !initialVisual->window->identity.sameInstance(initialInput->window->identity) &&
              initialVisual->window->visible && !initialInput->window->visible,
          "one exact process may own a visible visual HWND and a distinct hidden input HWND");
    if (initialVisual && initialVisual->window && initialInput && initialInput->window) {
        const auto visualIdentity = initialVisual->window->identity;
        const auto staleInputIdentity = initialInput->window->identity;
        const auto initialInputGeneration = initialInput->bindingGeneration;
        check(tracker.validateIdentity(visualIdentity) && tracker.validateIdentity(staleInputIdentity) &&
                  visualIdentity.process.sameInstance(launched.root) &&
                  staleInputIdentity.process.sameInstance(launched.root),
              "both initial targets validate against the exact authoritative Seat process identity");

        const auto inputGap = waitTarget(tracker, 71u, WindowTargetKind::Input,
                                         WindowTargetStatus::Reacquiring,
                                         std::chrono::milliseconds(2500));
        check(inputGap && !inputGap->window &&
                  inputGap->bindingGeneration > initialInputGeneration,
              "destroyed input HWND clears authority and advances the bounded reacquisition generation");
        check(!tracker.validateIdentity(staleInputIdentity),
              "destroyed input HWND cannot remain valid even if Windows later reuses its numeric value");
        const auto visualDuringGap = tracker.target(71u, WindowTargetKind::Visual);
        check(visualDuringGap && visualDuringGap->status == WindowTargetStatus::Bound &&
                  visualDuringGap->window &&
                  visualDuringGap->window->identity.sameInstance(visualIdentity),
              "input HWND replacement does not disturb the Seat's stable visual HWND");

        const auto replacementInput = waitTarget(tracker, 71u, WindowTargetKind::Input,
                                                 WindowTargetStatus::Bound,
                                                 std::chrono::milliseconds(2500));
        check(replacementInput && replacementInput->window &&
                  replacementInput->window->role == WindowRole::InputTarget &&
                  replacementInput->window->title == L"Hydra Input Sink Replacement" &&
                  !replacementInput->window->visible &&
                  replacementInput->window->identity.process.sameInstance(staleInputIdentity.process) &&
                  !replacementInput->window->identity.sameInstance(staleInputIdentity) &&
                  replacementInput->bindingGeneration > initialInputGeneration &&
                  tracker.validateIdentity(replacementInput->window->identity),
              "hidden input replacement is accepted only after exact-process validation with a newer target generation");
        if (replacementInput && replacementInput->window) {
            auto staleAtCurrentNumericHandle = staleInputIdentity;
            staleAtCurrentNumericHandle.nativeHandle =
                replacementInput->window->identity.nativeHandle;
            check(!tracker.validateIdentity(staleAtCurrentNumericHandle),
                  "stale tracker generation rejects an old identity even when probed at a real current numeric HWND value");
            if (replacementInput->window->identity.nativeHandle == staleInputIdentity.nativeHandle) {
                check(replacementInput->window->identity.trackerGeneration !=
                          staleInputIdentity.trackerGeneration,
                      "actual numeric HWND reuse still receives a fresh tracker identity generation");
            }
        }
    }

    tracker.stop();
    stopFixtureGroup(launched.group, error);
}

void testVisualRecreationDoesNotReplaceStableInputTarget() {
    std::string error;
    auto launched = ProcessLauncher::launch(
        fixtureSpec(72u, L"input-visual-recreate"), &error);
    check(launched.group != nullptr, "independent visual-recreate fixture launches");
    if (!launched.group) return;

    WindowTrackerOptions options;
    options.reacquisitionTimeoutMs = 2500u;
    WindowTracker tracker(options);
    check(tracker.setProfileRules(secondaryInputTargetRules(), &error),
          "independent visual/input rules are accepted");
    tracker.setProcessTrees({launched.group->snapshot()});
    check(tracker.start(&error), "independent visual-recreate tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto initialVisual = waitTarget(tracker, 72u, WindowTargetKind::Visual,
                                          WindowTargetStatus::Bound,
                                          std::chrono::milliseconds(1200));
    const auto initialInput = waitTarget(tracker, 72u, WindowTargetKind::Input,
                                         WindowTargetStatus::Bound,
                                         std::chrono::milliseconds(1200));
    check(initialVisual && initialVisual->window && initialInput && initialInput->window,
          "visual-recreate fixture exposes both initial targets");
    if (initialVisual && initialVisual->window && initialInput && initialInput->window) {
        const auto staleVisual = initialVisual->window->identity;
        const auto inputIdentity = initialInput->window->identity;
        const auto inputGeneration = initialInput->bindingGeneration;
        const auto visualGeneration = initialVisual->bindingGeneration;

        const auto gap = waitTarget(tracker, 72u, WindowTargetKind::Visual,
                                    WindowTargetStatus::Reacquiring,
                                    std::chrono::milliseconds(2500));
        check(gap && !gap->window && gap->bindingGeneration > visualGeneration,
              "visual destruction enters its own bounded reacquisition generation");
        const auto inputDuringGap = tracker.target(72u, WindowTargetKind::Input);
        check(inputDuringGap && inputDuringGap->status == WindowTargetStatus::Bound &&
                  inputDuringGap->window &&
                  inputDuringGap->window->identity.sameInstance(inputIdentity) &&
                  inputDuringGap->bindingGeneration == inputGeneration,
              "stable input authority remains unchanged while the visual HWND is absent");
        check(!tracker.validateIdentity(staleVisual) && tracker.validateIdentity(inputIdentity),
              "stale visual identity is rejected without invalidating the independent input HWND");

        const auto replacementVisual = waitTarget(tracker, 72u, WindowTargetKind::Visual,
                                                  WindowTargetStatus::Bound,
                                                  std::chrono::milliseconds(2500));
        check(replacementVisual && replacementVisual->window &&
                  replacementVisual->window->title == L"Hydra Visual Replacement" &&
                  replacementVisual->window->identity.process.sameInstance(launched.root) &&
                  !replacementVisual->window->identity.sameInstance(staleVisual) &&
                  replacementVisual->bindingGeneration > visualGeneration,
              "visual replacement reacquires independently from the stable input target");
        const auto finalInput = tracker.target(72u, WindowTargetKind::Input);
        check(finalInput && finalInput->status == WindowTargetStatus::Bound && finalInput->window &&
                  finalInput->window->identity.sameInstance(inputIdentity) &&
                  finalInput->bindingGeneration == inputGeneration,
              "new visual HWND is never substituted for the still-valid input target");
    }

    tracker.stop();
    const auto stoppedVisual = tracker.target(72u, WindowTargetKind::Visual);
    const auto stoppedInput = tracker.target(72u, WindowTargetKind::Input);
    check(stoppedVisual && stoppedInput &&
              stoppedVisual->status == WindowTargetStatus::Unresolved && !stoppedVisual->window &&
              stoppedInput->status == WindowTargetStatus::Unresolved && !stoppedInput->window,
          "stop invalidates independently bound visual and input targets together");
    stopFixtureGroup(launched.group, error);
}

void testDistinctInputRejectsOtherSeatAndForeignMatches() {
    std::string error;
    auto seatA = ProcessLauncher::launch(fixtureSpec(73u, L"input-stable"), &error);
    auto seatB = ProcessLauncher::launch(fixtureSpec(74u, L"input-stable"), &error);
    auto foreign = ProcessLauncher::launch(fixtureSpec(99u, L"input-foreign"), &error);
    check(seatA.group && seatB.group && foreign.group,
          "two Seats plus foreign same-input-window fixture launch");
    if (!(seatA.group && seatB.group && foreign.group)) {
        stopFixtureGroup(seatA.group, error);
        stopFixtureGroup(seatB.group, error);
        stopFixtureGroup(foreign.group, error);
        return;
    }

    HWND foreignWindow = nullptr;
    check(waitUntil([&] {
        foreignWindow = findFixtureWindow(foreign.root.processId, L"Hydra Input Sink");
        return foreignWindow != nullptr;
    }, std::chrono::milliseconds(1500)),
          "foreign process exposes the same hidden input title/class window");

    WindowTracker tracker;
    check(tracker.setProfileRules(secondaryInputTargetRules(), &error),
          "two-Seat distinct-input rules are accepted");
    tracker.setProcessTrees({seatA.group->snapshot(), seatB.group->snapshot()});
    check(tracker.start(&error), "two-Seat distinct-input tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(seatA.group, error);
        stopFixtureGroup(seatB.group, error);
        stopFixtureGroup(foreign.group, error);
        return;
    }

    const auto inputA = waitTarget(tracker, 73u, WindowTargetKind::Input,
                                   WindowTargetStatus::Bound,
                                   std::chrono::milliseconds(1500));
    const auto inputB = waitTarget(tracker, 74u, WindowTargetKind::Input,
                                   WindowTargetStatus::Bound,
                                   std::chrono::milliseconds(1500));
    check(inputA && inputA->window && inputB && inputB->window &&
              inputA->window->identity.process.sameInstance(seatA.root) &&
              inputB->window->identity.process.sameInstance(seatB.root) &&
              !inputA->window->identity.process.sameInstance(seatB.root) &&
              !inputB->window->identity.process.sameInstance(seatA.root),
          "identical role/class/title input HWNDs remain isolated by exact Seat process identity");

    const auto snapshot = tracker.snapshot();
    check(std::none_of(snapshot.windows.begin(), snapshot.windows.end(),
                       [&](const TrackedWindow& window) {
                           return window.identity.process.sameInstance(foreign.root);
                       }),
          "foreign same-class/same-title/same-geometry HWND is rejected before role ranking");
    if (foreignWindow != nullptr && inputA && inputA->window) {
        wchar_t foreignClass[128]{};
        const int classLength = GetClassNameW(
            foreignWindow, foreignClass, static_cast<int>(std::size(foreignClass)));
        RECT foreignBounds{};
        const bool sameShape = classLength > 0 &&
            std::wstring_view(foreignClass, static_cast<std::size_t>(classLength)) ==
                inputA->window->className &&
            GetWindowRect(foreignWindow, &foreignBounds) != FALSE &&
            foreignBounds.left == inputA->window->bounds.left &&
            foreignBounds.top == inputA->window->bounds.top &&
            foreignBounds.right == inputA->window->bounds.right &&
            foreignBounds.bottom == inputA->window->bounds.bottom;
        check(sameShape,
              "foreign rejection fixture really matches the owned input class and geometry");
    }

    if (inputA && inputA->window && inputB && inputB->window) {
        auto reusedNumericHandle = inputA->window->identity;
        reusedNumericHandle.nativeHandle = inputB->window->identity.nativeHandle;
        reusedNumericHandle.threadId = inputB->window->identity.threadId;
        reusedNumericHandle.trackerGeneration = inputB->window->identity.trackerGeneration;
        check(!tracker.validateIdentity(reusedNumericHandle),
              "a real current numeric HWND cannot validate against stale ownership from another process instance");
    }

    tracker.stop();
    stopFixtureGroup(seatA.group, error);
    stopFixtureGroup(seatB.group, error);
    stopFixtureGroup(foreign.group, error);
}

void testAmbiguousDistinctInputTargetFailsClosed() {
    std::string error;
    auto launched = ProcessLauncher::launch(fixtureSpec(75u, L"input-ambiguous"), &error);
    check(launched.group != nullptr, "ambiguous-input fixture launches");
    if (!launched.group) return;

    WindowTracker tracker;
    check(tracker.setProfileRules(secondaryInputTargetRules(), &error),
          "ambiguous input target profile is accepted as typed policy");
    tracker.setProcessTrees({launched.group->snapshot()});
    check(tracker.start(&error), "ambiguous-input tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto visual = waitTarget(tracker, 75u, WindowTargetKind::Visual,
                                   WindowTargetStatus::Bound,
                                   std::chrono::milliseconds(1500));
    const auto failedInput = waitTarget(tracker, 75u, WindowTargetKind::Input,
                                        WindowTargetStatus::FailedClosed,
                                        std::chrono::milliseconds(1500));
    check(visual && visual->window && failedInput && !failedInput->window &&
              failedInput->bindingGeneration != 0,
          "two equally valid exact-owned input HWNDs fail closed without disturbing the visual target");

    const auto snapshot = tracker.snapshot();
    std::vector<TrackedWindow> inputCandidates;
    for (const auto& window : snapshot.windows) {
        if (window.seatId == 75u && window.role == WindowRole::InputTarget) {
            inputCandidates.push_back(window);
        }
    }
    check(inputCandidates.size() == 2u &&
              inputCandidates[0].title == L"Hydra Input Sink" &&
              inputCandidates[1].title == L"Hydra Input Sink" &&
              inputCandidates[0].className == inputCandidates[1].className &&
              inputCandidates[0].bounds == inputCandidates[1].bounds,
          "ambiguity fixture presents two indistinguishable owned role/class/title/geometry candidates");

    if (!inputCandidates.empty()) {
        const auto oneCandidate = reinterpret_cast<HWND>(
            inputCandidates.front().identity.nativeHandle);
        check(PostMessageW(oneCandidate, WM_CLOSE, 0, 0) != FALSE,
              "one ambiguous input candidate can be removed deterministically");
        check(waitUntil([&] {
            const auto current = tracker.snapshot();
            return std::count_if(current.windows.begin(), current.windows.end(),
                                 [](const TrackedWindow& window) {
                                     return window.seatId == 75u &&
                                            window.role == WindowRole::InputTarget;
                                 }) == 1;
        }, std::chrono::milliseconds(1500)),
              "tracker observes removal of one ambiguous candidate");
        const auto stillFailed = tracker.target(75u, WindowTargetKind::Input);
        check(stillFailed && stillFailed->status == WindowTargetStatus::FailedClosed &&
                  !stillFailed->window,
              "ambiguity failure is sticky instead of silently choosing the surviving HWND");
    }

    tracker.stop();
    stopFixtureGroup(launched.group, error);
}

void testSplashVisualInputProgressionKeepsOwnershipAndRolesSeparate() {
    std::string error;
    auto launched = ProcessLauncher::launch(fixtureSpec(76u, L"input-progression"), &error);
    check(launched.group != nullptr, "splash/visual/input progression fixture launches");
    if (!launched.group) return;

    WindowTrackerOptions options;
    options.reacquisitionTimeoutMs = 4000u;
    WindowTracker tracker(options);
    check(tracker.setProfileRules(secondaryInputTargetRules(true), &error),
          "progression profile separates splash, visual and input roles");
    tracker.setProcessTrees({launched.group->snapshot()});
    check(tracker.start(&error), "progression tracker starts");
    if (!tracker.running()) {
        stopFixtureGroup(launched.group, error);
        return;
    }

    const auto splash = waitTarget(tracker, 76u, WindowTargetKind::Visual,
                                   WindowTargetStatus::Bound,
                                   std::chrono::milliseconds(1200));
    const auto inputBefore = tracker.target(76u, WindowTargetKind::Input);
    check(splash && splash->window && splash->window->role == WindowRole::Launcher &&
              splash->window->title == L"Hydra Splash" &&
              splash->window->identity.process.sameInstance(launched.root) &&
              inputBefore && inputBefore->status == WindowTargetStatus::Reacquiring &&
              !inputBefore->window,
          "owned splash may bootstrap visual identity while distinct input authority stays unresolved");

    std::optional<WindowTargetSnapshot> visualGame;
    check(waitUntil([&] {
        visualGame = tracker.target(76u, WindowTargetKind::Visual);
        const auto input = tracker.target(76u, WindowTargetKind::Input);
        return visualGame && visualGame->status == WindowTargetStatus::Bound &&
               visualGame->window && visualGame->window->role == WindowRole::PrimaryGame &&
               visualGame->window->title == L"Hydra Visual" && input &&
               input->status == WindowTargetStatus::Reacquiring && !input->window;
    }, std::chrono::milliseconds(3500)),
          "visual game HWND can become authoritative before the secondary input sink exists");

    const auto finalInput = waitTarget(tracker, 76u, WindowTargetKind::Input,
                                       WindowTargetStatus::Bound,
                                       std::chrono::milliseconds(2000));
    const auto finalVisual = tracker.target(76u, WindowTargetKind::Visual);
    check(visualGame && visualGame->window && finalVisual && finalVisual->window &&
              finalInput && finalInput->window &&
              finalVisual->window->identity.sameInstance(visualGame->window->identity) &&
              finalVisual->window->role == WindowRole::PrimaryGame &&
              finalInput->window->role == WindowRole::InputTarget &&
              !finalInput->window->visible &&
              !finalVisual->window->identity.sameInstance(finalInput->window->identity) &&
              finalVisual->window->identity.process.sameInstance(launched.root) &&
              finalInput->window->identity.process.sameInstance(launched.root),
          "splash-to-main role progression does not collapse later input-target ownership into the visual HWND");

    tracker.stop();
    stopFixtureGroup(launched.group, error);
}

#endif

} // namespace

int main() {
#ifdef _WIN32
    testQueueOverflowIsVisible();
    testProfileDiagnostics();
    testRealWindowTracking();
    testStableTargetAndRestartInvalidation();
    testDestroyedWindowReacquiresExactReplacement();
    testSplashPromotesToRealGameWindow();
    testSameTitleAndOtherSeatAreRejectedAsOwnershipAuthority();
    testConflictingExactProcessTreeAuthorityFailsClosed();
    testNoCandidateFailsClosedWithinBound();
    testInputDefaultsToValidatedVisualTarget();
    testDistinctInputTargetReacquiresOnlyValidatedSink();
    testVisualRecreationDoesNotReplaceStableInputTarget();
    testDistinctInputRejectsOtherSeatAndForeignMatches();
    testAmbiguousDistinctInputTargetFailsClosed();
    testSplashVisualInputProgressionKeepsOwnershipAndRolesSeparate();
#else
    std::cout << "Window tracker integration tests are Windows-only.\n";
#endif
    if (failures != 0) {
        std::cerr << failures << " window tracker test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Window tracker tests passed.\n";
    return EXIT_SUCCESS;
}
