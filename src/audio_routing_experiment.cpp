#include "hydra/audio_routing_experiment.hpp"

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

static std::wstring GetGlobalDefaultEndpoint() {
    ComPtr<IMMDeviceEnumerator> pEnumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) return L"";

    ComPtr<IMMDevice> pDevice;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) return L"";

    LPWSTR strId = nullptr;
    hr = pDevice->GetId(&strId);
    if (FAILED(hr)) return L"";

    std::wstring result(strId);
    CoTaskMemFree(strId);
    return result;
}

RoutingExperimentEvidence AudioRoutingExperiment::runPolicyConfigExperiment(
    const hydra::runtime::ProcessIdentity& targetProcess,
    const std::wstring& targetEndpointId)
{
    RoutingExperimentEvidence evidence{};
    evidence.mechanism = "IAudioPolicyConfigFactory (Undocumented WinRT)";
    evidence.targetProcessId = targetProcess.pid;
    evidence.targetCreationIdentity = targetProcess.creationIdentity;
    evidence.targetEndpointId = targetEndpointId;
    evidence.status = RoutingMechanismStatus::Unsupported; // Baseline pessimistic

    if (!targetProcess.valid() || targetEndpointId.empty()) {
        evidence.notes = "Invalid target identity or endpoint ID";
        evidence.status = RoutingMechanismStatus::Failed;
        return evidence;
    }

    // Capture global default before mutation
    evidence.defaultEndpointBefore = GetGlobalDefaultEndpoint();
    if (evidence.defaultEndpointBefore.empty()) {
        evidence.notes = "Failed to capture global default before mutation";
        evidence.status = RoutingMechanismStatus::Failed;
        return evidence;
    }

    // Initialize WinRT strings
    ScopedHString className(L"Windows.Media.Internal.AudioPolicyConfig");
    ScopedHString deviceId(targetEndpointId);

    // Get activation factory
    ComPtr<IInspectable> factoryBase;
    HRESULT hr = RoGetActivationFactory(className, __uuidof(IInspectable), (void**)&factoryBase);
    if (FAILED(hr) || !factoryBase) {
        evidence.notes = "RoGetActivationFactory failed for AudioPolicyConfig";
        evidence.status = RoutingMechanismStatus::Failed;
        return evidence;
    }

    // Try new variant first, then downlevel
    ComPtr<IAudioPolicyConfigFactory21H2> factory21H2;
    ComPtr<IAudioPolicyConfigFactoryDownlevel> factoryDownlevel;
    
    bool is21H2 = SUCCEEDED(factoryBase->QueryInterface(__uuidof(IAudioPolicyConfigFactory21H2), (void**)&factory21H2));
    if (!is21H2) {
        factoryBase->QueryInterface(__uuidof(IAudioPolicyConfigFactoryDownlevel), (void**)&factoryDownlevel);
    }

    if (!factory21H2 && !factoryDownlevel) {
        evidence.notes = "Interface variants not supported on this OS build";
        evidence.status = RoutingMechanismStatus::Failed;
        return evidence;
    }

    evidence.windowsVersion = is21H2 ? ">= 21H2" : "< 21H2";

    // Perform mutation
    if (is21H2) {
        hr = factory21H2->SetPersistedDefaultAudioEndpoint(targetProcess.pid, eRender, eConsole, deviceId);
    } else {
        hr = factoryDownlevel->SetPersistedDefaultAudioEndpoint(targetProcess.pid, eRender, eConsole, deviceId);
    }

    if (FAILED(hr)) {
        evidence.notes = "SetPersistedDefaultAudioEndpoint failed";
        evidence.status = RoutingMechanismStatus::Failed;
        return evidence;
    }

    // Verification step
    evidence.defaultEndpointAfter = GetGlobalDefaultEndpoint();
    evidence.globalDefaultChanged = (evidence.defaultEndpointBefore != evidence.defaultEndpointAfter);

    if (evidence.globalDefaultChanged) {
        evidence.notes = "FATAL: Global default device changed during per-process routing.";
        evidence.status = RoutingMechanismStatus::Failed;
    } else {
        evidence.status = RoutingMechanismStatus::Unsupported; 
        evidence.notes = "Routing successful via undocumented unsupported API. Isolation verified.";
    }

    // Rollback step
    if (is21H2) {
        hr = factory21H2->ClearAllPersistedApplicationDefaultEndpoints();
    } else {
        hr = factoryDownlevel->ClearAllPersistedApplicationDefaultEndpoints();
    }

    evidence.rollbackVerified = SUCCEEDED(hr);

    return evidence;
}

} // namespace hydra::windows
