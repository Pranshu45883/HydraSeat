#include "hydra/directinput_policy.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <windows.h>
#include <dinput.h>
#include <wrl/client.h>
#endif

namespace {

using hydra::directinput::DirectInputDeviceDescriptor;
using hydra::directinput::DirectInputInstanceId;
using hydra::directinput::DirectInputPolicyResult;
using hydra::directinput::DirectInputVisibilityPolicy;
using hydra::directinput::applyDirectInputVisibilityPolicy;
using hydra::directinput::directInputPolicyResultName;
using hydra::directinput::kMaxDirectInputNativeDevices;

DirectInputInstanceId syntheticId(std::uint32_t value) {
    DirectInputInstanceId result;
    result.data1 = value;
    result.data2 = static_cast<std::uint16_t>(value >> 16u);
    result.data3 = static_cast<std::uint16_t>(0x4000u | (value & 0x0fffu));
    result.data4 = {
        static_cast<std::uint8_t>(value & 0xffu),
        static_cast<std::uint8_t>((value >> 8u) & 0xffu),
        0x80u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u};
    return result;
}

DirectInputDeviceDescriptor syntheticDevice(std::uint32_t value,
                                            std::wstring_view name) {
    DirectInputDeviceDescriptor result;
    result.instanceId = syntheticId(value);
    result.productId = syntheticId(value + 1000u);
    result.deviceType = 4u;
    result.usagePage = 1u;
    result.usage = 5u;
    result.instanceName = name;
    result.productName = L"HydraSeat controlled DirectInput fixture";
    return result;
}

int runControlledProbe(std::string_view seat) {
    const auto a = syntheticDevice(1u, L"Duplicate friendly name");
    const auto b = syntheticDevice(2u, L"Duplicate friendly name");
    const auto c = syntheticDevice(3u, L"Third fixture controller");
    const std::vector<DirectInputDeviceDescriptor> native{a, b, c};

    DirectInputVisibilityPolicy policy;
    std::vector<DirectInputInstanceId> expected;
    std::vector<DirectInputInstanceId> forbidden;
    if (seat == "seat-a") {
        policy.orderedInstanceIds = {c.instanceId, a.instanceId};
        expected = {c.instanceId, a.instanceId};
        forbidden = {b.instanceId};
    } else if (seat == "seat-b") {
        policy.orderedInstanceIds = {b.instanceId};
        expected = {b.instanceId};
        forbidden = {a.instanceId, c.instanceId};
    } else {
        std::cerr << "unknown controlled seat selector\n";
        return 2;
    }

    std::vector<DirectInputDeviceDescriptor> visible;
    const auto result = applyDirectInputVisibilityPolicy(native, policy, visible);
    if (result != DirectInputPolicyResult::Success) {
        std::cerr << "controlled DirectInput policy failed: "
                  << directInputPolicyResultName(result) << '\n';
        return 3;
    }
    if (visible.size() != expected.size()) {
        std::cerr << "controlled DirectInput visible count mismatch\n";
        return 4;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (visible[index].instanceId != expected[index]) {
            std::cerr << "controlled DirectInput order mismatch\n";
            return 5;
        }
    }

    const auto crossVisible = static_cast<std::size_t>(std::count_if(
        visible.begin(), visible.end(), [&](const auto& entry) {
            return std::find(forbidden.begin(), forbidden.end(),
                             entry.instanceId) != forbidden.end();
        }));
    if (crossVisible != 0u) {
        std::cerr << "controlled DirectInput cross-policy visibility detected\n";
        return 6;
    }

    std::cout << "{\"event\":\"p3_ctrl_02_controlled\",\"seat\":\""
              << seat << "\",\"native_count\":" << native.size()
              << ",\"visible_count\":" << visible.size()
              << ",\"cross_visible\":" << crossVisible << "}\n";
    return EXIT_SUCCESS;
}

#ifdef _WIN32

DirectInputInstanceId normalizeGuid(const GUID& value) noexcept {
    DirectInputInstanceId result;
    result.data1 = static_cast<std::uint32_t>(value.Data1);
    result.data2 = static_cast<std::uint16_t>(value.Data2);
    result.data3 = static_cast<std::uint16_t>(value.Data3);
    for (std::size_t index = 0; index < result.data4.size(); ++index) {
        result.data4[index] = static_cast<std::uint8_t>(value.Data4[index]);
    }
    return result;
}

std::wstring boundedWideString(const wchar_t* value, std::size_t capacity) {
    if (value == nullptr) {
        return {};
    }
    std::size_t length = 0;
    while (length < capacity && value[length] != L'\0') {
        ++length;
    }
    return std::wstring(value, length);
}

struct NativeEnumerationState {
    std::vector<DirectInputDeviceDescriptor> devices;
    bool failed{false};
};

BOOL CALLBACK enumDeviceCallback(const DIDEVICEINSTANCEW* instance,
                                 void* reference) noexcept {
    auto* state = static_cast<NativeEnumerationState*>(reference);
    if (state == nullptr || instance == nullptr ||
        instance->dwSize < sizeof(DIDEVICEINSTANCEW) ||
        state->devices.size() >= kMaxDirectInputNativeDevices) {
        if (state != nullptr) {
            state->failed = true;
        }
        return DIENUM_STOP;
    }

    try {
        DirectInputDeviceDescriptor descriptor;
        descriptor.instanceId = normalizeGuid(instance->guidInstance);
        descriptor.productId = normalizeGuid(instance->guidProduct);
        descriptor.deviceType = static_cast<std::uint32_t>(instance->dwDevType);
        descriptor.usagePage = static_cast<std::uint16_t>(instance->wUsagePage);
        descriptor.usage = static_cast<std::uint16_t>(instance->wUsage);
        descriptor.instanceName = boundedWideString(
            instance->tszInstanceName, MAX_PATH);
        descriptor.productName = boundedWideString(
            instance->tszProductName, MAX_PATH);
        state->devices.push_back(std::move(descriptor));
    } catch (...) {
        state->failed = true;
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

int runNativeObservationSelfTest() {
    Microsoft::WRL::ComPtr<IDirectInput8W> directInput;
    const HRESULT createResult = DirectInput8Create(
        GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
        reinterpret_cast<void**>(directInput.GetAddressOf()), nullptr);
    if (FAILED(createResult) || directInput == nullptr) {
        std::cerr << "DirectInput8Create failed: 0x" << std::hex
                  << static_cast<unsigned long>(createResult) << std::dec << '\n';
        return 10;
    }

    NativeEnumerationState state;
    state.devices.reserve(kMaxDirectInputNativeDevices);
    const HRESULT enumResult = directInput->EnumDevices(
        DI8DEVCLASS_GAMECTRL, enumDeviceCallback, &state, DIEDFL_ATTACHEDONLY);
    if (FAILED(enumResult) || state.failed) {
        std::cerr << "IDirectInput8::EnumDevices failed or exceeded bounds: 0x"
                  << std::hex << static_cast<unsigned long>(enumResult)
                  << std::dec << '\n';
        return 11;
    }

    DirectInputVisibilityPolicy policy;
    if (!state.devices.empty()) {
        policy.orderedInstanceIds.push_back(state.devices.front().instanceId);
    }
    if (state.devices.size() >= 2u) {
        policy.orderedInstanceIds.insert(
            policy.orderedInstanceIds.begin(), state.devices[1].instanceId);
    }

    std::vector<DirectInputDeviceDescriptor> visible;
    const auto policyResult = applyDirectInputVisibilityPolicy(
        state.devices, policy, visible);
    if (policyResult != DirectInputPolicyResult::Success ||
        visible.size() != policy.orderedInstanceIds.size()) {
        std::cerr << "native DirectInput inventory failed policy validation: "
                  << directInputPolicyResultName(policyResult) << '\n';
        return 12;
    }

    std::cout << "{\"event\":\"p3_ctrl_02_native_observation\","
              << "\"attached_game_controllers\":" << state.devices.size()
              << ",\"controlled_visible\":" << visible.size() << "}\n";
    return EXIT_SUCCESS;
}

#endif

void printUsage() {
    std::cout
        << "HydraSeat controlled DirectInput probe\n"
        << "  --controlled-self-test seat-a|seat-b\n"
#ifdef _WIN32
        << "  --native-observation-self-test\n"
#endif
        ;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--controlled-self-test") {
        return runControlledProbe(argv[2]);
    }
#ifdef _WIN32
    if (argc == 2 &&
        std::string_view(argv[1]) == "--native-observation-self-test") {
        return runNativeObservationSelfTest();
    }
#endif
    printUsage();
    return argc == 1 ? EXIT_SUCCESS : 1;
}
