#include "hydra/hidhide_probe.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <winsvc.h>
#include <winver.h>

#include <cwctype>
#include <memory>
#endif

namespace hydra {
namespace {

constexpr std::array<HidHideVersion, 3> kKnownSupportedVersions{{
    {1, 7, 339, 0},
    {1, 7, 344, 0},
    {1, 7, 346, 0},
}};

bool isEvidence(HidHidePlatformStatus status) noexcept {
    return status == HidHidePlatformStatus::Success ||
           status == HidHidePlatformStatus::AccessDenied;
}

HidHideProbeDiagnostic queryFailureDiagnostic(
    HidHidePlatformStatus status,
    HidHideProbeDiagnostic ordinaryFailure) noexcept {
    if (status == HidHidePlatformStatus::AccessDenied) {
        return HidHideProbeDiagnostic::AccessDenied;
    }
    if (status == HidHidePlatformStatus::ResponseTooLarge) {
        return HidHideProbeDiagnostic::ResponseTooLarge;
    }
    if (status == HidHidePlatformStatus::Malformed) {
        return HidHideProbeDiagnostic::MalformedResponse;
    }
    return ordinaryFailure;
}

std::optional<bool> parseBooleanResponse(
    const HidHideBooleanQueryResult& result,
    HidHideProbeDiagnostic failureDiagnostic,
    HidHideProbeReport& report) {
    if (result.status != HidHidePlatformStatus::Success) {
        report.diagnostic = queryFailureDiagnostic(
            result.status, failureDiagnostic);
        report.systemError = result.systemError;
        return std::nullopt;
    }
    if (result.response.size() > kHidHideMaxControlResponseBytes) {
        report.diagnostic = HidHideProbeDiagnostic::ResponseTooLarge;
        return std::nullopt;
    }
    if (result.response.size() != 1 || result.response.front() > 1) {
        report.diagnostic = HidHideProbeDiagnostic::MalformedResponse;
        return std::nullopt;
    }
    return result.response.front() != 0;
}

#if defined(_WIN32)

constexpr std::size_t kMaxInterfaceCharacters = 32 * 1024;
constexpr DWORD kMaxServiceConfigBytes = 64 * 1024;
constexpr DWORD kMaxVersionInfoBytes = 1024 * 1024;
constexpr wchar_t kHidHideServiceName[] = L"HidHide";

// Public HidHide developer contract at revision
// 2b950fd9393e1644b4199f6eb4999e1720f0c6e9. Only the two documented
// read-only state queries are represented here; list and mutation IOCTLs are
// intentionally absent from this packet.
constexpr GUID kHidHideInterfaceGuid{
    0x0c320ff7,
    0xbd9b,
    0x42b6,
    {0xbd, 0xaf, 0x49, 0xfe, 0xb9, 0xc9, 0x16, 0x49}};
constexpr DWORD kHidHideDeviceType = 32769;
constexpr DWORD kIoctlGetActive =
    CTL_CODE(kHidHideDeviceType, 2052, METHOD_BUFFERED, FILE_READ_DATA);
constexpr DWORD kIoctlGetInverse =
    CTL_CODE(kHidHideDeviceType, 2054, METHOD_BUFFERED, FILE_READ_DATA);

template <typename Handle, typename CloseFunction>
class ScopedHandle {
public:
    ScopedHandle(Handle handle, CloseFunction closeFunction) noexcept
        : m_handle(handle), m_closeFunction(closeFunction) {}
    ~ScopedHandle() {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            m_closeFunction(m_handle);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    Handle get() const noexcept { return m_handle; }

private:
    Handle m_handle;
    CloseFunction m_closeFunction;
};

HidHidePlatformStatus statusForError(DWORD error) noexcept {
    if (error == ERROR_ACCESS_DENIED) {
        return HidHidePlatformStatus::AccessDenied;
    }
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_SERVICE_DOES_NOT_EXIST || error == ERROR_NOT_FOUND ||
        error == ERROR_NO_SUCH_DEVICE) {
        return HidHidePlatformStatus::NotFound;
    }
    return HidHidePlatformStatus::Failed;
}

bool startsWithInsensitive(std::wstring_view value,
                           std::wstring_view prefix) noexcept {
    if (value.size() < prefix.size()) {
        return false;
    }
    return CompareStringOrdinal(
               value.data(), static_cast<int>(prefix.size()),
               prefix.data(), static_cast<int>(prefix.size()), TRUE) ==
           CSTR_EQUAL;
}

std::optional<std::wstring> windowsDirectory() {
    std::array<wchar_t, MAX_PATH + 1> buffer{};
    const UINT length = GetWindowsDirectoryW(
        buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return std::nullopt;
    }
    return std::wstring(buffer.data(), length);
}

std::optional<std::wstring> normalizeDriverPath(std::wstring raw) {
    while (!raw.empty() && std::iswspace(raw.front()) != 0) {
        raw.erase(raw.begin());
    }
    while (!raw.empty() && std::iswspace(raw.back()) != 0) {
        raw.pop_back();
    }
    if (raw.empty()) {
        return std::nullopt;
    }

    if (raw.front() == L'\"') {
        const auto closing = raw.find(L'\"', 1);
        if (closing == std::wstring::npos) {
            return std::nullopt;
        }
        raw = raw.substr(1, closing - 1);
    }

    if (startsWithInsensitive(raw, L"\\??\\")) {
        raw.erase(0, 4);
    }

    if (startsWithInsensitive(raw, L"\\SystemRoot\\")) {
        const auto root = windowsDirectory();
        if (!root) {
            return std::nullopt;
        }
        raw = *root + raw.substr(11);
    } else if (startsWithInsensitive(raw, L"System32\\")) {
        const auto root = windowsDirectory();
        if (!root) {
            return std::nullopt;
        }
        raw = *root + L"\\" + raw;
    }

    const DWORD required = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
    if (required == 0 || required > kMaxInterfaceCharacters) {
        return std::nullopt;
    }
    std::vector<wchar_t> expanded(required);
    const DWORD written = ExpandEnvironmentStringsW(
        raw.c_str(), expanded.data(), required);
    if (written == 0 || written > required) {
        return std::nullopt;
    }
    return std::wstring(expanded.data());
}

struct ServiceHandles {
    SC_HANDLE manager{nullptr};
    SC_HANDLE service{nullptr};
};

HidHideEvidenceResult openHidHideService(ServiceHandles& handles) {
    handles.manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (handles.manager == nullptr) {
        const DWORD error = GetLastError();
        return {statusForError(error), error};
    }
    handles.service = OpenServiceW(
        handles.manager, kHidHideServiceName, SERVICE_QUERY_CONFIG);
    if (handles.service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(handles.manager);
        handles.manager = nullptr;
        return {statusForError(error), error};
    }
    return {HidHidePlatformStatus::Success, 0};
}

void closeServiceHandles(ServiceHandles& handles) noexcept {
    if (handles.service != nullptr) {
        CloseServiceHandle(handles.service);
    }
    if (handles.manager != nullptr) {
        CloseServiceHandle(handles.manager);
    }
    handles = {};
}

class WindowsHidHideProbePlatform final : public HidHideProbePlatform {
public:
    HidHideEvidenceResult installationEvidence() override {
        ServiceHandles handles;
        const auto result = openHidHideService(handles);
        closeServiceHandles(handles);
        return result;
    }

    HidHideEvidenceResult controlInterfaceEvidence() override {
        ULONG characterCount = 0;
        CONFIGRET result = CM_Get_Device_Interface_List_SizeW(
            &characterCount, const_cast<GUID*>(&kHidHideInterfaceGuid),
            nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result != CR_SUCCESS) {
            const DWORD error = CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE);
            return {statusForError(error), error};
        }
        if (characterCount == 0 ||
            characterCount > kMaxInterfaceCharacters) {
            return {characterCount > kMaxInterfaceCharacters
                        ? HidHidePlatformStatus::ResponseTooLarge
                        : HidHidePlatformStatus::Malformed,
                    0};
        }

        std::vector<wchar_t> interfaces(characterCount, L'\0');
        result = CM_Get_Device_Interface_ListW(
            const_cast<GUID*>(&kHidHideInterfaceGuid), nullptr,
            interfaces.data(), characterCount,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result != CR_SUCCESS) {
            const DWORD error = CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE);
            return {statusForError(error), error};
        }
        if (interfaces.back() != L'\0') {
            return {HidHidePlatformStatus::Malformed, 0};
        }

        std::size_t cursor = 0;
        std::size_t count = 0;
        std::wstring selected;
        while (cursor < interfaces.size() && interfaces[cursor] != L'\0') {
            const auto end = std::find(
                interfaces.begin() + static_cast<std::ptrdiff_t>(cursor),
                interfaces.end(), L'\0');
            if (end == interfaces.end()) {
                return {HidHidePlatformStatus::Malformed, 0};
            }
            if (count == 0) {
                selected.assign(
                    interfaces.begin() + static_cast<std::ptrdiff_t>(cursor),
                    end);
            }
            ++count;
            cursor = static_cast<std::size_t>(
                std::distance(interfaces.begin(), end)) + 1;
        }
        if (count == 0) {
            return {HidHidePlatformStatus::NotFound, 0};
        }
        if (count != 1 || cursor >= interfaces.size() ||
            interfaces[cursor] != L'\0') {
            return {HidHidePlatformStatus::Malformed, 0};
        }
        m_interfacePath = std::move(selected);
        return {HidHidePlatformStatus::Success, 0};
    }

    HidHideVersionResult driverVersion() override {
        ServiceHandles handles;
        const auto opened = openHidHideService(handles);
        if (opened.status != HidHidePlatformStatus::Success) {
            return {opened.status, std::nullopt, opened.systemError};
        }

        DWORD requiredBytes = 0;
        if (QueryServiceConfigW(
                handles.service, nullptr, 0, &requiredBytes) != FALSE) {
            closeServiceHandles(handles);
            return {HidHidePlatformStatus::Malformed, std::nullopt, 0};
        }
        const DWORD sizeError = GetLastError();
        if (sizeError != ERROR_INSUFFICIENT_BUFFER || requiredBytes == 0) {
            closeServiceHandles(handles);
            return {statusForError(sizeError), std::nullopt, sizeError};
        }
        if (requiredBytes > kMaxServiceConfigBytes) {
            closeServiceHandles(handles);
            return {HidHidePlatformStatus::ResponseTooLarge, std::nullopt, 0};
        }

        std::vector<std::uint8_t> storage(requiredBytes);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
        DWORD ignored = 0;
        if (QueryServiceConfigW(
                handles.service, config, requiredBytes, &ignored) == FALSE) {
            const DWORD error = GetLastError();
            closeServiceHandles(handles);
            return {statusForError(error), std::nullopt, error};
        }
        const std::wstring rawPath =
            config->lpBinaryPathName == nullptr
                ? std::wstring{}
                : std::wstring(config->lpBinaryPathName);
        closeServiceHandles(handles);

        const auto path = normalizeDriverPath(rawPath);
        if (!path) {
            return {HidHidePlatformStatus::Malformed, std::nullopt, 0};
        }

        DWORD versionHandle = 0;
        const DWORD versionBytes = GetFileVersionInfoSizeW(
            path->c_str(), &versionHandle);
        if (versionBytes == 0) {
            const DWORD error = GetLastError();
            return {statusForError(error), std::nullopt, error};
        }
        if (versionBytes > kMaxVersionInfoBytes) {
            return {HidHidePlatformStatus::ResponseTooLarge, std::nullopt, 0};
        }

        std::vector<std::uint8_t> versionInfo(versionBytes);
        if (GetFileVersionInfoW(
                path->c_str(), 0, versionBytes, versionInfo.data()) == FALSE) {
            const DWORD error = GetLastError();
            return {statusForError(error), std::nullopt, error};
        }

        VS_FIXEDFILEINFO* fixedInfo = nullptr;
        UINT fixedInfoBytes = 0;
        if (VerQueryValueW(
                versionInfo.data(), L"\\",
                reinterpret_cast<void**>(&fixedInfo),
                &fixedInfoBytes) == FALSE ||
            fixedInfo == nullptr || fixedInfoBytes < sizeof(VS_FIXEDFILEINFO) ||
            fixedInfo->dwSignature != 0xfeef04bd) {
            return {HidHidePlatformStatus::Malformed, std::nullopt, 0};
        }

        const HidHideVersion version{
            HIWORD(fixedInfo->dwFileVersionMS),
            LOWORD(fixedInfo->dwFileVersionMS),
            HIWORD(fixedInfo->dwFileVersionLS),
            LOWORD(fixedInfo->dwFileVersionLS)};
        return {HidHidePlatformStatus::Success, version, 0};
    }

    HidHideControlReadResult queryControlStateReadOnly() override {
        HidHideControlReadResult result;
        if (m_interfacePath.empty()) {
            result.openStatus = HidHidePlatformStatus::NotFound;
            return result;
        }

        const HANDLE rawHandle = CreateFileW(
            m_interfacePath.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (rawHandle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            result.openStatus = statusForError(error);
            result.systemError = error;
            return result;
        }
        const ScopedHandle handle(
            rawHandle, [](HANDLE value) { CloseHandle(value); });
        result.openStatus = HidHidePlatformStatus::Success;
        result.active = queryBoolean(handle.get(), kIoctlGetActive);
        result.inverseWhitelist = queryBoolean(
            handle.get(), kIoctlGetInverse);
        return result;
    }

private:
    static HidHideBooleanQueryResult queryBoolean(HANDLE handle,
                                                   DWORD controlCode) {
        std::array<std::uint8_t, kHidHideMaxControlResponseBytes> buffer{};
        DWORD bytesReturned = 0;
        if (DeviceIoControl(
                handle, controlCode, nullptr, 0, buffer.data(),
                static_cast<DWORD>(buffer.size()), &bytesReturned,
                nullptr) == FALSE) {
            const DWORD error = GetLastError();
            return {statusForError(error), {}, error};
        }
        if (bytesReturned > buffer.size()) {
            return {HidHidePlatformStatus::ResponseTooLarge, {}, 0};
        }
        return {
            HidHidePlatformStatus::Success,
            std::vector<std::uint8_t>(
                buffer.begin(),
                buffer.begin() + static_cast<std::ptrdiff_t>(bytesReturned)),
            0};
    }

    std::wstring m_interfacePath;
};

#else

class UnsupportedHidHideProbePlatform final : public HidHideProbePlatform {
public:
    HidHideEvidenceResult installationEvidence() override {
        return {HidHidePlatformStatus::UnsupportedPlatform, 0};
    }
    HidHideEvidenceResult controlInterfaceEvidence() override {
        return {HidHidePlatformStatus::UnsupportedPlatform, 0};
    }
    HidHideVersionResult driverVersion() override {
        return {HidHidePlatformStatus::UnsupportedPlatform, std::nullopt, 0};
    }
    HidHideControlReadResult queryControlStateReadOnly() override {
        HidHideControlReadResult result;
        result.openStatus = HidHidePlatformStatus::UnsupportedPlatform;
        return result;
    }
};

#endif

} // namespace

bool isKnownSupportedHidHideVersion(const HidHideVersion& version) noexcept {
    return std::find(
               kKnownSupportedVersions.begin(),
               kKnownSupportedVersions.end(), version) !=
           kKnownSupportedVersions.end();
}

HidHideProbeReport probeHidHide(HidHideProbePlatform& platform) {
    HidHideProbeReport report;
    const auto installation = platform.installationEvidence();
    if (installation.status == HidHidePlatformStatus::UnsupportedPlatform) {
        report.diagnostic = HidHideProbeDiagnostic::UnsupportedPlatform;
        return report;
    }

    const auto interface = platform.controlInterfaceEvidence();
    report.installedEvidence = isEvidence(installation.status) ||
                               isEvidence(interface.status);
    report.controlInterfacePresent =
        interface.status == HidHidePlatformStatus::Success;

    if (installation.status == HidHidePlatformStatus::NotFound &&
        interface.status == HidHidePlatformStatus::NotFound) {
        report.diagnostic = HidHideProbeDiagnostic::NotDetected;
        return report;
    }

    report.availability = HidHideAvailability::InstalledUnverified;
    if (interface.status != HidHidePlatformStatus::Success) {
        if (interface.status == HidHidePlatformStatus::NotFound &&
            installation.status != HidHidePlatformStatus::Success &&
            installation.status != HidHidePlatformStatus::NotFound) {
            report.diagnostic = queryFailureDiagnostic(
                installation.status,
                HidHideProbeDiagnostic::InstallationQueryFailed);
            report.systemError = installation.systemError;
        } else {
            report.diagnostic = queryFailureDiagnostic(
                interface.status,
                interface.status == HidHidePlatformStatus::NotFound
                    ? HidHideProbeDiagnostic::ControlInterfaceAbsent
                    : HidHideProbeDiagnostic::InterfaceQueryFailed);
            report.systemError = interface.systemError;
        }
        return report;
    }

    const auto version = platform.driverVersion();
    if (version.status != HidHidePlatformStatus::Success || !version.version) {
        report.diagnostic = queryFailureDiagnostic(
            version.status, HidHideProbeDiagnostic::VersionUnavailable);
        report.systemError = version.systemError;
        return report;
    }
    report.driverVersion = version.version;
    if (!isKnownSupportedHidHideVersion(*version.version)) {
        report.diagnostic = HidHideProbeDiagnostic::UnsupportedVersion;
        return report;
    }

    const auto control = platform.queryControlStateReadOnly();
    if (control.openStatus != HidHidePlatformStatus::Success) {
        report.diagnostic = queryFailureDiagnostic(
            control.openStatus, HidHideProbeDiagnostic::ControlOpenFailed);
        report.systemError = control.systemError;
        return report;
    }
    report.controlInterfaceReadable = true;

    report.active = parseBooleanResponse(
        control.active, HidHideProbeDiagnostic::ActiveQueryFailed, report);
    if (!report.active) {
        return report;
    }
    report.inverseWhitelist = parseBooleanResponse(
        control.inverseWhitelist,
        HidHideProbeDiagnostic::InverseQueryFailed, report);
    if (!report.inverseWhitelist) {
        return report;
    }

    report.availability = HidHideAvailability::VerifiedSupported;
    report.sessionBlacklistSupported = true;
    report.diagnostic = HidHideProbeDiagnostic::None;
    report.systemError = 0;
    return report;
}

HidHideProbeReport probeHidHide() {
#if defined(_WIN32)
    WindowsHidHideProbePlatform platform;
#else
    UnsupportedHidHideProbePlatform platform;
#endif
    return probeHidHide(platform);
}

std::string_view hidHideAvailabilityName(HidHideAvailability value) noexcept {
    switch (value) {
    case HidHideAvailability::Unavailable:
        return "unavailable";
    case HidHideAvailability::InstalledUnverified:
        return "installed-unverified";
    case HidHideAvailability::VerifiedSupported:
        return "verified-supported";
    }
    return "unknown";
}

std::string_view hidHideProbeDiagnosticName(
    HidHideProbeDiagnostic value) noexcept {
    switch (value) {
    case HidHideProbeDiagnostic::None:
        return "none";
    case HidHideProbeDiagnostic::UnsupportedPlatform:
        return "unsupported-platform";
    case HidHideProbeDiagnostic::NotDetected:
        return "not-detected";
    case HidHideProbeDiagnostic::InstallationQueryFailed:
        return "installation-query-failed";
    case HidHideProbeDiagnostic::ControlInterfaceAbsent:
        return "control-interface-absent";
    case HidHideProbeDiagnostic::InterfaceQueryFailed:
        return "interface-query-failed";
    case HidHideProbeDiagnostic::AccessDenied:
        return "access-denied";
    case HidHideProbeDiagnostic::VersionUnavailable:
        return "version-unavailable";
    case HidHideProbeDiagnostic::UnsupportedVersion:
        return "unsupported-version";
    case HidHideProbeDiagnostic::ControlOpenFailed:
        return "control-open-failed";
    case HidHideProbeDiagnostic::ActiveQueryFailed:
        return "active-query-failed";
    case HidHideProbeDiagnostic::InverseQueryFailed:
        return "inverse-query-failed";
    case HidHideProbeDiagnostic::MalformedResponse:
        return "malformed-response";
    case HidHideProbeDiagnostic::ResponseTooLarge:
        return "response-too-large";
    }
    return "unknown";
}

std::string formatHidHideVersion(const HidHideVersion& version) {
    std::ostringstream out;
    out << version.major << '.' << version.minor << '.'
        << version.revision << '.' << version.build;
    return out.str();
}

std::string formatHidHideProbeReport(const HidHideProbeReport& report) {
    const auto optionalBoolean = [](const std::optional<bool>& value) {
        if (!value) {
            return std::string_view{"unknown"};
        }
        return *value ? std::string_view{"true"}
                      : std::string_view{"false"};
    };

    std::ostringstream out;
    out << "HydraSeat HidHide read-only probe\n"
        << "state: " << hidHideAvailabilityName(report.availability) << '\n'
        << "installed_evidence: "
        << (report.installedEvidence ? "yes" : "no") << '\n'
        << "control_interface_present: "
        << (report.controlInterfacePresent ? "yes" : "no") << '\n'
        << "control_interface_readable: "
        << (report.controlInterfaceReadable ? "yes" : "no") << '\n'
        << "driver_version: "
        << (report.driverVersion
                ? formatHidHideVersion(*report.driverVersion)
                : std::string{"unknown"})
        << '\n'
        << "active: " << optionalBoolean(report.active) << '\n'
        << "inverse_whitelist: "
        << optionalBoolean(report.inverseWhitelist) << '\n'
        << "session_blacklist_capability: "
        << (report.sessionBlacklistSupported ? "known" : "unknown") << '\n'
        << "diagnostic: "
        << hidHideProbeDiagnosticName(report.diagnostic) << '\n'
        << "system_error: " << report.systemError << '\n'
        << "mutation_performed: no\n";
    return out.str();
}

} // namespace hydra
