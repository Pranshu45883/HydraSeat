#pragma once

#include <cstdint>

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

} // namespace hydra::runtime
