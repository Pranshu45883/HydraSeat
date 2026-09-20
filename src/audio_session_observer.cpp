#include "hydra/audio_session_observer.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>

namespace hydra::windows {

// Simple RAII wrapper for COM interfaces to avoid escaping raw pointers.
template <typename T>
struct ComPtr {
    T* ptr{nullptr};

    ~ComPtr() {
        if (ptr) {
            ptr->Release();
        }
    }

    T** operator&() {
        return &ptr;
    }

    T* operator->() {
        return ptr;
    }

    explicit operator bool() const {
        return ptr != nullptr;
    }
};

// RAII wrapper for task memory allocated strings.
struct ScopedCoTaskMem {
    LPWSTR str{nullptr};

    ~ScopedCoTaskMem() {
        if (str) {
            CoTaskMemFree(str);
        }
    }

    LPWSTR* operator&() {
        return &str;
    }
};

// RAII wrapper for Win32 HANDLE
struct ScopedHandle {
    HANDLE handle{NULL};

    ~ScopedHandle() {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

static AudioSessionState mapSessionState(::AudioSessionState state) {
    switch (state) {
    case AudioSessionStateActive:
        return AudioSessionState::Active;
    case AudioSessionStateInactive:
        return AudioSessionState::Inactive;
    case AudioSessionStateExpired:
        return AudioSessionState::Expired;
    default:
        return AudioSessionState::Unknown;
    }
}

// Safely attempts to resolve the process creation identity from a PID.
static std::optional<hydra::runtime::ProcessIdentity> resolveProcessIdentity(std::uint32_t pid) {
    if (pid == 0) {
        return std::nullopt;
    }

    // Open process with minimum required rights to query process times.
    ScopedHandle hProcess;
    hProcess.handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess.handle) {
        return std::nullopt;
    }

    FILETIME creationTime = {0}, exitTime = {0}, kernelTime = {0}, userTime = {0};
    if (!GetProcessTimes(hProcess.handle, &creationTime, &exitTime, &kernelTime, &userTime)) {
        return std::nullopt;
    }

    std::uint64_t creationId = (static_cast<std::uint64_t>(creationTime.dwHighDateTime) << 32) |
                               static_cast<std::uint64_t>(creationTime.dwLowDateTime);

    hydra::runtime::ProcessIdentity identity{pid, creationId};
    if (identity.valid()) {
        return identity;
    }

    return std::nullopt;
}



AudioSessionInventoryResult AudioSessionObserver::enumerateSessions() {
    AudioSessionInventoryResult result;
    result.isComplete = true;

    ComPtr<IMMDeviceEnumerator> pEnumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void**)&pEnumerator);

    if (hr == CO_E_NOTINITIALIZED) {
        result.error = AudioSessionObserverError{AudioSessionObserverError::Code::ComNotInitialized, hr};
        return result;
    }
    if (FAILED(hr) || !pEnumerator) {
        result.error = AudioSessionObserverError{AudioSessionObserverError::Code::EnumeratorCreationFailed, hr};
        return result;
    }

    ComPtr<IMMDeviceCollection> pCollection;
    // We enumerate all endpoints to ensure we don't miss sessions on disconnected/disabled devices.
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &pCollection);
    if (FAILED(hr) || !pCollection) {
        result.error = AudioSessionObserverError{AudioSessionObserverError::Code::EndpointEnumerationFailed, hr};
        return result;
    }

    UINT endpointCount = 0;
    hr = pCollection->GetCount(&endpointCount);
    if (FAILED(hr)) {
        result.error = AudioSessionObserverError{AudioSessionObserverError::Code::CollectionReadFailed, hr};
        return result;
    }

    std::vector<AudioSessionObservation> sessions;

    for (UINT i = 0; i < endpointCount; ++i) {
        ComPtr<IMMDevice> pEndpoint;
        hr = pCollection->Item(i, &pEndpoint);
        if (FAILED(hr) || !pEndpoint) {
            result.isComplete = false;
            continue;
        }

        ScopedCoTaskMem pwszID;
        hr = pEndpoint->GetId(&pwszID);
        if (FAILED(hr) || !pwszID.str) {
            result.isComplete = false;
            continue;
        }
        std::wstring endpointId(pwszID.str);

        std::optional<std::wstring> endpointStableId = std::nullopt;
#ifdef HYDRA_HAS_PKEY_AUDIOENDPOINT_STABLEID
        ComPtr<IPropertyStore> pProps;
        if (SUCCEEDED(pEndpoint->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
            PROPVARIANT varStableId;
            PropVariantInit(&varStableId);
            if (SUCCEEDED(pProps->GetValue(PKEY_AudioEndpoint_StableId, &varStableId)) && varStableId.vt == VT_LPWSTR && varStableId.pwszVal) {
                endpointStableId = varStableId.pwszVal;
            }
            PropVariantClear(&varStableId);
        }
#endif

        ComPtr<IAudioSessionManager2> pSessionManager;
        hr = pEndpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&pSessionManager);
        if (FAILED(hr)) {
            // Some endpoints (e.g., disconnected or unsupported) may fail to activate the session manager.
            // This is expected Windows behavior; we continue to the next endpoint but mark as incomplete.
            result.isComplete = false;
            continue;
        }

        ComPtr<IAudioSessionEnumerator> pSessionEnum;
        hr = pSessionManager->GetSessionEnumerator(&pSessionEnum);
        if (FAILED(hr) || !pSessionEnum) {
            result.isComplete = false;
            continue;
        }

        int sessionCount = 0;
        hr = pSessionEnum->GetCount(&sessionCount);
        if (FAILED(hr)) {
            result.isComplete = false;
            continue;
        }

        for (int j = 0; j < sessionCount; ++j) {
            ComPtr<IAudioSessionControl> pSessionControl;
            hr = pSessionEnum->GetSession(j, &pSessionControl);
            if (FAILED(hr) || !pSessionControl) {
                result.isComplete = false;
                continue;
            }

            ComPtr<IAudioSessionControl2> pSessionControl2;
            hr = pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2);
            if (FAILED(hr) || !pSessionControl2) {
                // Skipping a session makes absence non-authoritative.
                result.isComplete = false;
                continue;
            }

            DWORD pid = 0;
            hr = pSessionControl2->GetProcessId(&pid);
            if (FAILED(hr)) {
                // If we cannot get the PID, the session observation is fundamentally incomplete.
                result.isComplete = false;
                continue;
            }

            ::AudioSessionState rawState;
            hr = pSessionControl->GetState(&rawState);
            AudioSessionState mappedState = AudioSessionState::Unknown;
            if (SUCCEEDED(hr)) {
                mappedState = mapSessionState(rawState);
            }

            std::optional<std::wstring> optDisplayName;
            ScopedCoTaskMem displayNameStr;
            if (SUCCEEDED(pSessionControl->GetDisplayName(&displayNameStr)) && displayNameStr.str) {
                optDisplayName = std::wstring(displayNameStr.str);
            }

            std::optional<std::wstring> optGroupingParam;
            GUID groupingParamGuid;
            if (SUCCEEDED(pSessionControl->GetGroupingParam(&groupingParamGuid))) {
                LPOLESTR guidString = nullptr;
                if (SUCCEEDED(StringFromCLSID(groupingParamGuid, &guidString)) && guidString) {
                    optGroupingParam = std::wstring(guidString);
                    CoTaskMemFree(guidString);
                }
            }

            // Resolve creation identity securely.
            auto processIdentity = resolveProcessIdentity(pid);

            sessions.push_back({
                endpointId,
                endpointStableId,
                pid,
                std::move(processIdentity),
                mappedState,
                std::move(optDisplayName),
                std::move(optGroupingParam)
            });
        }
    }

    result.sessions = std::move(sessions);
    return result;
}

} // namespace hydra::windows
