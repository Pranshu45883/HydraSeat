#pragma once

#ifdef _WIN32

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::win32 {

struct RawInputDeviceListResult {
    std::vector<RAWINPUTDEVICELIST> devices;
    DWORD error{ERROR_SUCCESS};

    explicit operator bool() const noexcept { return error == ERROR_SUCCESS; }
};

struct DeviceInterfaceIdentity {
    std::wstring interfacePath;
    std::optional<std::wstring> deviceInstanceId;
    std::optional<std::wstring> parentDeviceInstanceId;
    std::optional<std::wstring> physicalAncestorInstanceId;
    std::optional<std::wstring> physicalContainerId;
    std::vector<std::wstring> ancestorInstanceIds;
    std::optional<std::wstring> physicalDisplayName;
    std::optional<std::uint8_t> usbInterfaceNumber;
    std::optional<std::uint8_t> usbInterfaceProtocol;
    bool syntheticOrRemote{false};
    bool physicalTransportProven{false};
};

RawInputDeviceListResult enumerateRawInputDevices();
std::optional<std::wstring> rawInputDeviceName(HANDLE deviceHandle);
std::optional<RID_DEVICE_INFO> rawInputDeviceInfo(HANDLE deviceHandle);
DeviceInterfaceIdentity resolveDeviceInterfaceIdentity(std::wstring_view interfacePath);
std::wstring makeStableRawInputDeviceId(
    std::wstring_view category, const DeviceInterfaceIdentity& identity);
std::wstring makeStableRawInputDeviceId(
    std::wstring_view category, std::wstring_view interfacePath);

} // namespace hydra::win32

#endif
