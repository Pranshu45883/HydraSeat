#include "hydra/directinput_policy.hpp"

#include <algorithm>
#include <utility>

namespace hydra::directinput {
namespace {

bool containsInstanceId(std::span<const DirectInputInstanceId> values,
                        const DirectInputInstanceId& wanted) noexcept {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

} // namespace

bool validDirectInputInstanceId(const DirectInputInstanceId& value) noexcept {
    if (value.data1 != 0u || value.data2 != 0u || value.data3 != 0u) {
        return true;
    }
    return std::any_of(value.data4.begin(), value.data4.end(),
                       [](std::uint8_t part) { return part != 0u; });
}

const char* directInputPolicyResultName(DirectInputPolicyResult value) noexcept {
    switch (value) {
    case DirectInputPolicyResult::Success: return "Success";
    case DirectInputPolicyResult::TooManyNativeDevices:
        return "TooManyNativeDevices";
    case DirectInputPolicyResult::TooManyPolicyEntries:
        return "TooManyPolicyEntries";
    case DirectInputPolicyResult::InvalidNativeInstanceId:
        return "InvalidNativeInstanceId";
    case DirectInputPolicyResult::InvalidPolicyInstanceId:
        return "InvalidPolicyInstanceId";
    case DirectInputPolicyResult::DuplicateNativeInstanceId:
        return "DuplicateNativeInstanceId";
    case DirectInputPolicyResult::DuplicatePolicyInstanceId:
        return "DuplicatePolicyInstanceId";
    case DirectInputPolicyResult::RequiredInstanceMissing:
        return "RequiredInstanceMissing";
    }
    return "Unknown";
}

DirectInputPolicyResult validateDirectInputVisibilityPolicy(
    const DirectInputVisibilityPolicy& policy) noexcept {
    if (policy.orderedInstanceIds.size() > kMaxDirectInputPolicyEntries) {
        return DirectInputPolicyResult::TooManyPolicyEntries;
    }

    for (std::size_t index = 0; index < policy.orderedInstanceIds.size();
         ++index) {
        const auto& id = policy.orderedInstanceIds[index];
        if (!validDirectInputInstanceId(id)) {
            return DirectInputPolicyResult::InvalidPolicyInstanceId;
        }
        const std::span<const DirectInputInstanceId> earlier(
            policy.orderedInstanceIds.data(), index);
        if (containsInstanceId(earlier, id)) {
            return DirectInputPolicyResult::DuplicatePolicyInstanceId;
        }
    }

    return DirectInputPolicyResult::Success;
}

DirectInputPolicyResult applyDirectInputVisibilityPolicy(
    std::span<const DirectInputDeviceDescriptor> nativeDevices,
    const DirectInputVisibilityPolicy& policy,
    std::vector<DirectInputDeviceDescriptor>& visibleDevices) {
    visibleDevices.clear();

    if (nativeDevices.size() > kMaxDirectInputNativeDevices) {
        return DirectInputPolicyResult::TooManyNativeDevices;
    }
    const auto policyResult = validateDirectInputVisibilityPolicy(policy);
    if (policyResult != DirectInputPolicyResult::Success) {
        return policyResult;
    }

    for (std::size_t index = 0; index < nativeDevices.size(); ++index) {
        const auto& device = nativeDevices[index];
        if (!validDirectInputInstanceId(device.instanceId)) {
            return DirectInputPolicyResult::InvalidNativeInstanceId;
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (nativeDevices[earlier].instanceId == device.instanceId) {
                return DirectInputPolicyResult::DuplicateNativeInstanceId;
            }
        }
    }

    std::vector<DirectInputDeviceDescriptor> staged;
    staged.reserve(policy.orderedInstanceIds.size());
    for (const auto& wanted : policy.orderedInstanceIds) {
        const auto found = std::find_if(
            nativeDevices.begin(), nativeDevices.end(),
            [&](const DirectInputDeviceDescriptor& device) {
                return device.instanceId == wanted;
            });
        if (found == nativeDevices.end()) {
            return DirectInputPolicyResult::RequiredInstanceMissing;
        }
        staged.push_back(*found);
    }

    visibleDevices = std::move(staged);
    return DirectInputPolicyResult::Success;
}

} // namespace hydra::directinput
