#pragma once

#include "hydra/window_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hydra::windowing {

enum class WindowChangeHint : std::uint8_t {
    Rescan = 0,
    Created = 1,
    Destroyed = 2,
    Shown = 3,
    Hidden = 4,
    TitleChanged = 5,
    LocationChanged = 6,
    Overflow = 7,
};

struct WindowRect {
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t right{0};
    std::int32_t bottom{0};

    friend bool operator==(const WindowRect&, const WindowRect&) = default;
};

struct TrackedWindow {
    WindowIdentity identity;
    SeatId seatId{0};
    WindowRole role{WindowRole::PrimaryGame};
    bool rootProcess{false};
    std::uintptr_t ownerHandle{0};
    bool visible{false};
    WindowRect bounds;
    std::wstring title;
    std::wstring className;

    friend bool operator==(const TrackedWindow&, const TrackedWindow&) = default;
};

struct WindowTargetSnapshot {
    SeatId seatId{0};
    WindowTargetKind kind{WindowTargetKind::Visual};
    WindowTargetStatus status{WindowTargetStatus::Unresolved};
    WindowRole desiredRole{WindowRole::PrimaryGame};
    std::uint64_t bindingGeneration{0};
    std::optional<TrackedWindow> window;

    friend bool operator==(const WindowTargetSnapshot&, const WindowTargetSnapshot&) = default;
};

struct SeatWindowTargets {
    SeatId seatId{0};
    WindowTargetSnapshot visual;
    WindowTargetSnapshot input;
    bool inputDistinct{false};

    friend bool operator==(const SeatWindowTargets&, const SeatWindowTargets&) = default;
};

struct WindowTrackerEvent {
    std::uint64_t sequence{0};
    WindowChangeHint hint{WindowChangeHint::Rescan};
    std::uintptr_t nativeHandle{0};
    std::optional<TrackedWindow> window;
    std::uint64_t droppedCallbackEvents{0};
};

struct WindowTrackerSnapshot {
    std::uint64_t sequence{0};
    std::uint64_t droppedCallbackEvents{0};
    std::vector<TrackedWindow> windows;
    std::vector<SeatWindowTargets> targets;
};

struct WindowTrackerOptions {
    std::size_t callbackQueueCapacity{256};
    std::size_t eventHistoryCapacity{256};
    std::uint32_t reacquisitionTimeoutMs{5000};
};

class WindowTargetObserver {
public:
    virtual ~WindowTargetObserver() = default;
    // Called from the tracker's worker after ownership state has been committed.
    // Implementations must only enqueue bounded work and return promptly.
    virtual void onWindowTargetChanged(const WindowTargetSnapshot& target) noexcept = 0;
};

class WindowTracker {
public:
    explicit WindowTracker(WindowTrackerOptions options = {});
    ~WindowTracker();
    WindowTracker(const WindowTracker&) = delete;
    WindowTracker& operator=(const WindowTracker&) = delete;
    WindowTracker(WindowTracker&&) noexcept;
    WindowTracker& operator=(WindowTracker&&) noexcept;

    bool start(std::string* error = nullptr);
    void stop() noexcept;
    bool running() const noexcept;

    void setProcessTrees(std::vector<process::ProcessTreeSnapshot> trees);
    bool setProfileRules(WindowProfileRules rules, std::string* error = nullptr);

    // Producers other than WinEvent (for example, a launcher adapter) may post a
    // bounded observation hint through the same queue. False means the queue was
    // full and the visible dropped-event counter was incremented.
    bool notifyWindowChange(std::uintptr_t nativeHandle, WindowChangeHint hint) noexcept;

    WindowTrackerSnapshot snapshot() const;
    // Input mirrors the validated visual target when no distinct input role is
    // configured. With a distinct role it never falls back to a visual/helper HWND:
    // consumers must require Bound, observe bindingGeneration changes, and call
    // validateIdentity() immediately before using the returned WindowIdentity.
    std::optional<WindowTargetSnapshot> target(SeatId seatId,
                                               WindowTargetKind kind) const;
    std::vector<WindowTrackerEvent> eventsAfter(std::uint64_t sequence,
                                                 std::size_t maxEvents,
                                                 bool& overflow) const;

    bool validateIdentity(const WindowIdentity& identity) const noexcept;
    std::uint64_t addTargetObserver(SeatId seatId, WindowTargetKind kind,
                                    std::weak_ptr<WindowTargetObserver> observer) const;
    void removeTargetObserver(std::uint64_t observerId) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hydra::windowing
