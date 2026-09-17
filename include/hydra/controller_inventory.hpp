#pragma once

#include "hydra/controller_identity.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hydra::controller {

struct PhysicalControllerDescriptor {
    std::wstring persistentId;
    std::wstring displayName;
    std::wstring devicePath;
    std::uint16_t vendorId{0};
    std::uint16_t productId{0};

    bool operator==(const PhysicalControllerDescriptor&) const = default;
};

struct InventorySnapshot {
    bool authoritative{false};
    std::vector<SourceDescriptor> sources;
    std::vector<PhysicalControllerDescriptor> physicalControllers;
    std::string error;
};

// XInput user indices are exposed only as runtime/session identities. Stable
// physical controller identity is reported separately from Windows PnP data.
InventorySnapshot scanControllerSources() noexcept;

} // namespace hydra::controller
