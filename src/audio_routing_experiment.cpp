#include "hydra/audio_routing_experiment.hpp"
#include "hydra/audio_session_observer.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <roapi.h>
#include <winstring.h>
#include <combaseapi.h>
#include <inspectable.h>
#include <iostream>

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
// Pre-21H2 Variant
MIDL_INTERFACE("2a59116d-6c4f-45e0-a74f-707e3fef9258")
IAudioPolicyConfigFactoryDownlevel : public IInspectable
{
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

// 21H2 and later Variant
MIDL_INTERFACE("ab3d4648-e242-459f-b02f-541c70306324")
IAudioPolicyConfigFactory21H2 : public IInspectable
{
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

// Deterministically constructs the full PnP device interface path required by the undocumented routing API.
// The raw IMMDevice::GetId() string (e.g., "{0.0...}.{GUID}") is rejected with E_INVALIDARG.
// The API mandates the SWD enumerator prefix and the audio render interface class GUID suffix.
static std::wstring resolveRenderDeviceInterfacePath(const std::wstring& endpointId) {
    // DEVINTERFACE_AUDIO_RENDER GUID: {e6327cad-dcec-4949-ae8a-991e976a79d2}
    return L"\\\\?\\SWD#MMDEVAPI#" + endpointId + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
}

// Validates that the process actually exists and has an enumerated audio session.
// This enforces the requirement: "Do not accept an arbitrary PID as proof of ownership."
static bool validateProcessOwnsAudioSession(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) {
        return false;
    }
    CloseHandle(hProcess);

    auto result = AudioSessionObserver::enumerateSessions();
    if (!result.isSuccess()) {
        return false;
    }

    for (const auto& session : result.sessions) {
        if (session.processId == pid) {
            return true;
        }
    }
    return false;
}

// Validates that the endpoint actually exists in the Windows audio system.
static bool validateEndpointExists(const std::wstring& endpointId) {
    ComPtr<IMMDeviceEnumerator> pEnumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) return false;

    ComPtr<IMMDevice> pDevice;
    hr = pEnumerator->GetDevice(endpointId.c_str(), &pDevice);
    return SUCCEEDED(hr) && pDevice;
}

HRESULT AudioRoutingExperiment::manualRoute(DWORD pid, const std::wstring& targetEndpointId) {
    if (!validateProcessOwnsAudioSession(pid)) {
        std::wcerr << L"HydraAudioRouter: Target process PID " << pid << L" does not exist or has no active audio session." << std::endl;
        return E_INVALIDARG;
    }

    if (!validateEndpointExists(targetEndpointId)) {
        std::wcerr << L"HydraAudioRouter: Target endpoint ID '" << targetEndpointId << L"' does not exist." << std::endl;
        return E_INVALIDARG;
    }

    std::wstring pnpInterfacePath = resolveRenderDeviceInterfacePath(targetEndpointId);

    std::wcout << L"HydraAudioRouter: Resolving IMMDevice ID to PnP interface path..." << std::endl;
    std::wcout << L"  Supplied PID: " << pid << std::endl;
    std::wcout << L"  Supplied Endpoint ID: " << targetEndpointId << std::endl;
    std::wcout << L"  Resolved PnP Path:    " << pnpInterfacePath << std::endl;

    ScopedHString className(L"Windows.Media.Internal.AudioPolicyConfig");
    ScopedHString deviceId(pnpInterfacePath);

    ComPtr<IInspectable> factoryBase;
    HRESULT hr = RoGetActivationFactory(className, __uuidof(IInspectable), (void**)&factoryBase);
    if (FAILED(hr) || !factoryBase) return hr;

    ComPtr<IAudioPolicyConfigFactory21H2> factory21H2;
    ComPtr<IAudioPolicyConfigFactoryDownlevel> factoryDownlevel;

    bool is21H2 = SUCCEEDED(factoryBase->QueryInterface(__uuidof(IAudioPolicyConfigFactory21H2), (void**)&factory21H2));
    if (!is21H2) {
        if (FAILED(factoryBase->QueryInterface(__uuidof(IAudioPolicyConfigFactoryDownlevel), (void**)&factoryDownlevel))) {
            return E_NOINTERFACE;
        }
    }

    if (is21H2) {
        return factory21H2->SetPersistedDefaultAudioEndpoint(pid, eRender, eConsole, deviceId);
    } else {
        return factoryDownlevel->SetPersistedDefaultAudioEndpoint(pid, eRender, eConsole, deviceId);
    }
}



} // namespace hydra::windows
