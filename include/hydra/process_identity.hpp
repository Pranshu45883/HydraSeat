#pragma once

#include <cstdint>
#include <optional>

namespace hydra::runtime {

// Exact runtime process identity. PID alone is insufficient because Windows can
// reuse process IDs; creationIdentity is the process creation FILETIME encoded
// as a 64-bit value.
struct ProcessIdentity {
    std::uint32_t pid{0};
    std::uint64_t creationIdentity{0};

    bool valid() const noexcept {
        return pid != 0 && creationIdentity != 0;
    }

    bool operator==(const ProcessIdentity&) const = default;
};

enum class ProcessOwnershipMatch {
    Match,
    Mismatch,
    Unknown
};

inline ProcessOwnershipMatch matchIdentity(
    const std::optional<ProcessIdentity>& observed,
    const ProcessIdentity& expected) noexcept
{
    if (!observed.has_value() || !observed->valid() || !expected.valid()) {
        return ProcessOwnershipMatch::Unknown;
    }
    return *observed == expected
        ? ProcessOwnershipMatch::Match
        : ProcessOwnershipMatch::Mismatch;
}

} // namespace hydra::runtime
