#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace hydra::hardware {

inline std::wstring trimTrailingNulls(std::wstring value) {
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

constexpr wchar_t asciiUpper(wchar_t character) noexcept {
    if (character >= L'a' && character <= L'z') {
        return static_cast<wchar_t>(character - (L'a' - L'A'));
    }
    return character;
}

inline std::wstring normalizeDevicePath(std::wstring_view path) {
    std::wstring normalized(path);
    normalized = trimTrailingNulls(std::move(normalized));
    for (auto& character : normalized) {
        if (character == L'/') {
            character = L'\\';
        } else {
            // Device interface paths use ASCII identifiers. Avoid locale-sensitive
            // case folding so identity keys are identical on every machine.
            character = asciiUpper(character);
        }
    }

    // Win32 accepts both spellings for the NT object-manager device prefix.
    // Use the form returned by Raw Input so equivalent paths have one key.
    if (normalized.starts_with(L"\\??\\")) {
        normalized.replace(0, 4, L"\\\\?\\");
    }
    return normalized;
}

inline std::wstring canonicalizeInstanceId(std::wstring_view instanceId) {
    return normalizeDevicePath(instanceId);
}

inline std::wstring canonicalizeContainerId(std::wstring_view containerId) {
    std::wstring normalized = trimTrailingNulls(std::wstring(containerId));
    for (auto& character : normalized) {
        character = asciiUpper(character);
    }
    return normalized;
}

inline std::wstring selectPhysicalIdentity(std::wstring_view physicalContainerId,
                                           std::wstring_view parentInstanceId,
                                           std::wstring_view deviceInstanceId,
                                           std::wstring_view interfacePath) {
    if (!physicalContainerId.empty()) {
        auto identity = canonicalizeContainerId(physicalContainerId);
        if (!identity.empty()) {
            return L"CONTAINER:" + identity;
        }
    }
    if (!parentInstanceId.empty()) {
        return canonicalizeInstanceId(parentInstanceId);
    }
    if (!deviceInstanceId.empty()) {
        return canonicalizeInstanceId(deviceInstanceId);
    }
    return normalizeDevicePath(interfacePath);
}

inline std::wstring selectPhysicalIdentity(std::wstring_view parentInstanceId,
                                           std::wstring_view deviceInstanceId,
                                           std::wstring_view interfacePath) {
    return selectPhysicalIdentity({}, parentInstanceId, deviceInstanceId, interfacePath);
}

inline std::wstring selectInterfaceDeviceIdentity(std::wstring_view deviceInstanceId,
                                                  std::wstring_view interfacePath) {
    if (!deviceInstanceId.empty()) {
        return canonicalizeInstanceId(deviceInstanceId);
    }
    return normalizeDevicePath(interfacePath);
}

inline std::wstring makeStableDeviceId(std::wstring_view category,
                                       std::wstring_view physicalContainerId,
                                       std::wstring_view parentInstanceId,
                                       std::wstring_view deviceInstanceId,
                                       std::wstring_view interfacePath) {
    const auto identity = selectPhysicalIdentity(
        physicalContainerId, parentInstanceId, deviceInstanceId, interfacePath);
    if (identity.empty()) {
        return {};
    }

    std::wstring id(category);
    id.push_back(L':');
    id.append(identity);
    return id;
}

inline std::wstring makeStableDeviceId(std::wstring_view category,
                                       std::wstring_view parentInstanceId,
                                       std::wstring_view deviceInstanceId,
                                       std::wstring_view interfacePath) {
    return makeStableDeviceId(
        category, {}, parentInstanceId, deviceInstanceId, interfacePath);
}

inline bool isPreferredRepresentativePath(std::wstring_view candidatePath,
                                          std::wstring_view currentPath) {
    if (candidatePath.empty()) {
        return false;
    }
    if (currentPath.empty()) {
        return true;
    }
    return normalizeDevicePath(candidatePath) < normalizeDevicePath(currentPath);
}

inline bool containsToken(std::wstring_view normalizedValue, std::wstring_view token) {
    return normalizedValue.find(token) != std::wstring_view::npos;
}

inline bool isObviousRemoteOrSyntheticInputPath(std::wstring_view path) {
    const auto normalized = normalizeDevicePath(path);
    const bool rootEnumerated = containsToken(normalized, L"ROOT#") ||
                                containsToken(normalized, L"ROOT\\");
    const bool explicitlyVirtual = containsToken(normalized, L"VIRTUAL");
    return containsToken(normalized, L"RDP_KBD") ||
           containsToken(normalized, L"RDP_MOU") ||
           containsToken(normalized, L"ROOT#RDP") ||
           containsToken(normalized, L"ROOT\\RDP") ||
           containsToken(normalized, L"TERMSRV") ||
           (rootEnumerated && explicitlyVirtual);
}

inline bool isObviousRemoteOrSyntheticInputIdentity(std::wstring_view interfacePath,
                                                     std::wstring_view deviceInstanceId,
                                                     std::wstring_view parentInstanceId) {
    return isObviousRemoteOrSyntheticInputPath(interfacePath) ||
           isObviousRemoteOrSyntheticInputPath(deviceInstanceId) ||
           isObviousRemoteOrSyntheticInputPath(parentInstanceId);
}

inline bool isLikelyInternalKeyboardPath(std::wstring_view path) {
    const auto normalized = normalizeDevicePath(path);
    return containsToken(normalized, L"I8042PRT") ||
           containsToken(normalized, L"PNP0303") ||
           containsToken(normalized, L"MSFT0001") ||
           containsToken(normalized, L"ACPI#");
}

inline bool isLikelyTouchpadPath(std::wstring_view path) {
    const auto normalized = normalizeDevicePath(path);
    return containsToken(normalized, L"TOUCHPAD") ||
           containsToken(normalized, L"PRECISIONTOUCHPAD") ||
           containsToken(normalized, L"PNP0C50") ||
           containsToken(normalized, L"MSFT0001") ||
           containsToken(normalized, L"ELAN") ||
           containsToken(normalized, L"SYNAPTICS");
}

inline bool isXInputShadowPath(std::wstring_view path) {
    const auto normalized = normalizeDevicePath(path);
    return containsToken(normalized, L"&IG_") || containsToken(normalized, L"#IG_");
}

inline bool isLikelyVirtualDisplayIdentity(std::wstring_view identity) {
    const auto normalized = normalizeDevicePath(identity);
    return containsToken(normalized, L"VIRTUAL DISPLAY") ||
           containsToken(normalized, L"VIRTUAL_DISPLAY") ||
           containsToken(normalized, L"INDIRECT DISPLAY") ||
           containsToken(normalized, L"INDIRECT_DISPLAY") ||
           containsToken(normalized, L"MIRROR DISPLAY");
}

} // namespace hydra::hardware
