#include "hydra/controller_inventory.hpp"

#include <array>
#include <cwchar>
#include <cwctype>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <initguid.h>
#include <devpkey.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <xinput.h>
#endif

namespace hydra::controller {
namespace {

std::wstring canonicalPersistentId(std::wstring value) {
    for (auto& ch : value) ch = static_cast<wchar_t>(std::towupper(ch));
    return value;
}

#if defined(_WIN32)

std::wstring formatContainerId(const GUID& value) {
    wchar_t buffer[64]{};
    std::swprintf(
        buffer, std::size(buffer),
        L"container:{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        static_cast<unsigned long>(value.Data1),
        static_cast<unsigned int>(value.Data2),
        static_cast<unsigned int>(value.Data3),
        static_cast<unsigned int>(value.Data4[0]),
        static_cast<unsigned int>(value.Data4[1]),
        static_cast<unsigned int>(value.Data4[2]),
        static_cast<unsigned int>(value.Data4[3]),
        static_cast<unsigned int>(value.Data4[4]),
        static_cast<unsigned int>(value.Data4[5]),
        static_cast<unsigned int>(value.Data4[6]),
        static_cast<unsigned int>(value.Data4[7]));
    return buffer;
}

std::wstring readStringProperty(HDEVINFO deviceSet,
                                SP_DEVINFO_DATA& deviceInfo,
                                const DEVPROPKEY& key) {
    std::array<wchar_t, 512> buffer{};
    DEVPROPTYPE type = 0;
    DWORD required = 0;
    if (!SetupDiGetDevicePropertyW(
            deviceSet, &deviceInfo, &key, &type,
            reinterpret_cast<PBYTE>(buffer.data()),
            static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
            &required, 0)) {
        return {};
    }
    if (type != DEVPROP_TYPE_STRING && type != DEVPROP_TYPE_STRING_INDIRECT) {
        return {};
    }
    return buffer.data();
}

bool readContainerId(HDEVINFO deviceSet,
                     SP_DEVINFO_DATA& deviceInfo,
                     GUID& containerId) noexcept {
    DEVPROPTYPE type = 0;
    DWORD required = 0;
    if (!SetupDiGetDevicePropertyW(
            deviceSet, &deviceInfo, &DEVPKEY_Device_ContainerId, &type,
            reinterpret_cast<PBYTE>(&containerId), sizeof(containerId),
            &required, 0)) {
        return false;
    }
    return type == DEVPROP_TYPE_GUID && required == sizeof(containerId);
}

bool describeGameController(const wchar_t* devicePath,
                            std::uint16_t& vendorId,
                            std::uint16_t& productId) noexcept {
    HANDLE handle = CreateFileW(
        devicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    PHIDP_PREPARSED_DATA preparsed = nullptr;
    HIDP_CAPS caps{};
    HIDD_ATTRIBUTES attributes{};
    attributes.Size = sizeof(attributes);

    const bool havePreparsed = HidD_GetPreparsedData(handle, &preparsed) != FALSE;
    const bool haveCaps = havePreparsed &&
        HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
    const bool haveAttributes = HidD_GetAttributes(handle, &attributes) != FALSE;

    if (preparsed != nullptr) HidD_FreePreparsedData(preparsed);
    CloseHandle(handle);

    if (!haveCaps || caps.UsagePage != 0x01 ||
        (caps.Usage != 0x04 && caps.Usage != 0x05 && caps.Usage != 0x08)) {
        return false;
    }

    if (haveAttributes) {
        vendorId = attributes.VendorID;
        productId = attributes.ProductID;
    }
    return true;
}

bool appendPhysicalControllers(std::vector<PhysicalControllerDescriptor>& output,
                               std::string& error) {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceSet = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceSet == INVALID_HANDLE_VALUE) {
        error = "SetupDiGetClassDevsW failed for HID controller inventory";
        return false;
    }

    std::set<std::wstring> seenContainers;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(
                deviceSet, nullptr, &hidGuid, index, &interfaceData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            SetupDiDestroyDeviceInfoList(deviceSet);
            error = "SetupDiEnumDeviceInterfaces failed for HID controller inventory";
            return false;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(
            deviceSet, &interfaceData, nullptr, 0, &required, nullptr);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;

        std::vector<std::byte> storage(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA deviceInfo{};
        deviceInfo.cbSize = sizeof(deviceInfo);

        if (!SetupDiGetDeviceInterfaceDetailW(
                deviceSet, &interfaceData, detail, required, nullptr, &deviceInfo)) {
            continue;
        }

        std::uint16_t vendorId = 0;
        std::uint16_t productId = 0;
        if (!describeGameController(detail->DevicePath, vendorId, productId)) continue;

        GUID containerId{};
        if (!readContainerId(deviceSet, deviceInfo, containerId)) continue;
        const auto persistentId = formatContainerId(containerId);
        if (!seenContainers.insert(persistentId).second) continue;

        auto displayName = readStringProperty(
            deviceSet, deviceInfo, DEVPKEY_Device_FriendlyName);
        if (displayName.empty()) {
            displayName = readStringProperty(
                deviceSet, deviceInfo, DEVPKEY_Device_DeviceDesc);
        }
        if (displayName.empty()) displayName = L"Game controller";

        output.push_back({persistentId, displayName, detail->DevicePath,
                          vendorId, productId});
    }

    SetupDiDestroyDeviceInfoList(deviceSet);
    return true;
}

#endif

} // namespace

InventorySnapshot ControllerInventory::scan() noexcept {
    InventorySnapshot snapshot;

#if defined(_WIN32)
    try {
        snapshot.sources.reserve(kXInputSlotCount);
        for (std::uint8_t slot = 0; slot < kXInputSlotCount; ++slot) {
            XINPUT_STATE state{};
            const DWORD result = XInputGetState(static_cast<DWORD>(slot), &state);

            SourceDescriptor source;
            source.runtimeKey = "xinput-slot:" + std::to_string(slot);
            source.displayName = L"XInput runtime slot " + std::to_wstring(slot);
            source.api = ApiSurface::XInput;
            source.identityQuality = IdentityQuality::RuntimeOnly;
            source.runtimeXInputSlot = slot;
            source.connected = (result == ERROR_SUCCESS);

            if (!seenSlots_[slot]) {
                seenSlots_[slot] = true;
                previousConnected_[slot] = source.connected;
                generations_[slot] = source.connected ? 1u : 0u;
            } else if (previousConnected_[slot] != source.connected) {
                previousConnected_[slot] = source.connected;
                ++generations_[slot];
            }
            source.sourceGeneration = generations_[slot];
            snapshot.sources.push_back(std::move(source));
        }

        if (!appendPhysicalControllers(snapshot.physicalControllers, snapshot.error)) {
            snapshot.sources.clear();
            snapshot.physicalControllers.clear();
            return snapshot;
        }

        snapshot.authoritative = true;
        snapshot.error.clear();
        return snapshot;
    } catch (...) {
        snapshot.sources.clear();
        snapshot.physicalControllers.clear();
        snapshot.error = "controller inventory allocation failed";
        return snapshot;
    }
#else
    snapshot.error = "native Windows controller inventory is unavailable on this platform";
    return snapshot;
#endif
}

InventorySnapshot scanControllerSources() noexcept {
    ControllerInventory inventory;
    return inventory.scan();
}

PairingResult pairPhysicalControllerToXInput(
    std::uint32_t seatId,
    const std::wstring& persistentControllerId,
    std::uint8_t runtimeSlot,
    const InventorySnapshot& inventory) noexcept {
    if (seatId != 1 && seatId != 2) {
        return {PairingStatus::InvalidSeat, std::nullopt};
    }
    if (persistentControllerId.empty()) {
        return {PairingStatus::InvalidPersistentId, std::nullopt};
    }
    if (runtimeSlot >= kXInputSlotCount) {
        return {PairingStatus::RuntimeSlotOutOfRange, std::nullopt};
    }

    const auto wanted = canonicalPersistentId(persistentControllerId);
    const PhysicalControllerDescriptor* physical = nullptr;
    std::size_t physicalMatches = 0;
    for (const auto& candidate : inventory.physicalControllers) {
        if (canonicalPersistentId(candidate.persistentId) != wanted) continue;
        ++physicalMatches;
        physical = &candidate;
    }
    if (physicalMatches == 0) {
        return {PairingStatus::PhysicalControllerNotFound, std::nullopt};
    }
    if (physicalMatches != 1 || physical == nullptr) {
        return {PairingStatus::AmbiguousPhysicalController, std::nullopt};
    }

    const SourceDescriptor* runtime = nullptr;
    std::size_t runtimeMatches = 0;
    for (const auto& source : inventory.sources) {
        if (source.api != ApiSurface::XInput || !source.runtimeXInputSlot ||
            *source.runtimeXInputSlot != runtimeSlot) {
            continue;
        }
        ++runtimeMatches;
        runtime = &source;
    }
    if (runtimeMatches != 1 || runtime == nullptr) {
        return {PairingStatus::RuntimeSourceNotFound, std::nullopt};
    }
    if (!runtime->connected) {
        return {PairingStatus::RuntimeSourceDisconnected, std::nullopt};
    }

    SeatBinding binding;
    binding.seatId = seatId;
    binding.api = ApiSurface::XInput;
    binding.runtimeKey = runtime->runtimeKey;
    binding.persistentControllerId = physical->persistentId;
    binding.runtimeXInputSlot = runtimeSlot;
    binding.sourceGeneration = runtime->sourceGeneration;
    return {PairingStatus::Ok, binding};
}

bool bindingMatchesInventory(const SeatBinding& binding,
                             const InventorySnapshot& inventory) noexcept {
    const SourceDescriptor* runtime = nullptr;
    std::size_t runtimeMatches = 0;
    for (const auto& source : inventory.sources) {
        if (source.api != binding.api || source.runtimeKey != binding.runtimeKey) continue;
        ++runtimeMatches;
        runtime = &source;
    }
    if (runtimeMatches != 1 || runtime == nullptr || !runtime->connected ||
        runtime->sourceGeneration != binding.sourceGeneration ||
        runtime->runtimeXInputSlot != binding.runtimeXInputSlot) {
        return false;
    }

    if (!binding.persistentControllerId) return true;

    const auto wanted = canonicalPersistentId(*binding.persistentControllerId);
    std::size_t physicalMatches = 0;
    for (const auto& physical : inventory.physicalControllers) {
        if (canonicalPersistentId(physical.persistentId) == wanted) {
            ++physicalMatches;
        }
    }
    return physicalMatches == 1;
}

} // namespace hydra::controller
