#include "hydra/hidhide_session_backend.hpp"
#include "hydra/hidhide_probe.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace hydra {
namespace {

#if defined(_WIN32)

constexpr DWORD kHidHideDeviceType = 32769u;
constexpr DWORD kIoctlGetWhitelist =
    CTL_CODE(kHidHideDeviceType, 2048, METHOD_BUFFERED, FILE_READ_DATA);
constexpr DWORD kIoctlSetWhitelist =
    CTL_CODE(kHidHideDeviceType, 2049, METHOD_BUFFERED, FILE_READ_DATA);
constexpr DWORD kIoctlGetBlacklist =
    CTL_CODE(kHidHideDeviceType, 2050, METHOD_BUFFERED, FILE_READ_DATA);
constexpr DWORD kIoctlSetBlacklist =
    CTL_CODE(kHidHideDeviceType, 2051, METHOD_BUFFERED, FILE_READ_DATA);
constexpr DWORD kIoctlGetActive =
    CTL_CODE(kHidHideDeviceType, 2052, METHOD_BUFFERED, FILE_READ_DATA);
constexpr DWORD kIoctlSetActive =
    CTL_CODE(kHidHideDeviceType, 2053, METHOD_BUFFERED, FILE_READ_DATA);
constexpr DWORD kIoctlGetInverse =
    CTL_CODE(kHidHideDeviceType, 2054, METHOD_BUFFERED, FILE_READ_DATA);
constexpr DWORD kIoctlSetInverse =
    CTL_CODE(kHidHideDeviceType, 2055, METHOD_BUFFERED, FILE_READ_DATA);
constexpr DWORD kIoctlAddSessionBlacklist =
    CTL_CODE(kHidHideDeviceType, 2056, METHOD_BUFFERED, FILE_READ_DATA);
constexpr DWORD kIoctlClearSessionBlacklist =
    CTL_CODE(kHidHideDeviceType, 2057, METHOD_BUFFERED, FILE_READ_DATA);
constexpr std::size_t kMaximumMultiStringBytes = 128u * 1024u;
constexpr wchar_t kControlDeviceName[] = L"\\\\.\\HidHide";

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_(value) {}
    ~ScopedHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
private:
    HANDLE value_;
};

std::string winError(std::string_view prefix, DWORD error) {
    return std::string(prefix) + ": " + std::to_string(error);
}

ScopedHandle openControl(std::string& error) {
    const HANDLE handle = CreateFileW(
        kControlDeviceName,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = winError("HidHide control-device open failed", GetLastError());
    } else {
        error.clear();
    }
    return ScopedHandle(handle);
}

bool queryBoolean(HANDLE handle, DWORD code, bool& value, std::string& error) {
    BOOLEAN raw = FALSE;
    DWORD returned = 0;
    if (DeviceIoControl(handle, code, nullptr, 0, &raw, sizeof(raw),
                        &returned, nullptr) == FALSE || returned != sizeof(raw)) {
        error = winError("HidHide boolean query failed", GetLastError());
        return false;
    }
    value = raw != FALSE;
    error.clear();
    return true;
}

bool setBoolean(HANDLE handle, DWORD code, bool value, std::string& error) {
    BOOLEAN raw = value ? TRUE : FALSE;
    DWORD returned = 0;
    if (DeviceIoControl(handle, code, &raw, sizeof(raw), nullptr, 0,
                        &returned, nullptr) == FALSE) {
        error = winError("HidHide boolean update failed", GetLastError());
        return false;
    }
    error.clear();
    return true;
}

bool parseMultiString(const std::vector<wchar_t>& buffer,
                      std::size_t maximumEntries,
                      std::vector<std::wstring>& values,
                      std::string& error) {
    values.clear();
    if (buffer.size() < 2u || buffer[buffer.size() - 1u] != L'\0' ||
        buffer[buffer.size() - 2u] != L'\0') {
        error = "HidHide multi-string response is not double-null terminated";
        return false;
    }
    std::size_t index = 0;
    while (index < buffer.size() && buffer[index] != L'\0') {
        std::size_t end = index;
        while (end < buffer.size() && buffer[end] != L'\0') ++end;
        if (end == buffer.size() || end == index ||
            end - index > kHidHideSessionMaxIdentifierChars ||
            values.size() >= maximumEntries) {
            error = "HidHide multi-string response exceeds the bounded session model";
            return false;
        }
        values.emplace_back(buffer.data() + index, end - index);
        index = end + 1u;
    }
    error.clear();
    return true;
}

bool queryMultiString(HANDLE handle,
                      DWORD code,
                      std::size_t maximumEntries,
                      std::vector<std::wstring>& values,
                      std::string& error) {
    DWORD needed = 0;
    if (DeviceIoControl(handle, code, nullptr, 0, nullptr, 0, &needed,
                        nullptr) == FALSE) {
        error = winError("HidHide multi-string size query failed", GetLastError());
        return false;
    }
    if (needed < 2u * sizeof(wchar_t) || needed > kMaximumMultiStringBytes ||
        needed % sizeof(wchar_t) != 0) {
        error = "HidHide multi-string size is malformed or exceeds the bound";
        return false;
    }
    std::vector<wchar_t> buffer(needed / sizeof(wchar_t));
    DWORD returned = 0;
    if (DeviceIoControl(handle, code, nullptr, 0, buffer.data(), needed,
                        &returned, nullptr) == FALSE || returned != needed) {
        error = winError("HidHide multi-string query failed", GetLastError());
        return false;
    }
    return parseMultiString(buffer, maximumEntries, values, error);
}

std::vector<wchar_t> makeMultiString(std::span<const std::wstring> values) {
    std::vector<wchar_t> buffer;
    std::size_t characters = 1u;
    for (const auto& value : values) characters += value.size() + 1u;
    if (values.empty()) ++characters;
    buffer.reserve(characters);
    for (const auto& value : values) {
        buffer.insert(buffer.end(), value.begin(), value.end());
        buffer.push_back(L'\0');
    }
    buffer.push_back(L'\0');
    if (values.empty()) buffer.push_back(L'\0');
    return buffer;
}

bool setMultiString(HANDLE handle,
                    DWORD code,
                    std::span<const std::wstring> values,
                    std::string& error) {
    const auto buffer = makeMultiString(values);
    const auto bytes64 = static_cast<std::uint64_t>(buffer.size()) * sizeof(wchar_t);
    if (bytes64 > std::numeric_limits<DWORD>::max() ||
        bytes64 > kMaximumMultiStringBytes) {
        error = "HidHide multi-string request exceeds the bound";
        return false;
    }
    DWORD returned = 0;
    if (DeviceIoControl(handle, code,
                        const_cast<wchar_t*>(buffer.data()),
                        static_cast<DWORD>(bytes64),
                        nullptr, 0, &returned, nullptr) == FALSE) {
        error = winError("HidHide multi-string update failed", GetLastError());
        return false;
    }
    error.clear();
    return true;
}

bool applyPersistentSnapshot(HANDLE handle,
                             const HidHideSessionSnapshot& snapshot,
                             std::string& error) {
    // Disable hiding during the multi-step persistent update so a partially
    // written whitelist cannot lock HydraSeat out of recovery input. The exact
    // intended active value is published last.
    return setBoolean(handle, kIoctlSetActive, false, error) &&
           setMultiString(handle, kIoctlSetWhitelist,
                          snapshot.allowedApplications, error) &&
           setMultiString(handle, kIoctlSetBlacklist,
                          snapshot.blockedDeviceInstanceIds, error) &&
           setBoolean(handle, kIoctlSetInverse,
                      snapshot.inverseWhitelist, error) &&
           setBoolean(handle, kIoctlSetActive, snapshot.active, error);
}

class NativeHidHideSessionPlatform final : public HidHideSessionPlatform {
public:
    NativeHidHideSessionPlatform() {
        const auto report = probeHidHide();
        verified_ = report.availability == HidHideAvailability::VerifiedSupported;
        sessionSupported_ = verified_ && report.sessionBlacklistSupported;
    }

    bool readState(HidHideSessionSnapshot& snapshot,
                   std::string& error) noexcept override {
        if (!verified_) {
            error = "HidHide native state is not verified supported";
            return false;
        }
        try {
            auto handle = openControl(error);
            if (!handle) return false;
            HidHideSessionSnapshot observed;
            if (!queryBoolean(handle.get(), kIoctlGetActive, observed.active, error) ||
                !queryBoolean(handle.get(), kIoctlGetInverse,
                              observed.inverseWhitelist, error) ||
                !queryMultiString(handle.get(), kIoctlGetBlacklist,
                                  kHidHideSessionMaxDevices,
                                  observed.blockedDeviceInstanceIds, error) ||
                !queryMultiString(handle.get(), kIoctlGetWhitelist,
                                  kHidHideSessionMaxApplications,
                                  observed.allowedApplications, error)) {
                return false;
            }
            snapshot = std::move(observed);
            error.clear();
            return true;
        } catch (...) {
            error = "HidHide native state query raised an unexpected exception";
            return false;
        }
    }

    bool writeState(const HidHideSessionSnapshot& snapshot,
                    std::string& error) noexcept override {
        if (!verified_) {
            error = "HidHide native mutation is not verified supported";
            return false;
        }
        if (snapshot.blockedDeviceInstanceIds.size() > kHidHideSessionMaxDevices ||
            snapshot.allowedApplications.size() > kHidHideSessionMaxApplications) {
            error = "HidHide native mutation snapshot exceeds bounded list counts";
            return false;
        }
        try {
            HidHideSessionSnapshot before;
            if (!readState(before, error)) return false;
            auto handle = openControl(error);
            if (!handle) return false;
            if (!applyPersistentSnapshot(handle.get(), snapshot, error)) {
                const std::string applyError = error;
                std::string restoreError;
                auto restoreHandle = openControl(restoreError);
                bool restored = restoreHandle &&
                    applyPersistentSnapshot(restoreHandle.get(), before, restoreError);
                HidHideSessionSnapshot verifiedRestore;
                if (restored) {
                    restored = readState(verifiedRestore, restoreError) &&
                        equivalentHidHideSessionSnapshots(verifiedRestore, before);
                }
                error = applyError;
                if (restored) {
                    error += "; prior persistent state restored internally";
                } else {
                    error += "; internal restore failed: " + restoreError;
                }
                return false;
            }
            error.clear();
            return true;
        } catch (...) {
            error = "HidHide native state update raised an unexpected exception";
            return false;
        }
    }

    bool addSessionBlacklist(
        std::span<const std::wstring> deviceInstanceIds,
        std::string& error) noexcept override {
        if (!sessionSupported_) {
            error = "HidHide process-lifetime session blacklist is not verified supported";
            return false;
        }
        if (deviceInstanceIds.empty() ||
            deviceInstanceIds.size() > kHidHideSessionMaxRequestedDevices) {
            error = "HidHide session blacklist request is empty or exceeds the bound";
            return false;
        }
        try {
            auto handle = openControl(error);
            if (!handle) return false;
            return setMultiString(handle.get(), kIoctlAddSessionBlacklist,
                                  deviceInstanceIds, error);
        } catch (...) {
            error = "HidHide session blacklist update raised an unexpected exception";
            return false;
        }
    }

    bool clearSessionBlacklist(std::string& error) noexcept override {
        if (!sessionSupported_) {
            error = "HidHide process-lifetime session blacklist is not verified supported";
            return false;
        }
        try {
            auto handle = openControl(error);
            if (!handle) return false;
            DWORD returned = 0;
            if (DeviceIoControl(handle.get(), kIoctlClearSessionBlacklist,
                                nullptr, 0, nullptr, 0, &returned,
                                nullptr) == FALSE) {
                error = winError("HidHide session blacklist clear failed", GetLastError());
                return false;
            }
            error.clear();
            return true;
        } catch (...) {
            error = "HidHide session blacklist clear raised an unexpected exception";
            return false;
        }
    }

    bool mutationSupported() const noexcept override { return verified_; }
    bool sessionBlacklistSupported() const noexcept override {
        return sessionSupported_;
    }

private:
    bool verified_{false};
    bool sessionSupported_{false};
};

#else

class NativeHidHideSessionPlatform final : public HidHideSessionPlatform {
public:
    bool readState(HidHideSessionSnapshot&, std::string& error) noexcept override {
        error = "HidHide is available only on Windows";
        return false;
    }
    bool writeState(const HidHideSessionSnapshot&, std::string& error) noexcept override {
        error = "HidHide native mutation is unavailable on this platform";
        return false;
    }
    bool addSessionBlacklist(std::span<const std::wstring>,
                             std::string& error) noexcept override {
        error = "HidHide session blacklist is unavailable on this platform";
        return false;
    }
    bool clearSessionBlacklist(std::string& error) noexcept override {
        error = "HidHide session blacklist is unavailable on this platform";
        return false;
    }
    bool mutationSupported() const noexcept override { return false; }
    bool sessionBlacklistSupported() const noexcept override { return false; }
};

#endif

} // namespace

std::shared_ptr<HidHideSessionPlatform> makeNativeHidHideSessionPlatform() {
    return std::make_shared<NativeHidHideSessionPlatform>();
}

} // namespace hydra
