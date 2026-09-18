#pragma once

#include <cstdint>
#include <optional>

namespace hydra::runtime {

// A PID alone is not an ownership identity because Windows may reuse it.
// creationIdentity is the process creation timestamp/token observed by the host.
struct ProcessIdentity {
    std::uint32_t pid{0};
    std::uint64_t creationIdentity{0};

    bool valid() const noexcept {
        return pid != 0 && creationIdentity != 0;
    }

    bool operator==(const ProcessIdentity&) const = default;
};

// Represents the pure result of comparing an observed identity to an expected identity.
enum class ProcessOwnershipMatch {
    Match,
    Mismatch,
    Unknown
};

// Pure helper to compare an observed identity with an expected one.
inline ProcessOwnershipMatch matchIdentity(
    const std::optional<ProcessIdentity>& observed, 
    const ProcessIdentity& expected) noexcept 
{
    if (!observed) return ProcessOwnershipMatch::Unknown;
    if (observed->pid != expected.pid) return ProcessOwnershipMatch::Mismatch;
    if (observed->creationIdentity != expected.creationIdentity) return ProcessOwnershipMatch::Mismatch;
    return ProcessOwnershipMatch::Match;
}

} // namespace hydra::runtime
