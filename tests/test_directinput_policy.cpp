#include "hydra/directinput_policy.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using hydra::directinput::DirectInputDeviceDescriptor;
using hydra::directinput::DirectInputInstanceId;
using hydra::directinput::DirectInputPolicyResult;
using hydra::directinput::DirectInputVisibilityPolicy;
using hydra::directinput::applyDirectInputVisibilityPolicy;
using hydra::directinput::kMaxDirectInputNativeDevices;
using hydra::directinput::kMaxDirectInputPolicyEntries;
using hydra::directinput::validateDirectInputVisibilityPolicy;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

DirectInputInstanceId id(std::uint32_t value) {
    DirectInputInstanceId result;
    result.data1 = value;
    result.data2 = static_cast<std::uint16_t>(value >> 16u);
    result.data3 = static_cast<std::uint16_t>(value ^ 0x55aau);
    result.data4 = {
        static_cast<std::uint8_t>(value & 0xffu),
        static_cast<std::uint8_t>((value >> 8u) & 0xffu),
        1u, 2u, 3u, 4u, 5u, 6u};
    return result;
}

DirectInputDeviceDescriptor device(std::uint32_t instance,
                                   std::wstring_view name = L"Controller") {
    DirectInputDeviceDescriptor result;
    result.instanceId = id(instance);
    result.productId = id(instance + 1000u);
    result.deviceType = 4u;
    result.usagePage = 1u;
    result.usage = 5u;
    result.instanceName = name;
    result.productName = L"Hydra synthetic game controller";
    return result;
}

void testDeterministicAllowlistOrder() {
    check(sizeof(DirectInputInstanceId) == 16u,
          "DirectInput instance identity stays a fixed 16-byte value on x86 and x64");
    const auto a = device(1u, L"Same friendly name");
    auto b = device(2u, L"Same friendly name");
    b.productId = a.productId;
    const auto c = device(3u, L"Third controller");
    std::vector<DirectInputDeviceDescriptor> native{a, b, c};
    DirectInputVisibilityPolicy policy{{c.instanceId, a.instanceId}};
    std::vector<DirectInputDeviceDescriptor> visible;

    check(applyDirectInputVisibilityPolicy(native, policy, visible) ==
              DirectInputPolicyResult::Success &&
              visible == std::vector<DirectInputDeviceDescriptor>{c, a},
          "policy allowlist controls both visibility and deterministic order");

    std::reverse(native.begin(), native.end());
    visible.clear();
    check(applyDirectInputVisibilityPolicy(native, policy, visible) ==
              DirectInputPolicyResult::Success &&
              visible == std::vector<DirectInputDeviceDescriptor>{c, a},
          "native enumeration order does not change the policy view");
    check(visible[0].instanceName != L"" &&
              a.instanceName == b.instanceName && a.productId == b.productId &&
              a.instanceId != b.instanceId && visible[0].instanceId != b.instanceId,
          "friendly/product identity remains metadata; instance GUID alone selects the device instance");
}

void testTwoIndependentControlledViews() {
    const auto a = device(11u);
    const auto b = device(12u);
    const auto c = device(13u);
    const std::vector<DirectInputDeviceDescriptor> native{a, b, c};
    const DirectInputVisibilityPolicy seatA{{c.instanceId, a.instanceId}};
    const DirectInputVisibilityPolicy seatB{{b.instanceId}};
    std::vector<DirectInputDeviceDescriptor> first;
    std::vector<DirectInputDeviceDescriptor> second;

    check(applyDirectInputVisibilityPolicy(native, seatA, first) ==
              DirectInputPolicyResult::Success &&
              applyDirectInputVisibilityPolicy(native, seatB, second) ==
              DirectInputPolicyResult::Success,
          "two controlled views can apply independent allowlists");
    check(first.size() == 2u && first[0].instanceId == c.instanceId &&
              first[1].instanceId == a.instanceId && second.size() == 1u &&
              second[0].instanceId == b.instanceId,
          "controlled views expose only their declared instance IDs");
    check(std::none_of(first.begin(), first.end(), [&](const auto& entry) {
              return entry.instanceId == b.instanceId;
          }) &&
              std::none_of(second.begin(), second.end(), [&](const auto& entry) {
                  return entry.instanceId == a.instanceId ||
                         entry.instanceId == c.instanceId;
              }),
          "controlled views contain no cross-policy controller visibility");
}

void testFailureIsCanonicalAndFailClosed() {
    const auto a = device(21u);
    const auto b = device(22u);
    const std::vector<DirectInputDeviceDescriptor> native{a, b};
    std::vector<DirectInputDeviceDescriptor> visible{a};

    DirectInputVisibilityPolicy missing{{a.instanceId, id(999u)}};
    check(applyDirectInputVisibilityPolicy(native, missing, visible) ==
              DirectInputPolicyResult::RequiredInstanceMissing &&
              visible.empty(),
          "a missing required instance fails closed with no partial view");

    DirectInputVisibilityPolicy duplicate{{a.instanceId, a.instanceId}};
    visible = {b};
    check(validateDirectInputVisibilityPolicy(duplicate) ==
              DirectInputPolicyResult::DuplicatePolicyInstanceId &&
              applyDirectInputVisibilityPolicy(native, duplicate, visible) ==
              DirectInputPolicyResult::DuplicatePolicyInstanceId &&
              visible.empty(),
          "duplicate policy instance IDs are rejected without output");

    DirectInputVisibilityPolicy invalid{{DirectInputInstanceId{}}};
    visible = {b};
    check(applyDirectInputVisibilityPolicy(native, invalid, visible) ==
              DirectInputPolicyResult::InvalidPolicyInstanceId &&
              visible.empty(),
          "zero policy GUID is rejected without output");

    auto invalidNative = native;
    invalidNative[0].instanceId = {};
    visible = {b};
    check(applyDirectInputVisibilityPolicy(invalidNative,
                                            DirectInputVisibilityPolicy{},
                                            visible) ==
              DirectInputPolicyResult::InvalidNativeInstanceId &&
              visible.empty(),
          "zero native instance GUID is rejected even for an empty view");

    auto duplicateNative = native;
    duplicateNative.push_back(a);
    visible = {b};
    check(applyDirectInputVisibilityPolicy(duplicateNative,
                                            DirectInputVisibilityPolicy{},
                                            visible) ==
              DirectInputPolicyResult::DuplicateNativeInstanceId &&
              visible.empty(),
          "ambiguous duplicate native instance IDs fail closed");
}

void testBoundsAndEmptyPolicy() {
    std::vector<DirectInputDeviceDescriptor> visible;
    check(applyDirectInputVisibilityPolicy(
              std::vector<DirectInputDeviceDescriptor>{},
              DirectInputVisibilityPolicy{}, visible) ==
              DirectInputPolicyResult::Success &&
              visible.empty(),
          "empty native inventory and empty allowlist are a valid zero-device view");

    std::vector<DirectInputDeviceDescriptor> native;
    native.reserve(kMaxDirectInputNativeDevices);
    DirectInputVisibilityPolicy maximumPolicy;
    maximumPolicy.orderedInstanceIds.reserve(kMaxDirectInputPolicyEntries);
    for (std::size_t index = 0; index < kMaxDirectInputNativeDevices; ++index) {
        const auto value = static_cast<std::uint32_t>(100u + index);
        native.push_back(device(value));
        if (index < kMaxDirectInputPolicyEntries) {
            maximumPolicy.orderedInstanceIds.push_back(native.back().instanceId);
        }
    }

    check(applyDirectInputVisibilityPolicy(native, maximumPolicy, visible) ==
              DirectInputPolicyResult::Success &&
              visible.size() == kMaxDirectInputPolicyEntries,
          "declared native and policy maximums are accepted");

    auto tooManyNative = native;
    tooManyNative.push_back(device(10000u));
    check(applyDirectInputVisibilityPolicy(tooManyNative,
                                            DirectInputVisibilityPolicy{},
                                            visible) ==
              DirectInputPolicyResult::TooManyNativeDevices &&
              visible.empty(),
          "native inventory above the bound is rejected");

    auto tooManyPolicy = maximumPolicy;
    tooManyPolicy.orderedInstanceIds.push_back(id(20000u));
    check(validateDirectInputVisibilityPolicy(tooManyPolicy) ==
              DirectInputPolicyResult::TooManyPolicyEntries,
          "policy above the bound is rejected");

    check(applyDirectInputVisibilityPolicy(native,
                                            DirectInputVisibilityPolicy{},
                                            visible) ==
              DirectInputPolicyResult::Success &&
              visible.empty(),
          "empty allowlist intentionally exposes no DirectInput devices");
}

} // namespace

int main() {
    testDeterministicAllowlistOrder();
    testTwoIndependentControlledViews();
    testFailureIsCanonicalAndFailClosed();
    testBoundsAndEmptyPolicy();
    std::cout << "DirectInput visibility policy tests passed\n";
    return EXIT_SUCCESS;
}
