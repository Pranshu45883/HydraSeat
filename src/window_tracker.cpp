#include "hydra/window_tracker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
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

constexpr std::size_t kMaxProfileRules = 64u;
constexpr std::size_t kMaxRuleTextChars = 512u;
constexpr std::size_t kMaxWindowTextChars = 2048u;
constexpr std::size_t kMaxWindowClassChars = 512u;
constexpr std::size_t kMaxCallbackQueueCapacity = 4096u;
constexpr std::size_t kMaxEventHistoryCapacity = 4096u;
constexpr std::size_t kDuplicateScanDepth = 16u;
constexpr std::uint32_t kMinReacquisitionTimeoutMs = 100u;
constexpr std::uint32_t kMaxReacquisitionTimeoutMs = 30000u;

#ifdef _WIN32

struct UniqueHandle {
    HANDLE value{nullptr};
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : value(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value(other.value) {
        other.value = nullptr;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    bool valid() const noexcept {
        return value != nullptr && value != INVALID_HANDLE_VALUE;
    }
    void reset(HANDLE replacement = nullptr) noexcept {
        if (valid()) CloseHandle(value);
        value = replacement;
    }
};

std::uint64_t fileTimeValue(const FILETIME& value) noexcept {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

process::ProcessIdentity queryProcessIdentity(HANDLE processHandle,
                                              std::uint32_t processId) {
    process::ProcessIdentity identity;
    identity.processId = processId;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(processHandle, &created, &exited, &kernel, &user) != FALSE) {
        identity.creationTime100ns = fileTimeValue(created);
    }
    std::wstring path(32768u, L'\0');
    DWORD chars = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(processHandle, 0, path.data(), &chars) != FALSE) {
        path.resize(chars);
        identity.executablePath = std::move(path);
    }
    return identity;
}

std::wstring windowText(HWND window) {
    const int rawLength = GetWindowTextLengthW(window);
    const std::size_t requested = rawLength > 0
        ? std::min<std::size_t>(static_cast<std::size_t>(rawLength),
                                kMaxWindowTextChars)
        : 0u;
    std::wstring value(requested + 1u, L'\0');
    const int copied = GetWindowTextW(window, value.data(),
                                      static_cast<int>(value.size()));
    if (copied <= 0) return {};
    value.resize(static_cast<std::size_t>(copied));
    return value;
}

std::wstring windowClass(HWND window) {
    std::wstring value(kMaxWindowClassChars + 1u, L'\0');
    const int copied = GetClassNameW(window, value.data(),
                                     static_cast<int>(value.size()));
    if (copied <= 0) return {};
    value.resize(static_cast<std::size_t>(copied));
    return value;
}

std::wstring lowercase(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return result;
}

bool containsInsensitive(std::wstring_view value, std::wstring_view needle) {
    if (needle.empty()) return true;
    const auto haystackLower = lowercase(value);
    const auto needleLower = lowercase(needle);
    return haystackLower.find(needleLower) != std::wstring::npos;
}

bool equalsInsensitive(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) return false;
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

#endif

} // namespace

std::string_view windowRoleName(WindowRole role) noexcept {
    switch (role) {
        case WindowRole::PrimaryGame: return "primary-game";
        case WindowRole::Launcher: return "launcher";
        case WindowRole::Dialog: return "dialog";
        case WindowRole::Overlay: return "overlay";
        case WindowRole::ChildOwnedPopup: return "child-owned-popup";
        case WindowRole::InputTarget: return "input-target";
        case WindowRole::Ignored: return "ignored";
    }
    return "unknown";
}

class WindowTracker::Impl {
public:
    struct RawEvent {
        std::uintptr_t nativeHandle{0};
        WindowChangeHint hint{WindowChangeHint::Rescan};
    };

    struct OwnerMatch {
        SeatId seatId{0};
        bool rootProcess{false};
    };

    struct CandidateRank {
        int rolePenalty{0};
        int visibilityPenalty{0};
        int ownerPenalty{0};
        int rootPenalty{0};
        std::uint64_t area{0};
        std::uint64_t processCreation{0};
        std::uint32_t processId{0};
        std::uint32_t threadId{0};
        std::uintptr_t nativeHandle{0};
    };

    struct CandidateSelection {
        const TrackedWindow* window{nullptr};
        bool ambiguous{false};
    };

    struct TargetBinding {
        WindowTargetStatus status{WindowTargetStatus::Unresolved};
        WindowRole desiredRole{WindowRole::PrimaryGame};
        std::uint64_t generation{0};
        int maximumRolePenalty{std::numeric_limits<int>::max()};
        std::chrono::steady_clock::time_point deadline{};
        std::optional<TrackedWindow> window;
    };

    struct SeatTargetState {
        SeatId seatId{0};
        process::ProcessIdentity authorityRoot;
        std::vector<process::ProcessIdentity> liveProcesses;
        TargetBinding visual;
        TargetBinding input;
        bool distinctInput{false};
    };

    struct ObserverRecord {
        std::uint64_t id{0};
        SeatId seatId{0};
        WindowTargetKind kind{WindowTargetKind::Visual};
        std::weak_ptr<WindowTargetObserver> observer;
    };

    explicit Impl(WindowTrackerOptions requested) {
        options.callbackQueueCapacity = std::clamp<std::size_t>(
            requested.callbackQueueCapacity, 1u, kMaxCallbackQueueCapacity);
        options.eventHistoryCapacity = std::clamp<std::size_t>(
            requested.eventHistoryCapacity, 1u, kMaxEventHistoryCapacity);
        options.reacquisitionTimeoutMs = std::clamp<std::uint32_t>(
            requested.reacquisitionTimeoutMs, kMinReacquisitionTimeoutMs,
            kMaxReacquisitionTimeoutMs);
#ifdef _WIN32
        wakeEvent = UniqueHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
#endif
    }

    ~Impl() {
        stop();
    }

    WindowTrackerOptions options;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<std::uint64_t> droppedCallbackEvents{0};
    std::atomic<bool> overflowDirty{false};

    mutable std::mutex configMutex;
    std::vector<process::ProcessTreeSnapshot> processTrees;
    WindowProfileRules profileRules;

    mutable std::mutex queueMutex;
    std::deque<RawEvent> queue;

    mutable std::mutex stateMutex;
    std::vector<TrackedWindow> windows;
    std::deque<WindowTrackerEvent> history;
    std::map<SeatId, SeatTargetState> targetStates;
    WindowRole visualTargetRole{WindowRole::PrimaryGame};
    std::optional<WindowRole> inputTargetRole;
    std::uint64_t sequence{0};
    std::uint64_t nextIdentityGeneration{1};

    mutable std::mutex observerMutex;
    mutable std::vector<ObserverRecord> observers;
    mutable std::uint64_t nextObserverId{1};

    std::thread worker;
    std::mutex readyMutex;
    std::condition_variable readyCv;
    bool ready{false};
    bool setupSucceeded{false};
    std::string setupError;

#ifdef _WIN32
    UniqueHandle wakeEvent;
    HWINEVENTHOOK lifecycleHook{nullptr};
    HWINEVENTHOOK propertyHook{nullptr};

    inline static thread_local Impl* callbackOwner = nullptr;

    static void CALLBACK winEventCallback(HWINEVENTHOOK hook, DWORD event, HWND window,
                                          LONG objectId, LONG childId, DWORD, DWORD) {
        if (window == nullptr || objectId != OBJID_WINDOW || childId != CHILDID_SELF) return;
        Impl* owner = callbackOwner;
        if (owner == nullptr ||
            (hook != owner->lifecycleHook && hook != owner->propertyHook)) {
            return;
        }

        WindowChangeHint hint;
        switch (event) {
            case EVENT_OBJECT_CREATE: hint = WindowChangeHint::Created; break;
            case EVENT_OBJECT_DESTROY: hint = WindowChangeHint::Destroyed; break;
            case EVENT_OBJECT_SHOW: hint = WindowChangeHint::Shown; break;
            case EVENT_OBJECT_HIDE: hint = WindowChangeHint::Hidden; break;
            case EVENT_OBJECT_NAMECHANGE: hint = WindowChangeHint::TitleChanged; break;
            case EVENT_OBJECT_LOCATIONCHANGE: hint = WindowChangeHint::LocationChanged; break;
            default: return;
        }
        (void)owner->enqueue(reinterpret_cast<std::uintptr_t>(window), hint);
    }

    static BOOL CALLBACK initialWindowCallback(HWND window, LPARAM parameter) {
        auto* self = reinterpret_cast<Impl*>(parameter);
        if (self != nullptr) self->observe(window, WindowChangeHint::Rescan);
        return TRUE;
    }
#endif

    bool enqueue(std::uintptr_t nativeHandle, WindowChangeHint hint) noexcept {
        if (hint != WindowChangeHint::Rescan && nativeHandle == 0) return false;
        {
            std::lock_guard lock(queueMutex);
            std::size_t inspected = 0;
            for (auto iterator = queue.rbegin(); iterator != queue.rend() &&
                 inspected < kDuplicateScanDepth; ++iterator, ++inspected) {
                if (iterator->nativeHandle == nativeHandle && iterator->hint == hint) {
                    return true;
                }
            }
            if (queue.size() >= options.callbackQueueCapacity) {
                droppedCallbackEvents.fetch_add(1, std::memory_order_relaxed);
                overflowDirty.store(true, std::memory_order_release);
#ifdef _WIN32
                if (wakeEvent.valid()) SetEvent(wakeEvent.value);
#endif
                return false;
            }
            queue.push_back(RawEvent{nativeHandle, hint});
        }
#ifdef _WIN32
        if (wakeEvent.valid()) SetEvent(wakeEvent.value);
#endif
        return true;
    }

    void publishEvent(WindowChangeHint hint, std::uintptr_t nativeHandle,
                      std::optional<TrackedWindow> window) {
        ++sequence;
        WindowTrackerEvent event;
        event.sequence = sequence;
        event.hint = hint;
        event.nativeHandle = nativeHandle;
        event.window = std::move(window);
        event.droppedCallbackEvents = droppedCallbackEvents.load(std::memory_order_relaxed);
        history.push_back(std::move(event));
        while (history.size() > options.eventHistoryCapacity) history.pop_front();
    }

    static int rolePenalty(WindowRole role, WindowRole desired) noexcept {
        if (role == desired) return 0;
        switch (role) {
            case WindowRole::PrimaryGame: return 10;
            case WindowRole::InputTarget: return 11;
            case WindowRole::Launcher: return 20;
            case WindowRole::Dialog: return 30;
            case WindowRole::ChildOwnedPopup: return 40;
            case WindowRole::Overlay: return 50;
            case WindowRole::Ignored: return 1000;
        }
        return 1000;
    }

    static int initialRolePenaltyLimit(WindowRole desired,
                                       WindowTargetKind kind) noexcept {
        if (kind == WindowTargetKind::Input) {
            // A profile that asks for a distinct input sink must prove exactly that
            // role. Falling back to a visual/helper HWND would silently redirect
            // cursor/input routing to an unvalidated architecture surface.
            return 0;
        }
        if (desired == WindowRole::PrimaryGame) {
            // A launcher/splash is a legitimate bootstrap visual target, but dialogs,
            // overlays and owned popups are not promoted merely because no game HWND
            // exists yet.
            return rolePenalty(WindowRole::Launcher, desired);
        }
        return 0;
    }

    static std::uint64_t windowArea(const TrackedWindow& window) noexcept {
        const auto width = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(window.bounds.right) - window.bounds.left);
        const auto height = std::max<std::int64_t>(
            0, static_cast<std::int64_t>(window.bounds.bottom) - window.bounds.top);
        return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    }

    static CandidateRank candidateRank(const TrackedWindow& window,
                                       WindowRole desired,
                                       WindowTargetKind kind) noexcept {
        CandidateRank rank;
        rank.rolePenalty = rolePenalty(window.role, desired);
        rank.visibilityPenalty = window.visible ? 0 : 1;
        rank.ownerPenalty = window.ownerHandle == 0 ? 0 : 1;
        rank.rootPenalty = window.rootProcess ? 1 : 0;
        rank.area = windowArea(window);
        rank.processCreation = window.identity.process.creationTime100ns;
        rank.processId = window.identity.process.processId;
        rank.threadId = window.identity.threadId;
        rank.nativeHandle = window.identity.nativeHandle;
        if (kind == WindowTargetKind::Input && window.role == desired) {
            // An explicitly classified owned input sink may legitimately be hidden.
            // Its visibility therefore never outranks the exact role match.
            rank.visibilityPenalty = 0;
        }
        return rank;
    }

    static bool betterCandidate(const CandidateRank& left,
                                const CandidateRank& right) noexcept {
        if (left.rolePenalty != right.rolePenalty) {
            return left.rolePenalty < right.rolePenalty;
        }
        if (left.visibilityPenalty != right.visibilityPenalty) {
            return left.visibilityPenalty < right.visibilityPenalty;
        }
        if (left.ownerPenalty != right.ownerPenalty) {
            return left.ownerPenalty < right.ownerPenalty;
        }
        if (left.rootPenalty != right.rootPenalty) {
            return left.rootPenalty < right.rootPenalty;
        }
        if (left.area != right.area) return left.area > right.area;
        if (left.processCreation != right.processCreation) {
            return left.processCreation < right.processCreation;
        }
        if (left.processId != right.processId) return left.processId < right.processId;
        if (left.threadId != right.threadId) return left.threadId < right.threadId;
        return left.nativeHandle < right.nativeHandle;
    }

    static void advanceBindingGeneration(TargetBinding& binding) noexcept {
        ++binding.generation;
        if (binding.generation == 0) ++binding.generation;
    }

    bool ownedBySeatAuthority(const SeatTargetState& state,
                              const TrackedWindow& window) const noexcept {
        if (window.seatId != state.seatId || !window.identity.valid()) return false;
        return std::any_of(state.liveProcesses.begin(), state.liveProcesses.end(),
                           [&](const process::ProcessIdentity& identity) {
                               return identity.sameInstance(window.identity.process);
                           });
    }

    const TrackedWindow* currentCandidate(const SeatTargetState& state,
                                          const TargetBinding& binding) const noexcept {
        if (!binding.window) return nullptr;
        const auto found = std::find_if(
            windows.begin(), windows.end(), [&](const TrackedWindow& candidate) {
                return candidate.identity.sameInstance(binding.window->identity) &&
                       ownedBySeatAuthority(state, candidate);
            });
        return found == windows.end() ? nullptr : &*found;
    }

    CandidateSelection selectCandidate(const SeatTargetState& state,
                                       const TargetBinding& binding,
                                       WindowTargetKind kind,
                                       int maximumRolePenalty) const noexcept {
        CandidateSelection selection;
        CandidateRank bestRank{};
        for (const auto& candidate : windows) {
            if (!ownedBySeatAuthority(state, candidate)) continue;
            const auto rank = candidateRank(candidate, binding.desiredRole, kind);
            if (rank.rolePenalty > maximumRolePenalty) continue;

            if (kind == WindowTargetKind::Input) {
                // A distinct input target is an authority handoff boundary, not a
                // presentation preference. Once exact process ownership and the
                // requested role are proven, HydraSeat still needs one unique HWND.
                // Geometry, visibility, enumeration order, thread order and numeric
                // HWND values are not permission to choose between two matching sinks.
                if (selection.window != nullptr) {
                    selection.window = nullptr;
                    selection.ambiguous = true;
                    return selection;
                }
                selection.window = &candidate;
                continue;
            }

            if (selection.window == nullptr || betterCandidate(rank, bestRank)) {
                selection.window = &candidate;
                bestRank = rank;
            }
        }
        return selection;
    }

    WindowTargetSnapshot targetSnapshotLocked(const SeatTargetState& state,
                                               WindowTargetKind kind) const {
        const TargetBinding* binding = &state.visual;
        if (kind == WindowTargetKind::Input && state.distinctInput) {
            binding = &state.input;
        }
        WindowTargetSnapshot snapshot;
        snapshot.seatId = state.seatId;
        snapshot.kind = kind;
        snapshot.status = binding->status;
        snapshot.desiredRole = binding->desiredRole;
        snapshot.bindingGeneration = binding->generation;
        snapshot.window = binding->window;
        return snapshot;
    }

    SeatWindowTargets seatTargetsSnapshotLocked(const SeatTargetState& state) const {
        SeatWindowTargets snapshot;
        snapshot.seatId = state.seatId;
        snapshot.visual = targetSnapshotLocked(state, WindowTargetKind::Visual);
        snapshot.input = targetSnapshotLocked(state, WindowTargetKind::Input);
        snapshot.inputDistinct = state.distinctInput && snapshot.visual.window &&
                                 snapshot.input.window &&
                                 !snapshot.visual.window->identity.sameInstance(
                                     snapshot.input.window->identity);
        return snapshot;
    }

    void startReacquisition(TargetBinding& binding,
                            std::chrono::steady_clock::time_point now,
                            int maximumRolePenalty) noexcept {
        binding.status = WindowTargetStatus::Reacquiring;
        binding.window.reset();
        binding.maximumRolePenalty = maximumRolePenalty;
        binding.deadline = now + std::chrono::milliseconds(options.reacquisitionTimeoutMs);
        advanceBindingGeneration(binding);
    }

    static void failClosedBinding(TargetBinding& binding) noexcept {
        binding.status = WindowTargetStatus::FailedClosed;
        binding.window.reset();
        advanceBindingGeneration(binding);
    }

    void refreshBindingLocked(SeatTargetState& state, TargetBinding& binding,
                              WindowTargetKind kind,
                              std::chrono::steady_clock::time_point now) {
        if (!state.authorityRoot.valid()) {
            if (binding.status != WindowTargetStatus::Unresolved || binding.window) {
                binding.status = WindowTargetStatus::Unresolved;
                binding.window.reset();
                binding.maximumRolePenalty = std::numeric_limits<int>::max();
                advanceBindingGeneration(binding);
            }
            return;
        }

        if (binding.status == WindowTargetStatus::Unresolved) {
            startReacquisition(binding, now,
                               initialRolePenaltyLimit(binding.desiredRole, kind));
        }

        if (binding.status == WindowTargetStatus::Bound) {
            const auto* current = currentCandidate(state, binding);
            if (current != nullptr) {
                const int candidateLimit = kind == WindowTargetKind::Input
                    ? binding.maximumRolePenalty
                    : std::numeric_limits<int>::max();
                const auto selection = selectCandidate(state, binding, kind, candidateLimit);
                if (selection.ambiguous) {
                    failClosedBinding(binding);
                    return;
                }
                const auto* best = selection.window;
                const auto currentRank = candidateRank(*current, binding.desiredRole, kind);
                if (best != nullptr && !best->identity.sameInstance(current->identity) &&
                    betterCandidate(candidateRank(*best, binding.desiredRole, kind),
                                    currentRank)) {
                    binding.window = *best;
                    binding.maximumRolePenalty = rolePenalty(best->role, binding.desiredRole);
                    advanceBindingGeneration(binding);
                } else {
                    // Keep the logical identity stable while refreshing observed bounds,
                    // visibility/title/class data for the exact same HWND instance.
                    binding.window = *current;
                }
                return;
            }

            const int previousRolePenalty = binding.window
                ? rolePenalty(binding.window->role, binding.desiredRole)
                : std::numeric_limits<int>::max();
            startReacquisition(binding, now, previousRolePenalty);
        }

        if (binding.status == WindowTargetStatus::Reacquiring) {
            const auto selection = selectCandidate(
                state, binding, kind, binding.maximumRolePenalty);
            if (selection.ambiguous) {
                failClosedBinding(binding);
                return;
            }
            const auto* replacement = selection.window;
            if (replacement != nullptr) {
                binding.status = WindowTargetStatus::Bound;
                binding.window = *replacement;
                binding.maximumRolePenalty = rolePenalty(replacement->role,
                                                         binding.desiredRole);
                advanceBindingGeneration(binding);
                return;
            }
            if (now >= binding.deadline) {
                failClosedBinding(binding);
            }
        }
        // FailedClosed is deliberately sticky until a new process-authority epoch,
        // profile target contract, or tracker restart explicitly begins a new epoch.
    }

    std::vector<WindowTargetSnapshot> refreshTargetsLocked(
        std::chrono::steady_clock::time_point now) {
        std::vector<WindowTargetSnapshot> notifications;
        for (auto& [seatId, state] : targetStates) {
            (void)seatId;
            const auto beforeVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
            const auto beforeInput = targetSnapshotLocked(state, WindowTargetKind::Input);

            refreshBindingLocked(state, state.visual, WindowTargetKind::Visual, now);
            if (state.distinctInput) {
                refreshBindingLocked(state, state.input, WindowTargetKind::Input, now);
            } else {
                state.input = state.visual;
            }

            const auto afterVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
            const auto afterInput = targetSnapshotLocked(state, WindowTargetKind::Input);
            if (beforeVisual != afterVisual) notifications.push_back(afterVisual);
            if (beforeInput != afterInput) notifications.push_back(afterInput);
        }
        return notifications;
    }

    void dispatchTargetNotifications(
        const std::vector<WindowTargetSnapshot>& notifications) const noexcept {
        if (notifications.empty()) return;
        std::vector<std::pair<std::shared_ptr<WindowTargetObserver>, WindowTargetSnapshot>> calls;
        {
            std::lock_guard lock(observerMutex);
            observers.erase(std::remove_if(observers.begin(), observers.end(),
                                           [](const ObserverRecord& record) {
                                               return record.observer.expired();
                                           }),
                            observers.end());
            for (const auto& notification : notifications) {
                for (const auto& record : observers) {
                    if (record.seatId != notification.seatId ||
                        record.kind != notification.kind) {
                        continue;
                    }
                    if (auto observer = record.observer.lock()) {
                        calls.emplace_back(std::move(observer), notification);
                    }
                }
            }
        }
        for (const auto& [observer, notification] : calls) {
            observer->onWindowTargetChanged(notification);
        }
    }

    void refreshTargetsNow() {
        std::vector<WindowTargetSnapshot> notifications;
        {
            std::lock_guard lock(stateMutex);
            notifications = refreshTargetsLocked(std::chrono::steady_clock::now());
        }
        dispatchTargetNotifications(notifications);
    }

    static void setUnresolved(TargetBinding& binding, WindowRole desiredRole) noexcept {
        binding.desiredRole = desiredRole;
        binding.status = WindowTargetStatus::Unresolved;
        binding.window.reset();
        binding.maximumRolePenalty = std::numeric_limits<int>::max();
        advanceBindingGeneration(binding);
    }

    void resetSeatBindingsLocked(SeatTargetState& state,
                                 std::chrono::steady_clock::time_point now,
                                 bool beginReacquisition) {
        state.visual.desiredRole = visualTargetRole;
        state.distinctInput = inputTargetRole.has_value();
        state.input.desiredRole = inputTargetRole.value_or(visualTargetRole);
        if (beginReacquisition && state.authorityRoot.valid()) {
            startReacquisition(
                state.visual, now,
                initialRolePenaltyLimit(state.visual.desiredRole,
                                        WindowTargetKind::Visual));
            if (state.distinctInput) {
                startReacquisition(
                    state.input, now,
                    initialRolePenaltyLimit(state.input.desiredRole,
                                            WindowTargetKind::Input));
            } else {
                state.input = state.visual;
            }
        } else {
            setUnresolved(state.visual, visualTargetRole);
            if (state.distinctInput) {
                setUnresolved(state.input, *inputTargetRole);
            } else {
                state.input = state.visual;
            }
        }
    }

    static void markConflictedSeat(std::vector<SeatId>& conflictedSeats,
                                   SeatId seatId) {
        if (seatId == 0 ||
            std::find(conflictedSeats.begin(), conflictedSeats.end(), seatId) !=
                conflictedSeats.end()) {
            return;
        }
        conflictedSeats.push_back(seatId);
    }

    static std::vector<process::ProcessTreeSnapshot> failClosedProcessTrees(
        std::vector<process::ProcessTreeSnapshot> trees) {
        std::map<SeatId, std::size_t> seatOccurrences;
        for (const auto& tree : trees) {
            if (tree.seatId != 0 && tree.root.valid()) ++seatOccurrences[tree.seatId];
        }

        std::vector<SeatId> conflictedSeats;
        for (const auto& [seatId, occurrences] : seatOccurrences) {
            if (occurrences > 1u) markConflictedSeat(conflictedSeats, seatId);
        }

        using ProcessKey = std::pair<std::uint32_t, std::uint64_t>;
        std::map<ProcessKey, SeatId> exactOwners;
        const auto claimIdentity = [&](SeatId seatId,
                                       const process::ProcessIdentity& identity) {
            if (seatId == 0 || !identity.valid()) return;
            const ProcessKey key{identity.processId, identity.creationTime100ns};
            const auto [iterator, inserted] = exactOwners.emplace(key, seatId);
            if (!inserted && iterator->second != seatId) {
                markConflictedSeat(conflictedSeats, iterator->second);
                markConflictedSeat(conflictedSeats, seatId);
            }
        };

        for (const auto& tree : trees) {
            if (tree.seatId == 0 || !tree.root.valid()) continue;
            claimIdentity(tree.seatId, tree.root);
            for (const auto& record : tree.processes) {
                claimIdentity(tree.seatId, record.identity);
            }
        }

        trees.erase(std::remove_if(
                        trees.begin(), trees.end(),
                        [&](const process::ProcessTreeSnapshot& tree) {
                            return tree.seatId == 0 || !tree.root.valid() ||
                                   std::find(conflictedSeats.begin(), conflictedSeats.end(),
                                             tree.seatId) != conflictedSeats.end();
                        }),
                    trees.end());
        return trees;
    }

    void replaceProcessTrees(std::vector<process::ProcessTreeSnapshot> trees) {
        // A ProcessTreeSnapshot is authority input, not a hint. If two Seat snapshots
        // claim the same exact PID+creation instance (or duplicate one Seat authority),
        // choosing the first vector entry would silently transfer HWND authority by
        // enumeration order. Drop every affected Seat instead so current bindings are
        // revoked synchronously and the following rescan cannot re-adopt its windows.
        trees = failClosedProcessTrees(std::move(trees));
        {
            std::lock_guard lock(configMutex);
            processTrees = trees;
        }

        std::vector<WindowTargetSnapshot> notifications;
        const auto now = std::chrono::steady_clock::now();
        const bool beginReacquisition = running.load(std::memory_order_acquire);
        {
            std::lock_guard lock(stateMutex);
            std::vector<SeatId> seen;
            seen.reserve(trees.size());
            for (const auto& tree : trees) {
                if (tree.seatId == 0 || !tree.root.valid()) continue;
                seen.push_back(tree.seatId);
                auto& state = targetStates[tree.seatId];
                const auto beforeVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
                const auto beforeInput = targetSnapshotLocked(state, WindowTargetKind::Input);
                const bool authorityChanged =
                    state.seatId == 0 || !state.authorityRoot.sameInstance(tree.root);
                state.seatId = tree.seatId;
                state.authorityRoot = tree.root;
                state.liveProcesses.clear();
                for (const auto& record : tree.processes) {
                    if (!record.exited && record.identity.valid()) {
                        state.liveProcesses.push_back(record.identity);
                    }
                }
                if (authorityChanged) {
                    resetSeatBindingsLocked(state, now, beginReacquisition);
                }
                const auto afterVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
                const auto afterInput = targetSnapshotLocked(state, WindowTargetKind::Input);
                if (beforeVisual != afterVisual) notifications.push_back(afterVisual);
                if (beforeInput != afterInput) notifications.push_back(afterInput);
            }

            for (auto& [seatId, state] : targetStates) {
                if (std::find(seen.begin(), seen.end(), seatId) != seen.end()) continue;
                const auto beforeVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
                const auto beforeInput = targetSnapshotLocked(state, WindowTargetKind::Input);
                state.authorityRoot = {};
                state.liveProcesses.clear();
                resetSeatBindingsLocked(state, now, false);
                const auto afterVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
                const auto afterInput = targetSnapshotLocked(state, WindowTargetKind::Input);
                if (beforeVisual != afterVisual) notifications.push_back(afterVisual);
                if (beforeInput != afterInput) notifications.push_back(afterInput);
            }
            if (beginReacquisition) {
                auto refreshed = refreshTargetsLocked(now);
                notifications.insert(notifications.end(), refreshed.begin(), refreshed.end());
            }
        }
        dispatchTargetNotifications(notifications);
        (void)enqueue(0, WindowChangeHint::Rescan);
    }

    void updateTargetRules(WindowRole visualRole,
                           std::optional<WindowRole> inputRole) {
        std::vector<WindowTargetSnapshot> notifications;
        const auto now = std::chrono::steady_clock::now();
        const bool beginReacquisition = running.load(std::memory_order_acquire);
        {
            std::lock_guard lock(stateMutex);
            if (visualTargetRole == visualRole && inputTargetRole == inputRole) return;
            visualTargetRole = visualRole;
            inputTargetRole = inputRole;
            for (auto& [seatId, state] : targetStates) {
                (void)seatId;
                const auto beforeVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
                const auto beforeInput = targetSnapshotLocked(state, WindowTargetKind::Input);
                resetSeatBindingsLocked(state, now, beginReacquisition);
                const auto afterVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
                const auto afterInput = targetSnapshotLocked(state, WindowTargetKind::Input);
                if (beforeVisual != afterVisual) notifications.push_back(afterVisual);
                if (beforeInput != afterInput) notifications.push_back(afterInput);
            }
            if (beginReacquisition) {
                auto refreshed = refreshTargetsLocked(now);
                notifications.insert(notifications.end(), refreshed.begin(), refreshed.end());
            }
        }
        dispatchTargetNotifications(notifications);
    }

    void prepareTargetsForStart() {
        std::vector<WindowTargetSnapshot> notifications;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(stateMutex);
            windows.clear();
            for (auto& [seatId, state] : targetStates) {
                (void)seatId;
                const auto beforeVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
                const auto beforeInput = targetSnapshotLocked(state, WindowTargetKind::Input);
                resetSeatBindingsLocked(state, now, state.authorityRoot.valid());
                const auto afterVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
                const auto afterInput = targetSnapshotLocked(state, WindowTargetKind::Input);
                if (beforeVisual != afterVisual) notifications.push_back(afterVisual);
                if (beforeInput != afterInput) notifications.push_back(afterInput);
            }
        }
        dispatchTargetNotifications(notifications);
    }

    void invalidateTargetsForStop() {
        std::vector<WindowTargetSnapshot> notifications;
        {
            std::lock_guard lock(stateMutex);
            const auto staleWindows = windows;
            windows.clear();
            for (const auto& stale : staleWindows) {
                publishEvent(WindowChangeHint::Destroyed,
                             stale.identity.nativeHandle, stale);
            }
            for (auto& [seatId, state] : targetStates) {
                (void)seatId;
                const auto beforeVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
                const auto beforeInput = targetSnapshotLocked(state, WindowTargetKind::Input);
                resetSeatBindingsLocked(state, std::chrono::steady_clock::now(), false);
                const auto afterVisual = targetSnapshotLocked(state, WindowTargetKind::Visual);
                const auto afterInput = targetSnapshotLocked(state, WindowTargetKind::Input);
                if (beforeVisual != afterVisual) notifications.push_back(afterVisual);
                if (beforeInput != afterInput) notifications.push_back(afterInput);
            }
        }
        dispatchTargetNotifications(notifications);
    }

    bool publishOverflowIfNeeded() {
        if (!overflowDirty.exchange(false, std::memory_order_acq_rel)) return false;
        std::vector<WindowTargetSnapshot> notifications;
        {
            std::lock_guard lock(stateMutex);
            publishEvent(WindowChangeHint::Overflow, 0, std::nullopt);
            const auto staleWindows = windows;
            windows.clear();
            for (const auto& stale : staleWindows) {
                publishEvent(WindowChangeHint::Destroyed,
                             stale.identity.nativeHandle, stale);
            }
            notifications = refreshTargetsLocked(std::chrono::steady_clock::now());
        }
        dispatchTargetNotifications(notifications);
        return true;
    }

    std::optional<OwnerMatch> ownerFor(const process::ProcessIdentity& identity) const {
        std::lock_guard lock(configMutex);
        for (const auto& tree : processTrees) {
            for (const auto& record : tree.processes) {
                if (record.exited || !record.identity.sameInstance(identity)) continue;
                return OwnerMatch{tree.seatId, record.root};
            }
        }
        return std::nullopt;
    }

    WindowRole classify(const TrackedWindow& window, bool rootProcess) const {
        WindowProfileRules rules;
        {
            std::lock_guard lock(configMutex);
            rules = profileRules;
        }

#ifdef _WIN32
        for (const auto& rule : rules.overrides) {
            if (rule.rootProcessOnly && !rootProcess) continue;
            if (!rule.titleContains.empty() &&
                !containsInsensitive(window.title, rule.titleContains)) continue;
            if (!rule.classNameEquals.empty() &&
                !equalsInsensitive(window.className, rule.classNameEquals)) continue;
            if (!rule.executablePathContains.empty() &&
                !containsInsensitive(window.identity.process.executablePath,
                                     rule.executablePathContains)) continue;
            return rule.role;
        }

        // A profile that chooses Ignored as its default is an explicit
        // whitelist: no heuristic is allowed to re-adopt an unspecified helper,
        // IME, overlay, or framework window from the same controlled process.
        if (rules.defaultRole == WindowRole::Ignored) return WindowRole::Ignored;

        if (equalsInsensitive(window.className, L"#32770")) return WindowRole::Dialog;
        if (window.ownerHandle != 0) return WindowRole::ChildOwnedPopup;
        const auto hwnd = reinterpret_cast<HWND>(window.identity.nativeHandle);
        const LONG_PTR extended = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((extended & (WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW)) != 0) {
            return WindowRole::Overlay;
        }
#else
        (void)window;
        (void)rootProcess;
#endif
        return rules.defaultRole;
    }

#ifdef _WIN32
    std::optional<TrackedWindow> inspect(HWND window) const {
        if (window == nullptr || IsWindow(window) == FALSE || GetAncestor(window, GA_ROOT) != window) {
            return std::nullopt;
        }
        DWORD processId = 0;
        const DWORD threadId = GetWindowThreadProcessId(window, &processId);
        if (threadId == 0 || processId == 0) return std::nullopt;

        UniqueHandle processHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                               FALSE, processId));
        if (!processHandle.valid()) return std::nullopt;
        auto processIdentity = queryProcessIdentity(processHandle.value, processId);
        if (!processIdentity.valid()) return std::nullopt;
        const auto owner = ownerFor(processIdentity);
        if (!owner) return std::nullopt;

        TrackedWindow result;
        result.identity.nativeHandle = reinterpret_cast<std::uintptr_t>(window);
        result.identity.process = std::move(processIdentity);
        result.identity.threadId = threadId;
        result.seatId = owner->seatId;
        result.rootProcess = owner->rootProcess;
        result.ownerHandle = reinterpret_cast<std::uintptr_t>(GetWindow(window, GW_OWNER));
        result.visible = IsWindowVisible(window) != FALSE;
        RECT rectangle{};
        if (GetWindowRect(window, &rectangle) != FALSE) {
            result.bounds.left = rectangle.left;
            result.bounds.top = rectangle.top;
            result.bounds.right = rectangle.right;
            result.bounds.bottom = rectangle.bottom;
        }
        result.title = windowText(window);
        result.className = windowClass(window);
        result.role = classify(result, owner->rootProcess);
        if (result.role == WindowRole::Ignored) return std::nullopt;
        return result;
    }
#endif

    void removeTracked(std::uintptr_t nativeHandle, WindowChangeHint hint) {
        std::vector<WindowTargetSnapshot> notifications;
        {
            std::lock_guard lock(stateMutex);
            const auto found = std::find_if(windows.begin(), windows.end(),
                                            [nativeHandle](const TrackedWindow& window) {
                                                return window.identity.nativeHandle == nativeHandle;
                                            });
            if (found == windows.end()) return;
            const auto removed = *found;
            windows.erase(found);
            publishEvent(hint, nativeHandle, removed);
            notifications = refreshTargetsLocked(std::chrono::steady_clock::now());
        }
        dispatchTargetNotifications(notifications);
    }

#ifdef _WIN32
    void observe(HWND window, WindowChangeHint hint) {
        const auto handle = reinterpret_cast<std::uintptr_t>(window);
        if (hint == WindowChangeHint::Destroyed && IsWindow(window) == FALSE) {
            removeTracked(handle, hint);
            return;
        }

        auto current = inspect(window);
        if (!current) {
            // Once a previously tracked HWND is no longer a valid owned top-level
            // window, the authoritative state transition is removal. Raw WinEvent
            // ordering can report HIDE immediately before DESTROY, so preserving
            // that raw hint here would lose the host-facing Destroyed transition.
            removeTracked(handle, WindowChangeHint::Destroyed);
            return;
        }

        std::vector<WindowTargetSnapshot> notifications;
        {
            std::lock_guard lock(stateMutex);
            auto found = std::find_if(windows.begin(), windows.end(),
                                      [handle](const TrackedWindow& value) {
                                          return value.identity.nativeHandle == handle;
                                      });
            const auto sameNativeOwner = [](const WindowIdentity& left,
                                            const WindowIdentity& right) {
                return left.nativeHandle == right.nativeHandle &&
                       left.threadId == right.threadId &&
                       left.process.sameInstance(right.process);
            };
            if (found != windows.end() && !sameNativeOwner(found->identity, current->identity)) {
                const auto stale = *found;
                windows.erase(found);
                publishEvent(WindowChangeHint::Destroyed, handle, stale);
                found = windows.end();
            }
            if (found == windows.end()) {
                current->identity.trackerGeneration = nextIdentityGeneration++;
                if (nextIdentityGeneration == 0) ++nextIdentityGeneration;
                windows.push_back(*current);
                publishEvent(WindowChangeHint::Created, handle, *current);
            } else {
                current->identity.trackerGeneration = found->identity.trackerGeneration;
                if (*found != *current) {
                    const auto previous = *found;
                    *found = *current;

                    bool publishedSemanticChange = false;
                    if (previous.title != current->title) {
                        publishEvent(WindowChangeHint::TitleChanged, handle, *current);
                        publishedSemanticChange = true;
                    }
                    if (previous.bounds != current->bounds) {
                        publishEvent(WindowChangeHint::LocationChanged, handle, *current);
                        publishedSemanticChange = true;
                    }
                    if (previous.visible != current->visible) {
                        publishEvent(current->visible ? WindowChangeHint::Shown
                                                      : WindowChangeHint::Hidden,
                                     handle, *current);
                        publishedSemanticChange = true;
                    }
                    if (!publishedSemanticChange) {
                        publishEvent(hint, handle, *current);
                    }
                }
            }
            notifications = refreshTargetsLocked(std::chrono::steady_clock::now());
        }
        dispatchTargetNotifications(notifications);
    }

    void rescan() {
        std::vector<std::uintptr_t> before;
        {
            std::lock_guard lock(stateMutex);
            before.reserve(windows.size());
            for (const auto& window : windows) before.push_back(window.identity.nativeHandle);
        }

        (void)EnumWindows(initialWindowCallback, reinterpret_cast<LPARAM>(this));
        for (const auto handle : before) {
            const auto window = reinterpret_cast<HWND>(handle);
            if (IsWindow(window) == FALSE || !inspect(window)) {
                removeTracked(handle, WindowChangeHint::Destroyed);
            }
        }
        // Existing reconciliation already performs a bounded periodic rescan. Use
        // that cadence to advance reacquisition deadlines; no separate infinite
        // foreground-window polling loop is introduced.
        refreshTargetsNow();
    }
#endif

    void processQueued() {
        std::deque<RawEvent> local;
        {
            std::lock_guard lock(queueMutex);
            local.swap(queue);
#ifdef _WIN32
            if (wakeEvent.valid()) ResetEvent(wakeEvent.value);
#endif
        }
        const bool overflowed = publishOverflowIfNeeded();
#ifdef _WIN32
        if (overflowed) rescan();
        for (const auto& event : local) {
            if (event.hint == WindowChangeHint::Rescan) {
                rescan();
            } else {
                observe(reinterpret_cast<HWND>(event.nativeHandle), event.hint);
            }
        }
#else
        (void)overflowed;
        (void)local;
#endif
    }

#ifdef _WIN32
    bool installHooks(std::string& error) {
        const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        callbackOwner = this;
        lifecycleHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE,
                                        nullptr, winEventCallback, 0, 0, flags);
        if (lifecycleHook == nullptr) {
            callbackOwner = nullptr;
            error = "SetWinEventHook lifecycle range failed (Win32=" +
                    std::to_string(GetLastError()) + ")";
            return false;
        }

        propertyHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE,
                                       nullptr, winEventCallback, 0, 0, flags);
        if (propertyHook == nullptr) {
            error = "SetWinEventHook property range failed (Win32=" +
                    std::to_string(GetLastError()) + ")";
            return false;
        }
        return true;
    }

    void uninstallHooks() noexcept {
        if (propertyHook != nullptr) {
            (void)UnhookWinEvent(propertyHook);
            propertyHook = nullptr;
        }
        if (lifecycleHook != nullptr) {
            (void)UnhookWinEvent(lifecycleHook);
            lifecycleHook = nullptr;
        }
        if (callbackOwner == this) callbackOwner = nullptr;
    }
#endif

    void workerMain() {
#ifdef _WIN32
        std::string error;
        const bool installed = wakeEvent.valid() && installHooks(error);
        {
            std::lock_guard lock(readyMutex);
            setupSucceeded = installed;
            setupError = installed ? std::string{} :
                (wakeEvent.valid() ? std::move(error) : "unable to create window tracker wake event");
            ready = true;
            running.store(installed, std::memory_order_release);
        }
        readyCv.notify_all();
        if (!installed) {
            uninstallHooks();
            return;
        }

        rescan();
        auto lastReconcile = std::chrono::steady_clock::now();
        while (!stopRequested.load(std::memory_order_acquire)) {
            HANDLE handles[] = {wakeEvent.value};
            const DWORD wait = MsgWaitForMultipleObjects(1, handles, FALSE, 100u, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0 + 1u) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            processQueued();

            // WinEvent is the primary path, but some custom/toolkit windows do
            // not emit every accessibility property notification consistently.
            // Reconcile on the host worker at a bounded low rate so stale title,
            // visibility, or geometry cannot persist indefinitely.
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReconcile >= std::chrono::milliseconds(250)) {
                rescan();
                lastReconcile = now;
            }
        }
        processQueued();
        uninstallHooks();
        running.store(false, std::memory_order_release);
#else
        {
            std::lock_guard lock(readyMutex);
            setupSucceeded = false;
            setupError = "window tracking is Windows-only";
            ready = true;
        }
        readyCv.notify_all();
#endif
    }

    bool start(std::string* error) {
        if (running.load(std::memory_order_acquire)) return true;
        if (worker.joinable()) worker.join();
        stopRequested.store(false, std::memory_order_release);
        prepareTargetsForStart();
        {
            std::lock_guard lock(readyMutex);
            ready = false;
            setupSucceeded = false;
            setupError.clear();
        }
        worker = std::thread([this] { workerMain(); });
        std::unique_lock lock(readyMutex);
        if (!readyCv.wait_for(lock, std::chrono::seconds(3), [this] { return ready; })) {
            if (error) *error = "window tracker worker did not initialize";
            lock.unlock();
            stop();
            return false;
        }
        if (!setupSucceeded) {
            if (error) *error = setupError;
            lock.unlock();
            if (worker.joinable()) worker.join();
            invalidateTargetsForStop();
            return false;
        }
        return true;
    }

    void stop() noexcept {
        stopRequested.store(true, std::memory_order_release);
#ifdef _WIN32
        if (wakeEvent.valid()) SetEvent(wakeEvent.value);
#endif
        if (worker.joinable()) worker.join();
        running.store(false, std::memory_order_release);
        invalidateTargetsForStop();
    }
};

WindowTracker::WindowTracker(WindowTrackerOptions options)
    : impl_(std::make_unique<Impl>(options)) {}
WindowTracker::~WindowTracker() = default;
WindowTracker::WindowTracker(WindowTracker&&) noexcept = default;
WindowTracker& WindowTracker::operator=(WindowTracker&&) noexcept = default;

bool WindowTracker::start(std::string* error) {
    return impl_ && impl_->start(error);
}

void WindowTracker::stop() noexcept {
    if (impl_) impl_->stop();
}

bool WindowTracker::running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

void WindowTracker::setProcessTrees(std::vector<process::ProcessTreeSnapshot> trees) {
    if (!impl_) return;
    std::sort(trees.begin(), trees.end(), [](const auto& left, const auto& right) {
        return left.seatId < right.seatId;
    });
    impl_->replaceProcessTrees(std::move(trees));
}

bool WindowTracker::setProfileRules(WindowProfileRules rules, std::string* error) {
    if (!impl_) return false;
    if (rules.overrides.size() > kMaxProfileRules) {
        if (error) *error = "window profile exceeds 64 typed overrides";
        return false;
    }
    if (rules.visualTargetRole == WindowRole::Ignored ||
        (rules.inputTargetRole && *rules.inputTargetRole == WindowRole::Ignored)) {
        if (error) *error = "window target role cannot be Ignored";
        return false;
    }
    for (const auto& rule : rules.overrides) {
        if (rule.titleContains.size() > kMaxRuleTextChars ||
            rule.classNameEquals.size() > kMaxRuleTextChars ||
            rule.executablePathContains.size() > kMaxRuleTextChars) {
            if (error) *error = "window profile override text exceeds 512 characters";
            return false;
        }
        if (rule.titleContains.empty() && rule.classNameEquals.empty() &&
            rule.executablePathContains.empty() && !rule.rootProcessOnly) {
            if (error) *error = "window profile override has no typed selector";
            return false;
        }
    }
    const auto visualTargetRole = rules.visualTargetRole;
    const auto inputTargetRole = rules.inputTargetRole;
    {
        std::lock_guard lock(impl_->configMutex);
        impl_->profileRules = std::move(rules);
    }
    impl_->updateTargetRules(visualTargetRole, inputTargetRole);
    (void)impl_->enqueue(0, WindowChangeHint::Rescan);
    return true;
}

bool WindowTracker::notifyWindowChange(std::uintptr_t nativeHandle,
                                       WindowChangeHint hint) noexcept {
    return impl_ && impl_->enqueue(nativeHandle, hint);
}

WindowTrackerSnapshot WindowTracker::snapshot() const {
    WindowTrackerSnapshot result;
    if (!impl_) return result;
    std::lock_guard lock(impl_->stateMutex);
    result.sequence = impl_->sequence;
    result.droppedCallbackEvents = impl_->droppedCallbackEvents.load(std::memory_order_relaxed);
    result.windows = impl_->windows;
    result.targets.reserve(impl_->targetStates.size());
    for (const auto& [seatId, state] : impl_->targetStates) {
        (void)seatId;
        result.targets.push_back(impl_->seatTargetsSnapshotLocked(state));
    }
    std::sort(result.windows.begin(), result.windows.end(),
              [](const TrackedWindow& left, const TrackedWindow& right) {
                  if (left.seatId != right.seatId) return left.seatId < right.seatId;
                  if (left.identity.process.creationTime100ns !=
                      right.identity.process.creationTime100ns) {
                      return left.identity.process.creationTime100ns <
                             right.identity.process.creationTime100ns;
                  }
                  return left.identity.nativeHandle < right.identity.nativeHandle;
              });
    return result;
}

std::optional<WindowTargetSnapshot> WindowTracker::target(
    SeatId seatId, WindowTargetKind kind) const {
    if (!impl_ || seatId == 0) return std::nullopt;
    std::lock_guard lock(impl_->stateMutex);
    const auto found = impl_->targetStates.find(seatId);
    if (found == impl_->targetStates.end()) return std::nullopt;
    return impl_->targetSnapshotLocked(found->second, kind);
}

std::vector<WindowTrackerEvent> WindowTracker::eventsAfter(std::uint64_t afterSequence,
                                                            std::size_t maxEvents,
                                                            bool& overflow) const {
    overflow = false;
    std::vector<WindowTrackerEvent> result;
    if (!impl_ || maxEvents == 0) return result;
    std::lock_guard lock(impl_->stateMutex);
    if (impl_->history.empty()) return result;
    const auto oldest = impl_->history.front().sequence;
    if (afterSequence + 1u < oldest) {
        overflow = true;
        return result;
    }
    for (const auto& event : impl_->history) {
        if (event.sequence <= afterSequence) continue;
        if (result.size() >= maxEvents) {
            overflow = true;
            break;
        }
        result.push_back(event);
    }
    return result;
}

bool WindowTracker::validateIdentity(const WindowIdentity& identity) const noexcept {
    if (!impl_ || !identity.valid()) return false;
#ifdef _WIN32
    const auto window = reinterpret_cast<HWND>(identity.nativeHandle);
    if (IsWindow(window) == FALSE) return false;
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(window, &processId);
    if (threadId != identity.threadId || processId != identity.process.processId) return false;
    UniqueHandle processHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                           FALSE, processId));
    if (!processHandle.valid()) return false;
    const auto current = queryProcessIdentity(processHandle.value, processId);
    if (!current.sameInstance(identity.process)) return false;
    std::lock_guard lock(impl_->stateMutex);
    const auto tracked = std::find_if(
        impl_->windows.begin(), impl_->windows.end(),
        [&](const TrackedWindow& windowState) {
            return windowState.identity.nativeHandle == identity.nativeHandle;
        });
    if (tracked == impl_->windows.end() || !tracked->identity.sameInstance(identity)) {
        return false;
    }
    const auto authority = impl_->targetStates.find(tracked->seatId);
    return authority != impl_->targetStates.end() &&
           impl_->ownedBySeatAuthority(authority->second, *tracked);
#else
    return false;
#endif
}

std::uint64_t WindowTracker::addTargetObserver(
    SeatId seatId, WindowTargetKind kind,
    std::weak_ptr<WindowTargetObserver> observer) const {
    if (!impl_ || seatId == 0 || observer.expired()) return 0;
    std::lock_guard lock(impl_->observerMutex);
    auto id = impl_->nextObserverId++;
    if (id == 0) id = impl_->nextObserverId++;
    impl_->observers.push_back(Impl::ObserverRecord{id, seatId, kind, std::move(observer)});
    return id;
}

void WindowTracker::removeTargetObserver(std::uint64_t observerId) const noexcept {
    if (!impl_ || observerId == 0) return;
    std::lock_guard lock(impl_->observerMutex);
    impl_->observers.erase(
        std::remove_if(impl_->observers.begin(), impl_->observers.end(),
                       [observerId](const Impl::ObserverRecord& record) {
                           return record.id == observerId;
                       }),
        impl_->observers.end());
}

} // namespace hydra::windowing
