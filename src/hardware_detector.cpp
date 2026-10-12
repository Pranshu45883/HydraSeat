#include "hydra/hardware_detector.hpp"
#include "hydra/controller_inventory.hpp"
#include "hydra/display_topology.hpp"
#include "hydra/hardware_identity.hpp"
#include "hydra/raw_input_utils.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <dxgi.h>
#include <xinput.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#endif

namespace hydra {

namespace {

enum class PhysicalInputRole {
    Keyboard,
    Mouse,
};

struct RawInputEndpoint {
    PhysicalInputRole role{PhysicalInputRole::Keyboard};
    uintptr_t nativeHandle{0};
    std::wstring devicePath;
    win32::DeviceInterfaceIdentity identity;
};

struct PhysicalInputGroup {
    std::wstring physicalKey;
    std::wstring displayName;
    std::vector<RawInputEndpoint> keyboards;
    std::vector<RawInputEndpoint> mice;
};

std::wstring physicalIdentityKey(
    const win32::DeviceInterfaceIdentity& identity) {
    const std::wstring_view container =
        identity.physicalContainerId
            ? std::wstring_view(*identity.physicalContainerId)
            : std::wstring_view{};
    const std::wstring_view ancestor =
        identity.physicalAncestorInstanceId
            ? std::wstring_view(*identity.physicalAncestorInstanceId)
            : (identity.parentDeviceInstanceId
                   ? std::wstring_view(*identity.parentDeviceInstanceId)
                   : std::wstring_view{});
    const std::wstring_view device =
        identity.deviceInstanceId
            ? std::wstring_view(*identity.deviceInstanceId)
            : std::wstring_view{};
    return hardware::selectPhysicalIdentity(
        container, ancestor, device, identity.interfacePath);
}

bool receiverLike(std::wstring_view name) {
    const auto normalized = hardware::normalizeDevicePath(name);
    return hardware::containsToken(normalized, L"RECEIVER") ||
           hardware::containsToken(normalized, L"DONGLE") ||
           hardware::containsToken(normalized, L"ADAPTER");
}

std::optional<PhysicalInputRole> primaryRole(
    const PhysicalInputGroup& group) {
    struct Candidate {
        std::uint16_t order;
        PhysicalInputRole role;
    };
    std::optional<Candidate> best;

    const auto consider = [&](const RawInputEndpoint& endpoint) {
        const auto protocol = endpoint.identity.usbInterfaceProtocol;
        if (protocol) {
            if (*protocol == 1 && endpoint.role != PhysicalInputRole::Keyboard) {
                return;
            }
            if (*protocol == 2 && endpoint.role != PhysicalInputRole::Mouse) {
                return;
            }
        }

        const std::uint16_t order = endpoint.identity.usbInterfaceNumber
            ? static_cast<std::uint16_t>(*endpoint.identity.usbInterfaceNumber)
            : (std::numeric_limits<std::uint16_t>::max)();
        if (!best || order < best->order) {
            best = Candidate{order, endpoint.role};
        }
    };

    for (const auto& endpoint : group.keyboards) consider(endpoint);
    for (const auto& endpoint : group.mice) consider(endpoint);
    if (!best) return std::nullopt;
    return best->role;
}

const RawInputEndpoint* representativeEndpoint(
    const std::vector<RawInputEndpoint>& endpoints) {
    if (endpoints.empty()) return nullptr;
    return &*std::min_element(
        endpoints.begin(), endpoints.end(),
        [](const RawInputEndpoint& lhs, const RawInputEndpoint& rhs) {
            return hardware::normalizeDevicePath(lhs.devicePath) <
                   hardware::normalizeDevicePath(rhs.devicePath);
        });
}

std::vector<PhysicalInputGroup> enumeratePhysicalInputGroups() {
    std::vector<PhysicalInputGroup> groups;
#ifdef _WIN32
    const auto rawDevices = win32::enumerateRawInputDevices();
    if (!rawDevices) return groups;

    std::map<std::wstring, PhysicalInputGroup> byPhysicalIdentity;
    for (const auto& rawDevice : rawDevices.devices) {
        if (rawDevice.dwType != RIM_TYPEKEYBOARD &&
            rawDevice.dwType != RIM_TYPEMOUSE) {
            continue;
        }

        const auto path = win32::rawInputDeviceName(rawDevice.hDevice);
        if (!path) continue;

        auto identity = win32::resolveDeviceInterfaceIdentity(*path);
        if (identity.syntheticOrRemote || !identity.physicalTransportProven) {
            continue;
        }

        const auto physicalKey = physicalIdentityKey(identity);
        if (physicalKey.empty()) continue;

        auto& group = byPhysicalIdentity[physicalKey];
        group.physicalKey = physicalKey;
        if (group.displayName.empty() && identity.physicalDisplayName) {
            group.displayName = *identity.physicalDisplayName;
        }

        RawInputEndpoint endpoint;
        endpoint.role = rawDevice.dwType == RIM_TYPEKEYBOARD
            ? PhysicalInputRole::Keyboard
            : PhysicalInputRole::Mouse;
        endpoint.nativeHandle =
            reinterpret_cast<uintptr_t>(rawDevice.hDevice);
        endpoint.devicePath = *path;
        endpoint.identity = std::move(identity);

        auto& endpoints = endpoint.role == PhysicalInputRole::Keyboard
            ? group.keyboards
            : group.mice;

        // Keep every top-level collection until role arbitration is done.
        // Collections collapse to one user-facing physical device later.
        endpoints.push_back(std::move(endpoint));
    }

    groups.reserve(byPhysicalIdentity.size());
    for (auto& [key, group] : byPhysicalIdentity) {
        (void)key;
        if (group.displayName.empty()) {
            group.displayName = L"Physical HID device";
        }
        groups.push_back(std::move(group));
    }
#endif
    return groups;
}

std::vector<DeviceInfo> devicesForRole(
    const std::vector<PhysicalInputGroup>& groups,
    PhysicalInputRole wantedRole) {
    std::vector<DeviceInfo> result;

    for (const auto& group : groups) {
        const auto& endpoints = wantedRole == PhysicalInputRole::Keyboard
            ? group.keyboards
            : group.mice;
        const auto* representative = representativeEndpoint(endpoints);
        if (!representative) continue;

        const bool composite =
            !group.keyboards.empty() && !group.mice.empty();
        const auto primary = primaryRole(group);
        const bool needsActivity =
            composite &&
            (receiverLike(group.displayName) ||
             !primary ||
             *primary != wantedRole);

        DeviceInfo info;
        info.type = wantedRole == PhysicalInputRole::Keyboard
            ? DeviceType::Keyboard
            : DeviceType::Mouse;
        info.devicePath = representative->devicePath;
        for (const auto& endpoint : endpoints) {
            if (endpoint.nativeHandle == 0) continue;
            if (std::find(
                    info.nativeHandles.begin(),
                    info.nativeHandles.end(),
                    endpoint.nativeHandle) == info.nativeHandles.end()) {
                info.nativeHandles.push_back(endpoint.nativeHandle);
            }
        }
        if (!info.nativeHandles.empty()) {
            info.nativeHandle = info.nativeHandles.front();
        }
        info.id = win32::makeStableRawInputDeviceId(
            wantedRole == PhysicalInputRole::Keyboard
                ? L"keyboard"
                : L"mouse",
            representative->identity);
        if (info.id.empty()) continue;
        info.name = group.displayName;
        if (wantedRole == PhysicalInputRole::Keyboard &&
            hardware::isLikelyInternalKeyboardPath(
                representative->devicePath)) {
            info.name = L"Internal Keyboard";
        } else if (wantedRole == PhysicalInputRole::Mouse &&
                   hardware::isLikelyTouchpadPath(
                       representative->devicePath)) {
            info.name = L"Touchpad";
        }
        info.requiresActivityConfirmation = needsActivity;
        result.push_back(std::move(info));
    }

    std::sort(
        result.begin(), result.end(),
        [](const DeviceInfo& lhs, const DeviceInfo& rhs) {
            if (lhs.requiresActivityConfirmation !=
                rhs.requiresActivityConfirmation) {
                return !lhs.requiresActivityConfirmation;
            }
            if (lhs.name != rhs.name) return lhs.name < rhs.name;
            return lhs.id < rhs.id;
        });
    return result;
}

} // namespace

std::vector<DeviceInfo> HardwareDetector::detectDisplays() {
    std::vector<DeviceInfo> result;

#ifdef _WIN32
    // Seat v1 targets physical monitors. DisplayConfig + DXGI gives us the
    // target identity/transport evidence that EnumDisplayDevices alone cannot:
    // virtual/remote/indirect outputs can also be attached to the desktop.
    display::DisplayTopologyInventory topologyInventory;
    const auto topology = topologyInventory.refresh();
    if (!topology.querySucceeded) return result;

    for (const auto& output : topology.outputs) {
        if (!output.active || !output.attached ||
            output.virtualLikelihood !=
                display::VirtualDisplayLikelihood::PhysicalLikely ||
            output.gdiDeviceName.empty()) {
            continue;
        }

        const auto stableKey = output.identity.stableKey();
        DeviceInfo info;
        info.id.assign(stableKey.begin(), stableKey.end());
        info.name = !output.friendlyName.empty()
            ? output.friendlyName
            : (!output.dxgiAdapterDescription.empty()
                   ? output.dxgiAdapterDescription
                   : output.gdiDeviceName);
        info.devicePath = output.gdiDeviceName;
        info.type = DeviceType::Display;
        result.push_back(std::move(info));
    }

    std::sort(
        result.begin(), result.end(),
        [](const DeviceInfo& lhs, const DeviceInfo& rhs) {
            return lhs.id < rhs.id;
        });
#endif

    return result;
}

std::vector<DeviceInfo> HardwareDetector::detectKeyboards() {
#ifdef _WIN32
    const auto groups = enumeratePhysicalInputGroups();
    return devicesForRole(groups, PhysicalInputRole::Keyboard);
#else
    return {};
#endif
}

std::vector<DeviceInfo> HardwareDetector::detectMice() {
#ifdef _WIN32
    const auto groups = enumeratePhysicalInputGroups();
    return devicesForRole(groups, PhysicalInputRole::Mouse);
#else
    return {};
#endif
}

std::vector<DeviceInfo> HardwareDetector::detectControllers() {
    std::vector<DeviceInfo> result;

    const auto inventory = controller::scanControllerSources();
    if (!inventory.authoritative) return result;

    result.reserve(inventory.physicalControllers.size());
    for (const auto& controllerInfo : inventory.physicalControllers) {
        DeviceInfo info;
        info.id = controllerInfo.persistentId;
        info.name = controllerInfo.displayName;
        info.devicePath = controllerInfo.devicePath;
        info.type = DeviceType::Controller;
        result.push_back(std::move(info));
    }

    return result;
}

void HardwareDetector::printReport() {
    std::wcout << L"===========================================\n";
    std::wcout << L"       HydraSeat Hardware Report           \n";
    std::wcout << L"===========================================\n\n";

    auto displays = detectDisplays();
    std::wcout << L"[Displays Found: " << displays.size() << L"]\n";
    for (const auto& d : displays) {
        std::wcout << L"  - " << d.name << L" (" << d.id << L")\n";
    }

    auto keyboards = detectKeyboards();
    std::wcout << L"\n[Keyboards Found: " << keyboards.size() << L"]\n";
    for (const auto& k : keyboards) {
        std::wcout << L"  - " << k.name << L"\n";
        if (!k.devicePath.empty()) {
            std::wcout << L"    Path: " << k.devicePath << L"\n";
        }
    }

    auto mice = detectMice();
    std::wcout << L"\n[Mice / Touchpads Found: " << mice.size() << L"]\n";
    for (const auto& m : mice) {
        std::wcout << L"  - " << m.name << L"\n";
        if (!m.devicePath.empty()) {
            std::wcout << L"    Path: " << m.devicePath << L"\n";
        }
    }

    auto controllers = detectControllers();
    std::wcout << L"\n[Controllers Found: " << controllers.size() << L"]\n";
    for (const auto& c : controllers) {
        std::wcout << L"  - " << c.name << L"\n";
    }

    std::wcout << L"\n===========================================\n";
}

} // namespace hydra
