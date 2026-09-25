#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hydra::directinput {

inline constexpr std::size_t kMaxDirectInputNativeDevices = 64;
inline constexpr std::size_t kMaxDirectInputPolicyEntries = 32;

// Fixed-width GUID representation used by HydraSeat-owned policy/profile code.
// It intentionally does not serialize or expose the Windows GUID memory layout.
struct DirectInputInstanceId {
    std::uint32_t data1{0};
    std::uint16_t data2{0};
    std::uint16_t data3{0};
    std::array<std::uint8_t, 8> data4{};

    bool operator==(const DirectInputInstanceId&) const = default;
};

struct DirectInputDeviceDescriptor {
    DirectInputInstanceId instanceId{};
    DirectInputInstanceId productId{};
    std::uint32_t deviceType{0};
    std::uint16_t usagePage{0};
    std::uint16_t usage{0};
    std::wstring instanceName;
    std::wstring productName;

    bool operator==(const DirectInputDeviceDescriptor&) const = default;
};

// orderedInstanceIds is an allowlist and the desired process-local enumeration
// order. Unlisted native devices are hidden from the controlled policy view.
// Every listed instance is required: a missing ID fails closed rather than
// silently changing the visible controller set.
struct DirectInputVisibilityPolicy {
    std::vector<DirectInputInstanceId> orderedInstanceIds;
};

enum class DirectInputPolicyResult : std::uint32_t {
    Success = 0,
    TooManyNativeDevices = 1,
    TooManyPolicyEntries = 2,
    InvalidNativeInstanceId = 3,
    InvalidPolicyInstanceId = 4,
    DuplicateNativeInstanceId = 5,
    DuplicatePolicyInstanceId = 6,
    RequiredInstanceMissing = 7
};

bool validDirectInputInstanceId(const DirectInputInstanceId& value) noexcept;
const char* directInputPolicyResultName(DirectInputPolicyResult value) noexcept;

DirectInputPolicyResult validateDirectInputVisibilityPolicy(
    const DirectInputVisibilityPolicy& policy) noexcept;

// On every failure visibleDevices is empty. On success the output is ordered by
// policy.orderedInstanceIds, never by friendly name or native enumeration order.
DirectInputPolicyResult applyDirectInputVisibilityPolicy(
    std::span<const DirectInputDeviceDescriptor> nativeDevices,
    const DirectInputVisibilityPolicy& policy,
    std::vector<DirectInputDeviceDescriptor>& visibleDevices);

} // namespace hydra::directinput
