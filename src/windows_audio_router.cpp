#include "windows_audio_router.hpp"
#include "hydra/audio_session_observer.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <roapi.h>
#include <winstring.h>
#include <combaseapi.h>
#include <inspectable.h>

namespace hydra::windows {

// RAII wrapper for COM interfaces
template <typename T>
struct ComPtr {
    T* ptr{nullptr};
    ~ComPtr() { if (ptr) ptr->Release(); }
    T** operator&() { return &ptr; }
    T* operator->() { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

// RAII wrapper for HSTRING
struct ScopedHString {
    HSTRING hstr{nullptr};
    ScopedHString(const std::wstring& str) {
        WindowsCreateString(str.c_str(), static_cast<UINT32>(str.length()), &hstr);
    }
    ~ScopedHString() {
        if (hstr) WindowsDeleteString(hstr);
    }
    operator HSTRING() const { return hstr; }
};

// Undocumented Windows 10/11 interface for per-app audio routing.
MIDL_INTERFACE("2a59116d-6c4f-45e0-a74f-707e3fef9258")
IAudioPolicyConfigFactoryDownlevel : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE dummy1() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy2() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy3() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy4() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy5() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy6() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy7() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy8() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy9() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy10() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy11() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy12() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy13() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy14() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy15() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy16() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy17() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy18() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy19() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(DWORD processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(DWORD processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

MIDL_INTERFACE("ab3d4648-e242-459f-b02f-541c70306324")
IAudioPolicyConfigFactory21H2 : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE dummy1() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy2() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy3() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy4() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy5() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy6() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy7() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy8() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy9() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy10() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy11() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy12() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy13() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy14() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy15() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy16() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy17() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy18() = 0;
    virtual HRESULT STDMETHODCALLTYPE dummy19() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(DWORD processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(DWORD processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

static std::wstring resolveRenderDeviceInterfacePath(const std::wstring& endpointId) {
    return L"\\\\?\\SWD#MMDEVAPI#" + endpointId + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
}

static hydra::runtime::AudioRouteStatus validateProcessIdentity(const hydra::runtime::ProcessIdentity& process) {
    if (!process.valid()) return hydra::runtime::AudioRouteStatus::InvalidProcess;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid);
    if (!hProcess) {
        return hydra::runtime::AudioRouteStatus::ProcessNotFound;
    }
    CloseHandle(hProcess);

    auto result = AudioSessionObserver::enumerateSessions();
    if (!result.isSuccess()) {
        return hydra::runtime::AudioRouteStatus::OsApiError;
    }

    bool hasSession = false;
    for (const auto& session : result.sessions) {
        if (session.processId == process.pid) {
            hasSession = true;
            if (session.processIdentity && hydra::runtime::matchIdentity(session.processIdentity, process) != hydra::runtime::ProcessOwnershipMatch::Match) {
                return hydra::runtime::AudioRouteStatus::IdentityMismatch;
            }
        }
    }

    if (!hasSession) {
        return hydra::runtime::AudioRouteStatus::AudioSessionNotFound;
    }

    return hydra::runtime::AudioRouteStatus::Success;
}

static hydra::runtime::AudioRouteStatus validateEndpointExists(const std::wstring& endpointId) {
    ComPtr<IMMDeviceEnumerator> pEnumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) return hydra::runtime::AudioRouteStatus::OsApiError;

    ComPtr<IMMDevice> pDevice;
    hr = pEnumerator->GetDevice(endpointId.c_str(), &pDevice);
    if (FAILED(hr) || !pDevice) return hydra::runtime::AudioRouteStatus::EndpointNotFound;

    DWORD state = 0;
    hr = pDevice->GetState(&state);
    if (FAILED(hr)) return hydra::runtime::AudioRouteStatus::OsApiError;

    if (!(state & DEVICE_STATE_ACTIVE)) {
        return hydra::runtime::AudioRouteStatus::EndpointUnavailable;
    }

    return hydra::runtime::AudioRouteStatus::Success;
}

static hydra::runtime::AudioRouteStatus callAudioPolicyConfigFactory(DWORD pid, const std::wstring& deviceIdStr) {
    ScopedHString className(L"Windows.Media.Internal.AudioPolicyConfig");
    ScopedHString deviceId(deviceIdStr);

    ComPtr<IInspectable> factoryBase;
    HRESULT hr = RoGetActivationFactory(className, __uuidof(IInspectable), (void**)&factoryBase);
    if (FAILED(hr) || !factoryBase) return hydra::runtime::AudioRouteStatus::OsApiError;

    ComPtr<IAudioPolicyConfigFactory21H2> factory21H2;
    ComPtr<IAudioPolicyConfigFactoryDownlevel> factoryDownlevel;

    bool is21H2 = SUCCEEDED(factoryBase->QueryInterface(__uuidof(IAudioPolicyConfigFactory21H2), (void**)&factory21H2));
    if (!is21H2) {
        if (FAILED(factoryBase->QueryInterface(__uuidof(IAudioPolicyConfigFactoryDownlevel), (void**)&factoryDownlevel))) {
            return hydra::runtime::AudioRouteStatus::OsApiError;
        }
    }

    if (is21H2) {
        hr = factory21H2->SetPersistedDefaultAudioEndpoint(pid, eRender, eConsole, deviceId);
    } else {
        hr = factoryDownlevel->SetPersistedDefaultAudioEndpoint(pid, eRender, eConsole, deviceId);
    }

    return SUCCEEDED(hr) ? hydra::runtime::AudioRouteStatus::Success : hydra::runtime::AudioRouteStatus::RoutingFailed;
}

hydra::runtime::AudioRouteStatus WindowsAudioRouter::assignEndpoint(
    const hydra::runtime::ProcessIdentity& process,
    const hydra::runtime::AudioEndpointIdentity& endpoint) noexcept 
{
    if (!endpoint.valid()) return hydra::runtime::AudioRouteStatus::EndpointNotFound;

    auto pStatus = validateProcessIdentity(process);
    if (pStatus != hydra::runtime::AudioRouteStatus::Success) return pStatus;

    auto eStatus = validateEndpointExists(endpoint.endpointId);
    if (eStatus != hydra::runtime::AudioRouteStatus::Success) return eStatus;

    std::wstring pnpInterfacePath = resolveRenderDeviceInterfacePath(endpoint.endpointId);
    return callAudioPolicyConfigFactory(process.pid, pnpInterfacePath);
}

hydra::runtime::AudioRouteStatus WindowsAudioRouter::clearAssignment(
    const hydra::runtime::ProcessIdentity& process) noexcept 
{
    auto pStatus = validateProcessIdentity(process);
    if (pStatus != hydra::runtime::AudioRouteStatus::Success) return pStatus;

    return callAudioPolicyConfigFactory(process.pid, L"");
}

} // namespace hydra::windows
