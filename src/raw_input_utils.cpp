#include "hydra/raw_input_utils.hpp"

#ifdef _WIN32

#include "hydra/hardware_identity.hpp"

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <iomanip>
#include <setupapi.h>
#include <sstream>
#include <utility>
#include <vector>

namespace hydra::win32 {

namespace {

constexpr UINT kMaxDeviceNameCharacters = 32768;
constexpr int kDeviceNameQueryAttempts = 3;

class DeviceInfoSet final {
public:
    explicit DeviceInfoSet(HDEVINFO handle) noexcept : handle_(handle) {}
    ~DeviceInfoSet() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(handle_);
        }
    }

    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

    HDEVINFO get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HDEVINFO handle_{INVALID_HANDLE_VALUE};
};

std::optional<std::wstring> setupApiInstanceId(
    HDEVINFO deviceInfoSet, SP_DEVINFO_DATA& deviceInfoData) {
    DWORD requiredCharacters = 0;
    if (!SetupDiGetDeviceInstanceIdW(
            deviceInfoSet, &deviceInfoData, nullptr, 0, &requiredCharacters) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return std::nullopt;
    }
    if (requiredCharacters == 0 || requiredCharacters > kMaxDeviceNameCharacters) {
        return std::nullopt;
    }

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(requiredCharacters) + 1, L'\0');
    if (!SetupDiGetDeviceInstanceIdW(deviceInfoSet, &deviceInfoData, buffer.data(),
                                     static_cast<DWORD>(buffer.size()), nullptr)) {
        return std::nullopt;
    }
    return hardware::trimTrailingNulls(std::wstring(buffer.data()));
}

std::optional<std::wstring> configManagerInstanceId(DEVINST deviceInstance) {
    ULONG idCharacters = 0;
    if (CM_Get_Device_ID_Size(&idCharacters, deviceInstance, 0) != CR_SUCCESS ||
        idCharacters == 0 || idCharacters >= kMaxDeviceNameCharacters) {
        return std::nullopt;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(idCharacters) + 1, L'\0');
    if (CM_Get_Device_IDW(deviceInstance, buffer.data(),
                          static_cast<ULONG>(buffer.size()), 0) != CR_SUCCESS) {
        return std::nullopt;
    }
    return hardware::trimTrailingNulls(std::wstring(buffer.data()));
}

std::optional<std::wstring> configManagerStringProperty(
    DEVINST deviceInstance,
    const DEVPROPKEY& key) {
    DEVPROPTYPE propertyType = 0;
    ULONG propertyBytes = 0;
    CONFIGRET result = CM_Get_DevNode_PropertyW(
        deviceInstance, &key, &propertyType, nullptr, &propertyBytes, 0);
    if (result != CR_BUFFER_SMALL || propertyBytes < sizeof(wchar_t) ||
        propertyBytes > kMaxDeviceNameCharacters * sizeof(wchar_t)) {
        return std::nullopt;
    }

    const auto characterCapacity =
        static_cast<std::size_t>(
            (propertyBytes + sizeof(wchar_t) - 1u) / sizeof(wchar_t)) + 1u;
    std::vector<wchar_t> buffer(characterCapacity, L'\0');
    result = CM_Get_DevNode_PropertyW(
        deviceInstance, &key, &propertyType,
        reinterpret_cast<PBYTE>(buffer.data()), &propertyBytes, 0);
    if (result != CR_SUCCESS ||
        (propertyType != DEVPROP_TYPE_STRING &&
         propertyType != DEVPROP_TYPE_STRING_INDIRECT)) {
        return std::nullopt;
    }

    const auto terminator =
        std::find(buffer.cbegin(), buffer.cend(), L'\0');
    if (terminator == buffer.cend()) return std::nullopt;
    auto value = hardware::trimTrailingNulls(
        std::wstring(buffer.cbegin(), terminator));
    return value.empty() ? std::nullopt
                         : std::optional<std::wstring>{std::move(value)};
}

std::vector<std::wstring> configManagerStringListProperty(
    DEVINST deviceInstance,
    const DEVPROPKEY& key) {
    DEVPROPTYPE propertyType = 0;
    ULONG propertyBytes = 0;
    CONFIGRET result = CM_Get_DevNode_PropertyW(
        deviceInstance, &key, &propertyType, nullptr, &propertyBytes, 0);
    if (result != CR_BUFFER_SMALL || propertyBytes < sizeof(wchar_t) * 2 ||
        propertyBytes > kMaxDeviceNameCharacters * sizeof(wchar_t)) {
        return {};
    }

    const auto characterCapacity =
        static_cast<std::size_t>(
            (propertyBytes + sizeof(wchar_t) - 1u) / sizeof(wchar_t)) + 1u;
    std::vector<wchar_t> buffer(characterCapacity, L'\0');
    result = CM_Get_DevNode_PropertyW(
        deviceInstance, &key, &propertyType,
        reinterpret_cast<PBYTE>(buffer.data()), &propertyBytes, 0);
    if (result != CR_SUCCESS || propertyType != DEVPROP_TYPE_STRING_LIST) {
        return {};
    }

    std::vector<std::wstring> values;
    const auto propertyCharacters =
        std::min<std::size_t>(
            propertyBytes / sizeof(wchar_t), buffer.size());
    auto current = buffer.cbegin();
    const auto end = buffer.cbegin() +
        static_cast<std::ptrdiff_t>(propertyCharacters);
    while (current < end && *current != L'\0') {
        const auto terminator = std::find(current, end, L'\0');
        if (terminator == end) return {};
        values.emplace_back(current, terminator);
        current = terminator + 1;
    }
    return values;
}

std::optional<std::uint8_t> parseUsbInterfaceNumber(
    std::wstring_view instanceId) {
    const auto normalized = hardware::canonicalizeInstanceId(instanceId);
    const auto marker = normalized.find(L"&MI_");
    if (marker == std::wstring::npos || marker + 6 > normalized.size()) {
        return std::nullopt;
    }

    const auto hexValue = [](wchar_t ch) -> int {
        if (ch >= L'0' && ch <= L'9') return ch - L'0';
        if (ch >= L'A' && ch <= L'F') return 10 + (ch - L'A');
        return -1;
    };
    const int high = hexValue(normalized[marker + 4]);
    const int low = hexValue(normalized[marker + 5]);
    if (high < 0 || low < 0) return std::nullopt;
    return static_cast<std::uint8_t>((high << 4) | low);
}

std::optional<std::uint8_t> usbHidBootProtocol(DEVINST deviceInstance) {
    const auto compatibleIds = configManagerStringListProperty(
        deviceInstance, DEVPKEY_Device_CompatibleIds);
    for (const auto& compatibleId : compatibleIds) {
        const auto normalized = hardware::canonicalizeInstanceId(compatibleId);
        if (!hardware::containsToken(normalized, L"CLASS_03") ||
            !hardware::containsToken(normalized, L"SUBCLASS_01")) {
            continue;
        }
        if (hardware::containsToken(normalized, L"PROT_01")) {
            return static_cast<std::uint8_t>(1);
        }
        if (hardware::containsToken(normalized, L"PROT_02")) {
            return static_cast<std::uint8_t>(2);
        }
    }
    return std::nullopt;
}

bool hasPhysicalTransportPrefix(std::wstring_view instanceId) {
    const auto normalized = hardware::canonicalizeInstanceId(instanceId);
    return normalized.starts_with(L"USB\\") ||
           normalized.starts_with(L"ACPI\\") ||
           normalized.starts_with(L"BTHENUM\\") ||
           normalized.starts_with(L"BTHLEDEVICE\\") ||
           normalized.starts_with(L"I2C\\");
}

bool isSyntheticTransportIdentity(std::wstring_view instanceId) {
    const auto normalized = hardware::canonicalizeInstanceId(instanceId);
    return hardware::isObviousRemoteOrSyntheticInputPath(normalized) ||
           normalized.starts_with(L"ROOT\\") ||
           normalized.starts_with(L"SWD\\") ||
           hardware::containsToken(normalized, L"VIGEM") ||
           hardware::containsToken(normalized, L"VJOY") ||
           hardware::containsToken(normalized, L"FEIZHI_VIRTUAL");
}

bool isNullGuid(const GUID& value) noexcept {
    if (value.Data1 != 0 || value.Data2 != 0 || value.Data3 != 0) {
        return false;
    }
    for (const auto byte : value.Data4) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::wstring formatGuid(const GUID& value) {
    std::wostringstream output;
    output << L'{' << std::uppercase << std::hex << std::setfill(L'0')
           << std::setw(8) << value.Data1 << L'-'
           << std::setw(4) << value.Data2 << L'-'
           << std::setw(4) << value.Data3 << L'-'
           << std::setw(2) << static_cast<unsigned int>(value.Data4[0])
           << std::setw(2) << static_cast<unsigned int>(value.Data4[1]) << L'-'
           << std::setw(2) << static_cast<unsigned int>(value.Data4[2])
           << std::setw(2) << static_cast<unsigned int>(value.Data4[3])
           << std::setw(2) << static_cast<unsigned int>(value.Data4[4])
           << std::setw(2) << static_cast<unsigned int>(value.Data4[5])
           << std::setw(2) << static_cast<unsigned int>(value.Data4[6])
           << std::setw(2) << static_cast<unsigned int>(value.Data4[7]) << L'}';
    return output.str();
}

std::optional<std::wstring> configManagerContainerId(DEVINST deviceInstance) {
    GUID value{};
    DEVPROPTYPE propertyType = 0;
    ULONG propertyBytes = sizeof(value);
    if (CM_Get_DevNode_PropertyW(
            deviceInstance, &DEVPKEY_Device_ContainerId, &propertyType,
            reinterpret_cast<PBYTE>(&value), &propertyBytes, 0) != CR_SUCCESS ||
        propertyType != DEVPROP_TYPE_GUID || propertyBytes != sizeof(value) ||
        isNullGuid(value)) {
        return std::nullopt;
    }
    return formatGuid(value);
}

bool isPhysicalUsbDeviceInstance(std::wstring_view instanceId) {
    const auto normalized = hardware::canonicalizeInstanceId(instanceId);
    return normalized.starts_with(L"USB\\") &&
           hardware::containsToken(normalized, L"VID_") &&
           hardware::containsToken(normalized, L"PID_") &&
           !hardware::containsToken(normalized, L"&MI_");
}

struct PhysicalAncestorIdentity {
    std::optional<std::wstring> instanceId;
    std::optional<std::wstring> containerId;
    DEVINST deviceInstance{0};
};

PhysicalAncestorIdentity resolvePhysicalAncestor(DEVINST deviceInstance) {
    constexpr int kMaxAncestorDepth = 12;
    DEVINST current = deviceInstance;
    for (int depth = 0; depth < kMaxAncestorDepth; ++depth) {
        const auto instanceId = configManagerInstanceId(current);
        if (instanceId && isPhysicalUsbDeviceInstance(*instanceId)) {
            return {*instanceId, configManagerContainerId(current), current};
        }

        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS || parent == current) {
            break;
        }
        current = parent;
    }
    return {};
}

} // namespace

RawInputDeviceListResult enumerateRawInputDevices() {
    UINT deviceCount = 0;
    if (GetRawInputDeviceList(nullptr, &deviceCount, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) {
        return {{}, GetLastError()};
    }

    if (deviceCount == 0) {
        return {};
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<RAWINPUTDEVICELIST> devices(deviceCount);
        UINT capacity = deviceCount;
        const UINT returned = GetRawInputDeviceList(
            devices.data(), &capacity, sizeof(RAWINPUTDEVICELIST));

        if (returned != static_cast<UINT>(-1)) {
            devices.resize(returned);
            return {std::move(devices), ERROR_SUCCESS};
        }

        const DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER || capacity <= deviceCount) {
            return {{}, error};
        }
        deviceCount = capacity;
    }

    return {{}, ERROR_INSUFFICIENT_BUFFER};
}

std::optional<std::wstring> rawInputDeviceName(HANDLE deviceHandle) {
    if (deviceHandle == nullptr) {
        return std::nullopt;
    }

    // Device arrival/removal can invalidate the size between the two Win32
    // calls. Re-query a bounded number of times instead of using a stale size.
    for (int attempt = 0; attempt < kDeviceNameQueryAttempts; ++attempt) {
        UINT requiredCharacters = 0;
        if (GetRawInputDeviceInfoW(deviceHandle, RIDI_DEVICENAME, nullptr,
                                   &requiredCharacters) == static_cast<UINT>(-1)) {
            return std::nullopt;
        }
        if (requiredCharacters == 0 || requiredCharacters > kMaxDeviceNameCharacters) {
            return std::nullopt;
        }

        // Keep one known-zero slot beyond the API-reported requirement. Do not
        // interpret the generic byte-oriented return value as a wchar_t count.
        std::vector<wchar_t> buffer(
            static_cast<std::size_t>(requiredCharacters) + 1, L'\0');
        UINT capacity = static_cast<UINT>(buffer.size());
        const UINT returned = GetRawInputDeviceInfoW(
            deviceHandle, RIDI_DEVICENAME, buffer.data(), &capacity);
        if (returned == static_cast<UINT>(-1)) {
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                continue;
            }
            return std::nullopt;
        }
        if (returned == 0) {
            return std::nullopt;
        }

        const auto terminator = std::find(buffer.cbegin(), buffer.cend(), L'\0');
        if (terminator == buffer.cend()) {
            return std::nullopt;
        }
        auto name = hardware::trimTrailingNulls(
            std::wstring(buffer.cbegin(), terminator));
        if (name.empty()) {
            return std::nullopt;
        }
        return name;
    }

    return std::nullopt;
}

std::optional<RID_DEVICE_INFO> rawInputDeviceInfo(HANDLE deviceHandle) {
    if (deviceHandle == nullptr) {
        return std::nullopt;
    }

    RID_DEVICE_INFO info{};
    info.cbSize = sizeof(info);
    UINT infoSize = sizeof(info);
    if (GetRawInputDeviceInfoW(deviceHandle, RIDI_DEVICEINFO, &info, &infoSize) ==
        static_cast<UINT>(-1)) {
        return std::nullopt;
    }
    return info;
}

DeviceInterfaceIdentity resolveDeviceInterfaceIdentity(std::wstring_view interfacePath) {
    DeviceInterfaceIdentity result;
    result.interfacePath = hardware::trimTrailingNulls(std::wstring(interfacePath));
    if (result.interfacePath.empty()) {
        return result;
    }

    DeviceInfoSet deviceInfoSet(SetupDiCreateDeviceInfoList(nullptr, nullptr));
    if (!deviceInfoSet) {
        return result;
    }

    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);
    if (!SetupDiOpenDeviceInterfaceW(deviceInfoSet.get(), result.interfacePath.c_str(),
                                     0, &interfaceData)) {
        return result;
    }

    SP_DEVINFO_DATA deviceInfoData{};
    deviceInfoData.cbSize = sizeof(deviceInfoData);
    DWORD requiredSize = 0;
    if (SetupDiGetDeviceInterfaceDetailW(
            deviceInfoSet.get(), &interfaceData, nullptr, 0, &requiredSize,
            nullptr) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        requiredSize < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
        return result;
    }

    std::vector<std::byte> detailStorage(requiredSize);
    auto* detailData = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
        detailStorage.data());
    detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(
            deviceInfoSet.get(), &interfaceData, detailData, requiredSize,
            nullptr, &deviceInfoData)) {
        return result;
    }

    result.deviceInstanceId = setupApiInstanceId(deviceInfoSet.get(), deviceInfoData);
    result.syntheticOrRemote =
        hardware::isObviousRemoteOrSyntheticInputPath(result.interfacePath);

    // Keep the complete devnode ancestry. Raw Input exposes HID top-level
    // collections, while the user cares about the physical transport device
    // behind those collections. Walking ancestry also lets us reject virtual
    // ROOT/SWD devices even when their child HID path looks ordinary.
    DEVINST current = deviceInfoData.DevInst;
    constexpr int kMaxAncestorDepth = 16;
    for (int depth = 0; depth < kMaxAncestorDepth; ++depth) {
        const auto currentId = configManagerInstanceId(current);
        if (currentId) {
            result.ancestorInstanceIds.push_back(*currentId);

            // Once the chain has reached a proven local transport (USB, ACPI,
            // Bluetooth or I2C), higher system/root-bus devnodes no longer
            // describe whether the input peripheral itself is virtual.
            if (!result.physicalTransportProven) {
                result.syntheticOrRemote =
                    result.syntheticOrRemote ||
                    isSyntheticTransportIdentity(*currentId);
            }
            result.physicalTransportProven =
                result.physicalTransportProven ||
                hasPhysicalTransportPrefix(*currentId);
        }

        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS ||
            parent == current) {
            break;
        }
        current = parent;
    }

    const auto physicalAncestor = resolvePhysicalAncestor(deviceInfoData.DevInst);
    result.physicalAncestorInstanceId = physicalAncestor.instanceId;
    result.physicalContainerId = physicalAncestor.containerId;
    if (physicalAncestor.deviceInstance != 0) {
        result.physicalTransportProven = true;
        result.physicalDisplayName = configManagerStringProperty(
            physicalAncestor.deviceInstance, DEVPKEY_Device_BusReportedDeviceDesc);
        if (!result.physicalDisplayName) {
            result.physicalDisplayName = configManagerStringProperty(
                physicalAncestor.deviceInstance, DEVPKEY_Device_DeviceDesc);
        }
    }

    DEVINST parentInstance = 0;
    if (CM_Get_Parent(&parentInstance, deviceInfoData.DevInst, 0) == CR_SUCCESS) {
        result.parentDeviceInstanceId = configManagerInstanceId(parentInstance);
        if (result.parentDeviceInstanceId) {
            result.usbInterfaceNumber =
                parseUsbInterfaceNumber(*result.parentDeviceInstanceId);
        }
        result.usbInterfaceProtocol = usbHidBootProtocol(parentInstance);

        if (!result.physicalContainerId) {
            result.physicalContainerId = configManagerContainerId(parentInstance);
        }
        if (!result.physicalDisplayName) {
            result.physicalDisplayName = configManagerStringProperty(
                parentInstance, DEVPKEY_Device_BusReportedDeviceDesc);
        }
    }
    if (!result.physicalContainerId) {
        result.physicalContainerId = configManagerContainerId(deviceInfoData.DevInst);
    }
    if (!result.physicalDisplayName) {
        result.physicalDisplayName = configManagerStringProperty(
            deviceInfoData.DevInst, DEVPKEY_Device_FriendlyName);
    }
    if (!result.physicalDisplayName) {
        result.physicalDisplayName = configManagerStringProperty(
            deviceInfoData.DevInst, DEVPKEY_Device_DeviceDesc);
    }

    return result;
}

std::wstring makeStableRawInputDeviceId(
    std::wstring_view category, const DeviceInterfaceIdentity& identity) {
    const std::wstring_view containerId = identity.physicalContainerId
                                              ? *identity.physicalContainerId
                                              : std::wstring_view{};
    const std::wstring_view physicalAncestorId = identity.physicalAncestorInstanceId
                                                     ? *identity.physicalAncestorInstanceId
                                                     : std::wstring_view{};
    const std::wstring_view parentId = identity.parentDeviceInstanceId
                                           ? *identity.parentDeviceInstanceId
                                           : std::wstring_view{};
    const std::wstring_view selectedAncestorId = !physicalAncestorId.empty()
                                                     ? physicalAncestorId
                                                     : parentId;
    const std::wstring_view deviceId = identity.deviceInstanceId
                                           ? *identity.deviceInstanceId
                                           : std::wstring_view{};
    return hardware::makeStableDeviceId(
        category, containerId, selectedAncestorId, deviceId, identity.interfacePath);
}

std::wstring makeStableRawInputDeviceId(
    std::wstring_view category, std::wstring_view interfacePath) {
    return makeStableRawInputDeviceId(
        category, resolveDeviceInterfaceIdentity(interfacePath));
}

} // namespace hydra::win32

#endif
