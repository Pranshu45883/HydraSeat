#include "hydra/hardware_identity.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using hydra::hardware::canonicalizeContainerId;
    using hydra::hardware::isObviousRemoteOrSyntheticInputIdentity;
    using hydra::hardware::isPreferredRepresentativePath;
    using hydra::hardware::makeStableDeviceId;
    using hydra::hardware::normalizeDevicePath;
    using hydra::hardware::selectInterfaceDeviceIdentity;
    using hydra::hardware::selectPhysicalIdentity;
    using namespace std::string_view_literals;

    const std::wstring withTrailingNulls(
        L"//?/hid#vid_046d&pid_c31c&col01#7&abc&0&0000\0\0"sv);
    check(normalizeDevicePath(withTrailingNulls) ==
              L"\\\\?\\HID#VID_046D&PID_C31C&COL01#7&ABC&0&0000",
          "normalization removes trailing NULs and canonicalizes ASCII path syntax");
    check(normalizeDevicePath(L"\\??\\hid#vid_046d&pid_c31c#instance") ==
              L"\\\\?\\HID#VID_046D&PID_C31C#INSTANCE",
          "equivalent NT path prefixes normalize to the Raw Input spelling");
    check(normalizeDevicePath(L"").empty(), "an empty path stays empty");

    constexpr std::wstring_view firstInterface =
        L"\\\\?\\HID#VID_046D&PID_C31C&MI_00&COL01#7&111111&0&0000#{GUID}";
    constexpr std::wstring_view siblingInterface =
        L"\\\\?\\HID#VID_046D&PID_C31C&MI_00&COL02#7&111111&0&0001#{GUID}";
    constexpr std::wstring_view otherPhysicalUnit =
        L"\\\\?\\HID#VID_046D&PID_C31C&MI_00&COL01#8&222222&0&0000#{GUID}";

    constexpr std::wstring_view firstDeviceInstance =
        L"HID\\VID_046D&PID_C31C&MI_00&COL01\\7&111111&0&0000";
    constexpr std::wstring_view siblingDeviceInstance =
        L"HID\\VID_046D&PID_C31C&MI_00&COL02\\7&111111&0&0001";
    constexpr std::wstring_view firstParent =
        L"USB\\VID_046D&PID_C31C&MI_00\\7&111111&0&0000";
    constexpr std::wstring_view siblingParent =
        L"USB\\VID_046D&PID_C31C&MI_01\\7&111111&0&0001";
    constexpr std::wstring_view otherParent =
        L"USB\\VID_046D&PID_C31C&MI_00\\8&222222&0&0000";
    constexpr std::wstring_view firstContainer =
        L"{a5b6c7d8-1111-2222-3344-5566778899aa}";
    constexpr std::wstring_view otherContainer =
        L"{a5b6c7d8-1111-2222-3344-5566778899bb}";
    constexpr std::wstring_view firstPhysicalUsbAncestor =
        L"USB\\VID_046D&PID_C31C\\6&AAAA1111&0&2";
    constexpr std::wstring_view otherPhysicalUsbAncestor =
        L"USB\\VID_046D&PID_C31C\\6&BBBB2222&0&2";

    check(canonicalizeContainerId(firstContainer) ==
              L"{A5B6C7D8-1111-2222-3344-5566778899AA}",
          "container identity normalization is deterministic and locale-independent");
    check(selectPhysicalIdentity(firstContainer, firstParent, firstDeviceInstance, firstInterface) ==
              L"CONTAINER:{A5B6C7D8-1111-2222-3344-5566778899AA}",
          "Windows container identity takes precedence over collection/interface identity");
    check(selectPhysicalIdentity(firstContainer, siblingParent, siblingDeviceInstance, siblingInterface) ==
              selectPhysicalIdentity(firstContainer, firstParent, firstDeviceInstance, firstInterface),
          "multiple HID interfaces in one physical container collapse to one identity");
    check(selectPhysicalIdentity(firstContainer, firstParent, firstDeviceInstance, firstInterface) !=
              selectPhysicalIdentity(otherContainer, otherParent, firstDeviceInstance, otherPhysicalUnit),
          "identical VID/PID devices with distinct physical containers remain distinct");
    check(makeStableDeviceId(
              L"Keyboard", L"", firstPhysicalUsbAncestor, firstDeviceInstance, firstInterface) ==
              makeStableDeviceId(
                  L"Keyboard", L"", firstPhysicalUsbAncestor, siblingDeviceInstance, siblingInterface),
          "a resolved full USB ancestor collapses sibling interfaces when ContainerId is unavailable");
    check(makeStableDeviceId(
              L"Keyboard", L"", firstPhysicalUsbAncestor, firstDeviceInstance, firstInterface) !=
              makeStableDeviceId(
                  L"Keyboard", L"", otherPhysicalUsbAncestor, firstDeviceInstance, otherPhysicalUnit),
          "full USB ancestor fallback keeps same-VID/PID physical instances distinct");

    check(selectPhysicalIdentity(firstParent, firstDeviceInstance, firstInterface) ==
              L"USB\\VID_046D&PID_C31C&MI_00\\7&111111&0&0000",
          "resolved parent instance ID is the preferred physical identity");
    check(selectPhysicalIdentity(firstParent, siblingDeviceInstance, siblingInterface) ==
              selectPhysicalIdentity(firstParent, firstDeviceInstance, firstInterface),
          "different HID collections with the same resolved parent deduplicate");
    check(selectPhysicalIdentity(firstParent, firstDeviceInstance, firstInterface) !=
              selectPhysicalIdentity(otherParent, firstDeviceInstance, otherPhysicalUnit),
          "identical VID/PID units with different resolved parents remain distinct");

    check(selectPhysicalIdentity(L"", firstDeviceInstance, firstInterface) ==
              L"HID\\VID_046D&PID_C31C&MI_00&COL01\\7&111111&0&0000",
          "device instance ID is used when parent resolution is unavailable");
    check(selectPhysicalIdentity(L"", L"", firstInterface) ==
              normalizeDevicePath(firstInterface),
          "full interface path is the conservative final fallback");
    check(selectPhysicalIdentity(L"", L"", firstInterface) !=
              selectPhysicalIdentity(L"", L"", siblingInterface),
          "fallback does not guess that distinct symbolic paths are one device");

    std::wstring representativeForward(siblingInterface);
    if (isPreferredRepresentativePath(firstInterface, representativeForward)) {
        representativeForward = firstInterface;
    }
    std::wstring representativeReverse(firstInterface);
    if (isPreferredRepresentativePath(siblingInterface, representativeReverse)) {
        representativeReverse = siblingInterface;
    }
    check(normalizeDevicePath(representativeForward) == normalizeDevicePath(representativeReverse),
          "representative Raw Input interface selection is independent of enumeration order");
    check(!isPreferredRepresentativePath(L"", representativeForward),
          "an empty candidate can never replace a valid representative interface");

    check(selectInterfaceDeviceIdentity(firstDeviceInstance, firstInterface) ==
              L"HID\\VID_046D&PID_C31C&MI_00&COL01\\7&111111&0&0000",
          "interface resolution prefers its device instance over its symbolic path");
    check(selectInterfaceDeviceIdentity(L"", firstInterface) ==
              normalizeDevicePath(firstInterface),
          "an unresolved interface keeps the complete symbolic path as fallback");

    check(makeStableDeviceId(L"Mouse", firstParent, firstDeviceInstance, firstInterface) !=
              makeStableDeviceId(L"Keyboard", firstParent, firstDeviceInstance, firstInterface),
          "different input categories do not suppress each other even with one parent");
    check(makeStableDeviceId(
              L"Keyboard", firstContainer, firstParent, firstDeviceInstance, firstInterface) ==
              makeStableDeviceId(
                  L"Keyboard", firstContainer, siblingParent, siblingDeviceInstance, siblingInterface),
          "stable persisted ID collapses sibling HID interfaces in one physical container");
    check(makeStableDeviceId(
              L"Keyboard", firstContainer, firstParent, firstDeviceInstance, firstInterface) !=
              makeStableDeviceId(
                  L"Keyboard", otherContainer, otherParent, firstDeviceInstance, otherPhysicalUnit),
          "stable persisted ID keeps same-VID/PID physical devices separate by container authority");
    check(makeStableDeviceId(L"Keyboard", L"", L"", L"").empty(),
          "missing identity data cannot produce a stable ID");

    check(hydra::hardware::isObviousRemoteOrSyntheticInputPath(
              L"\\\\?\\ROOT#RDP_MOU#0000"),
          "obvious remote input paths are filtered");
    check(hydra::hardware::isObviousRemoteOrSyntheticInputPath(
              L"\\\\?\\ROOT#FEIZHI_VIRTUAL_KEYBOARD#0000"),
          "explicit ROOT-enumerated virtual input devices are filtered fail-closed");
    check(!hydra::hardware::isObviousRemoteOrSyntheticInputPath(firstInterface),
          "ordinary local HID paths are retained");
    check(isObviousRemoteOrSyntheticInputIdentity(
              firstInterface, firstDeviceInstance, L"ROOT\\RDP_KBD\\0000"),
          "remote/synthetic evidence in resolved ancestry remains fail-closed");
    check(hydra::hardware::isLikelyInternalKeyboardPath(
              L"\\\\?\\ACPI#PNP0303#3&ABC&0"),
          "internal keyboard paths are recognized only as a label heuristic");
    check(hydra::hardware::isLikelyTouchpadPath(
              L"\\\\?\\HID#ELAN&PNP0C50#INSTANCE"),
          "touchpad path heuristic remains a fallback classifier");
    check(hydra::hardware::isXInputShadowPath(
              L"\\\\?\\HID#VID_045E&PID_028E&IG_00#INSTANCE"),
          "XInput shadow HID paths are recognized");
    check(hydra::hardware::isLikelyVirtualDisplayIdentity(
              L"Example Indirect Display Adapter"),
          "virtual-display classification remains explicitly heuristic");
    check(!hydra::hardware::isLikelyVirtualDisplayIdentity(
              L"DISPLAY\\DEL40A9\\5&12345&0&UID4352"),
          "ordinary monitor instance IDs are not marked virtual");

    std::cout << "Hardware identity tests passed.\n";
    return EXIT_SUCCESS;
}
