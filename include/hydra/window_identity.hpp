#pragma once

#include "hydra/process_group.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::windowing {

enum class WindowRole : std::uint8_t {
    PrimaryGame = 0,
    Launcher = 1,
    Dialog = 2,
    Overlay = 3,
    ChildOwnedPopup = 4,
    InputTarget = 5,
    Ignored = 6,
};

enum class WindowTargetKind : std::uint8_t {
    Visual = 0,
    Input = 1,
};

enum class WindowTargetStatus : std::uint8_t {
    Unresolved = 0,
    Bound = 1,
    Reacquiring = 2,
    FailedClosed = 3,
};

struct WindowIdentity {
    std::uintptr_t nativeHandle{0};
    process::ProcessIdentity process;
    std::uint32_t threadId{0};
    std::uint64_t trackerGeneration{0};

    bool valid() const noexcept {
        return nativeHandle != 0 && threadId != 0 && trackerGeneration != 0 &&
               process.valid();
    }

    bool sameInstance(const WindowIdentity& other) const noexcept {
        return nativeHandle == other.nativeHandle &&
               threadId == other.threadId &&
               trackerGeneration == other.trackerGeneration &&
               process.sameInstance(other.process);
    }

    friend bool operator==(const WindowIdentity&, const WindowIdentity&) = default;
};

struct WindowRule {
    WindowRole role{WindowRole::PrimaryGame};
    std::wstring titleContains;
    std::wstring classNameEquals;
    std::wstring executablePathContains;
    bool rootProcessOnly{false};
};

struct WindowProfileRules {
    WindowRole defaultRole{WindowRole::PrimaryGame};
    std::vector<WindowRule> overrides;
    // Classification may use profile title/class/path selectors only after exact
    // process-tree ownership is proven. These role preferences rank already-owned
    // candidates; they are never ownership authority.
    WindowRole visualTargetRole{WindowRole::PrimaryGame};
    // When absent, the input target mirrors the validated visual target exactly.
    // Profiles with a distinct owned input sink can opt into another explicit role
    // without making title/class text an ownership credential. A distinct input role
    // must resolve to one exact Seat-owned HWND; multiple matching owned candidates
    // are ambiguous and fail closed rather than being chosen by geometry, focus,
    // enumeration order, or the numeric HWND value.
    std::optional<WindowRole> inputTargetRole;
};

std::string_view windowRoleName(WindowRole role) noexcept;

} // namespace hydra::windowing
