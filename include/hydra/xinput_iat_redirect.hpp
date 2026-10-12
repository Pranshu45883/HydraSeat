#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hydra::controller {

enum class XInputRedirectStatus : std::uint8_t {
    Success = 0,
    AlreadyInstalled = 1,
    MissingImport = 2,
    UnsupportedImport = 3,
    InvalidImage = 4,
    PatchFailure = 5,
    RollbackFailure = 6,
    UnsupportedPlatform = 7,
};

struct XInputReplacementSet {
    std::uintptr_t getState{0};
    std::uintptr_t setState{0};
    std::uintptr_t getCapabilities{0};

    bool valid() const noexcept {
        return getState != 0 && setState != 0 && getCapabilities != 0;
    }
};

struct XInputRedirectReport {
    XInputRedirectStatus status{XInputRedirectStatus::InvalidImage};
    std::uint32_t discoveredFunctionMask{0};
    std::size_t patchedSlotCount{0};
    bool rollbackComplete{true};
    std::uint32_t systemError{0};
    std::string error;

    explicit operator bool() const noexcept {
        return status == XInputRedirectStatus::Success ||
               status == XInputRedirectStatus::AlreadyInstalled;
    }
};

// Redirects statically imported XInput calls in the target executable to the
// already-loaded HydraSeat process-local adapter. The redirect is deliberately
// narrow: unsupported XInput imports, ordinal imports, delay/dynamic-only usage,
// malformed PE metadata, or partial patching fail closed.
class XInputIatRedirect final {
public:
    XInputIatRedirect() = default;
    ~XInputIatRedirect();

    XInputIatRedirect(const XInputIatRedirect&) = delete;
    XInputIatRedirect& operator=(const XInputIatRedirect&) = delete;
    XInputIatRedirect(XInputIatRedirect&&) = delete;
    XInputIatRedirect& operator=(XInputIatRedirect&&) = delete;

    XInputRedirectReport install(
        const XInputReplacementSet& replacements) noexcept;
    XInputRedirectReport uninstall() noexcept;

    bool installed() const noexcept { return installed_; }

private:
    struct Slot {
        std::uintptr_t* address{nullptr};
        std::uintptr_t original{0};
        std::uintptr_t replacement{0};
        std::uint8_t functionIndex{0};
    };

    std::vector<Slot> slots_;
    bool installed_{false};
};

} // namespace hydra::controller
