#include "hydra/audio_endpoint_inventory.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <unordered_set>

namespace hydra::windows {


// Helper to safely map Windows device states to our focused inventory state.
static AudioEndpointState mapDeviceState(DWORD dwState) {
    switch (dwState) {
    case DEVICE_STATE_ACTIVE:
        return AudioEndpointState::Active;
    case DEVICE_STATE_DISABLED:
        return AudioEndpointState::Disabled;
    case DEVICE_STATE_NOTPRESENT:
        return AudioEndpointState::NotPresent;
    case DEVICE_STATE_UNPLUGGED:
        return AudioEndpointState::Unplugged;
    default:
        return AudioEndpointState::Unknown;
    }
}

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

// RAII wrapper for PROPVARIANT to ensure proper cleanup.
struct ScopedPropVariant {
    PROPVARIANT var;

    ScopedPropVariant() {
        PropVariantInit(&var);
    }

    ~ScopedPropVariant() {
        PropVariantClear(&var);
    }

    PROPVARIANT* get() {
        return &var;
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

AudioInventoryResult AudioEndpointInventory::enumerateRenderEndpoints() {
    AudioInventoryResult result;

    // We do not initialize COM globally here. We expect the caller's thread to have an active COM apartment.
    // If CoCreateInstance returns CO_E_NOTINITIALIZED, we report it explicitly.
    ComPtr<IMMDeviceEnumerator> pEnumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void**)&pEnumerator);

    if (hr == CO_E_NOTINITIALIZED) {
        result.error = AudioEndpointInventoryError{AudioEndpointInventoryError::Code::ComNotInitialized, hr};
        return result;
    }
    if (FAILED(hr) || !pEnumerator) {
        result.error = AudioEndpointInventoryError{AudioEndpointInventoryError::Code::EnumeratorCreationFailed, hr};
        return result;
    }

    ComPtr<IMMDeviceCollection> pCollection;
    // DEVICE_STATEMASK_ALL is intentional to expose availability rather than silently hiding endpoints.
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &pCollection);
    if (FAILED(hr) || !pCollection) {
        result.error = AudioEndpointInventoryError{AudioEndpointInventoryError::Code::EnumerationFailed, hr};
        return result;
    }

    UINT count = 0;
    hr = pCollection->GetCount(&count);
    if (FAILED(hr)) {
        result.error = AudioEndpointInventoryError{AudioEndpointInventoryError::Code::CollectionReadFailed, hr};
        return result;
    }

    std::vector<AudioRenderEndpoint> endpoints;
    endpoints.reserve(count);
    std::unordered_set<std::wstring> seenEndpointIds;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> pEndpoint;
        hr = pCollection->Item(i, &pEndpoint);
        if (FAILED(hr) || !pEndpoint) {
            result.error = AudioEndpointInventoryError{AudioEndpointInventoryError::Code::EndpointReadFailed, hr};
            return result;
        }

        ScopedCoTaskMem pwszID;
        hr = pEndpoint->GetId(&pwszID);
        if (FAILED(hr) || !pwszID.str) {
            // Endpoint identity cannot be established. Fail closed.
            result.error = AudioEndpointInventoryError{AudioEndpointInventoryError::Code::EndpointReadFailed, hr};
            return result;
        }

        std::wstring endpointId(pwszID.str);
        
        // Ensure no duplicate endpoints are present in the snapshot.
        if (seenEndpointIds.count(endpointId) > 0) {
            result.error = AudioEndpointInventoryError{AudioEndpointInventoryError::Code::UnexpectedWindowsState, E_UNEXPECTED};
            return result;
        }
        seenEndpointIds.insert(endpointId);

        DWORD dwState = 0;
        hr = pEndpoint->GetState(&dwState);
        AudioEndpointState mappedState = AudioEndpointState::Unknown;
        if (SUCCEEDED(hr)) {
            mappedState = mapDeviceState(dwState);
        } else {
            // State read failure. Do not assume 'Active'.
            mappedState = AudioEndpointState::Unknown;
        }

        ComPtr<IPropertyStore> pProps;
        hr = pEndpoint->OpenPropertyStore(STGM_READ, &pProps);
        
        std::wstring friendlyName = L"";
        std::optional<std::wstring> stableId = std::nullopt;

        if (SUCCEEDED(hr) && pProps) {
            ScopedPropVariant varName;
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, varName.get())) && varName.var.vt == VT_LPWSTR && varName.var.pwszVal) {
                friendlyName = varName.var.pwszVal;
            }

#ifdef HYDRA_HAS_PKEY_AUDIOENDPOINT_STABLEID
            ScopedPropVariant varStableId;
            if (SUCCEEDED(pProps->GetValue(PKEY_AudioEndpoint_StableId, varStableId.get())) && varStableId.var.vt == VT_LPWSTR && varStableId.var.pwszVal) {
                stableId = varStableId.var.pwszVal;
            }
#endif
        }

        endpoints.push_back({
            std::move(endpointId),
            std::move(stableId),
            std::move(friendlyName),
            mappedState
        });
    }

    result.endpoints = std::move(endpoints);
    return result;
}

} // namespace hydra::windows
