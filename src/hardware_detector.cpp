#include "hydra/hardware_detector.hpp"
#include "hydra/controller_inventory.hpp"
#include "hydra/display_topology.hpp"
#include "hydra/hardware_identity.hpp"
#include "hydra/hid_usage.hpp"
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
    bool requiresTransportConfirmation{false};
};

std::wstring physicalIdentityKey(
    const win32::DeviceInterfaceIdentity& identity) {
    // Grouping must use EXACTLY the same physical key as the host's input
    // activity and Seat binding IDs. A shared ContainerId (particularly on
    // ACPI/I2C laptops) is not sufficient to merge separate keyboard inputs.
    return win32::makeStableRawInputDeviceId(L"physical", identity);
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
        if (identity.syntheticOrRemote) continue;
        // Some wired USB keyboards temporarily lack SetupAPI ancestry during
        // hotplug. Keep a narrowly identified HID VID/PID candidate, but never
        // expose it for Seat assignment until this Raw Input ID produces real
        // input activity. Generic/virtual/ambiguous paths stay excluded.
        const bool provisional =
            !identity.physicalTransportProven &&
            hardware::isProvisionalUsbHidInputPath(*path);
        if (!identity.physicalTransportProven && !provisional) continue;

        const auto physicalKey = physicalIdentityKey(identity);
        if (physicalKey.empty()) continue;

        auto& group = byPhysicalIdentity[physicalKey];
        group.physicalKey = physicalKey;
        group.requiresTransportConfirmation |= provisional;
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
            group.requiresTransportConfirmation ||
            (composite &&
             (receiverLike(group.displayName) ||
              !primary ||
              *primary != wantedRole));

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
        const auto hasTouchpadEvidence = [&]() {
            if (hardware::isLikelyTouchpadPath(representative->devicePath) ||
                hardware::isLikelyTouchpadPath(group.displayName)) {
                return true;
            }
            for (const auto& instance : representative->identity.ancestorInstanceIds) {
                if (hardware::isLikelyTouchpadPath(instance)) return true;
            }
            return false;
        };
        if (wantedRole == PhysicalInputRole::Keyboard &&
            hardware::isLikelyInternalKeyboardPath(
                representative->devicePath)) {
            info.name = L"Internal Keyboard";
        } else if (wantedRole == PhysicalInputRole::Mouse &&
                   hasTouchpadEvidence()) {
            info.name = L"Touchpad";
        }
        info.requiresActivityConfirmation = needsActivity;
        info.requiresFreshTransportConfirmation =
            group.requiresTransportConfirmation;
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

// Digitizer Page 0x0D / Usage 0x05 is a Precision Touchpad HID sensor,
// not the Generic Desktop mouse collection (Page 0x01 / Usage 0x02).
// Enumerate it for the Hardware page ONLY. Windows can translate its movement
// into mouse events with hDevice == nullptr, which cannot be assigned to a Seat.
std::vector<DeviceInfo> HardwareDetector::detectTouchpadSensors() {
    std::vector<DeviceInfo> sensors;
#ifdef _WIN32
    const auto rawDevices = win32::enumerateRawInputDevices();
    if (!rawDevices) return sensors;

    std::map<std::wstring, DeviceInfo> byId;
    for (const auto& device : rawDevices.devices) {
        if (device.dwType != RIM_TYPEHID) continue;
        const auto details = win32::rawInputDeviceInfo(device.hDevice);
        if (!details || details->dwType != RIM_TYPEHID ||
            hid::classifyCollection(details->hid.usUsagePage,
                                    details->hid.usUsage) != hid::CollectionKind::Touchpad) {
            continue;
        }
        const auto path = win32::rawInputDeviceName(device.hDevice);
        if (!path) continue;
        const auto identity = win32::resolveDeviceInterfaceIdentity(*path);
        if (identity.syntheticOrRemote ||
            hardware::isObviousRemoteOrSyntheticInputPath(*path)) {
            continue;
        }
        const auto stableId =
            win32::makeStableRawInputDeviceId(L"touchpad-sensor", identity);
        if (stableId.empty()) continue;

        auto [position, inserted] = byId.try_emplace(stableId);
        if (inserted) {
            auto& info = position->second;
            info.id = stableId;
            info.type = DeviceType::Mouse;
            info.name = L"Precision Touchpad (HID sensor only)";
            info.devicePath = *path;
            info.nativeHandle = reinterpret_cast<uintptr_t>(device.hDevice);
            // This must never enter the canonical assignable mouse inventory.
            info.requiresActivityConfirmation = true;
        }
    }
    sensors.reserve(byId.size());
    for (auto& [id, device] : byId) {
        (void)id;
        sensors.push_back(std::move(device));
    }
#endif
    return sensors;
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
