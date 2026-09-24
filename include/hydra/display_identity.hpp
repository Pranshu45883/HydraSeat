#pragma once

#include <cstdint>
#include <string>

namespace hydra::display {

struct AdapterLuid {
    std::uint32_t lowPart{0};
    std::int32_t highPart{0};

    friend bool operator==(const AdapterLuid&, const AdapterLuid&) = default;
    friend bool operator<(const AdapterLuid& left, const AdapterLuid& right) noexcept {
        if (left.highPart != right.highPart) return left.highPart < right.highPart;
        return left.lowPart < right.lowPart;
    }

    std::string stableKey() const;
};

struct DisplayAdapterIdentity {
    AdapterLuid luid;
    std::string stableKey() const;

    friend bool operator==(const DisplayAdapterIdentity&,
                           const DisplayAdapterIdentity&) = default;
};

struct DisplayOutputIdentity {
    AdapterLuid adapterLuid;
    std::uint32_t targetId{0};
    std::wstring monitorDevicePath;

    // Stable within a topology even when Windows/DXGI enumeration order changes.
    // When a monitor device path is available it is preferred over array indices.
    std::string stableKey() const;

    bool sameTarget(const DisplayOutputIdentity& other) const noexcept {
        return adapterLuid == other.adapterLuid && targetId == other.targetId;
    }
};

} // namespace hydra::display
