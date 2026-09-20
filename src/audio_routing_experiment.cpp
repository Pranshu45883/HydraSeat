#include "hydra/audio_routing_experiment.hpp"
#include "hydra/audio_session_observer.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <roapi.h>
#include <winstring.h>
#include <combaseapi.h>
#include <inspectable.h>

namespace hydra::windows {

template <typename T>
struct ComPtr {
    T* ptr{nullptr};
    ~ComPtr() { if (ptr) ptr->Release(); }
    T** operator&() { return &ptr; }
    T* operator->() { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

struct ScopedHandle {
    HANDLE handle{nullptr};
    ~ScopedHandle() {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

struct ScopedHString {
    HSTRING value{nullptr};
    HRESULT hr{E_FAIL};

    explicit ScopedHString(const std::wstring& text) {
        hr = WindowsCreateString(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            &value);
    }

    ~ScopedHString() {
        if (value) WindowsDeleteString(value);
    }

    explicit operator bool() const noexcept {
        return SUCCEEDED(hr) && value != nullptr;
    }
};

struct OwnedHString {
    HSTRING value{nullptr};
    ~OwnedHString() {
        if (value) WindowsDeleteString(value);
    }
};

static std::wstring copyHString(HSTRING value) {
    if (!value) return {};
    UINT32 length = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(value, &length);
    return raw ? std::wstring(raw, length) : std::wstring{};
}

// Undocumented Windows 10/11 interface for per-app audio routing.
// The final method remains declared only to preserve the reverse-engineered
// vtable shape; HydraSeat never calls the global clear operation.
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
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(
        DWORD processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(
        DWORD processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

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
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(
        DWORD processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(
        DWORD processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

static AudioRoutingProbeResult makeResult(
    AudioRoutingProbeStatus status,
    HRESULT hr,
    bool targetVerified = false,
    bool rollbackVerified = false)
{
    return {
        status,
        static_cast<std::int32_t>(hr),
        targetVerified,
        rollbackVerified
    };
}

static std::wstring resolveRenderDeviceInterfacePath(
    const std::wstring& endpointId)
{
    // DEVINTERFACE_AUDIO_RENDER GUID:
    // {e6327cad-dcec-4949-ae8a-991e976a79d2}
    return L"\\\\?\\SWD#MMDEVAPI#" + endpointId +
           L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
}

static bool openExactProcess(
    const hydra::runtime::ProcessIdentity& expected,
    ScopedHandle& process)
{
    if (!expected.valid()) return false;

    process.handle = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        static_cast<DWORD>(expected.pid));
    if (!process.handle) return false;

    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(
            process.handle, &creation, &exit, &kernel, &user)) {
        return false;
    }

    const std::uint64_t creationIdentity =
        (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) |
        static_cast<std::uint64_t>(creation.dwLowDateTime);

    return creationIdentity == expected.creationIdentity;
}

static bool hasExactObservedAudioSession(
    const hydra::runtime::ProcessIdentity& expected)
{
    const auto result = AudioSessionObserver::enumerateSessions();
    if (!result.isSuccess() || !result.isComplete) return false;

    for (const auto& session : result.sessions) {
        if (hydra::runtime::matchIdentity(
                session.processIdentity, expected) ==
            hydra::runtime::ProcessOwnershipMatch::Match) {
            return true;
        }
    }

    return false;
}

static bool validateEndpointExists(const std::wstring& endpointId) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return false;

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDevice(endpointId.c_str(), &device);
    return SUCCEEDED(hr) && device;
}

template <typename Factory>
static AudioRoutingProbeResult probeWithFactory(
    Factory* factory,
    const hydra::runtime::ProcessIdentity& targetProcess,
    const std::wstring& targetEndpointId)
{
    OwnedHString previousRaw;
    HRESULT hr = factory->GetPersistedDefaultAudioEndpoint(
        static_cast<DWORD>(targetProcess.pid),
        eRender,
        eConsole,
        &previousRaw.value);

    // Without an exact prior mapping there is no target-scoped rollback
    // contract that we can prove safe. Refuse to mutate.
    const std::wstring previousMapping =
        SUCCEEDED(hr) ? copyHString(previousRaw.value) : std::wstring{};
    if (FAILED(hr) || previousMapping.empty()) {
        return makeResult(
            AudioRoutingProbeStatus::UnsafeWithoutRestorableBaseline,
            FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    }

    const std::wstring targetMapping =
        resolveRenderDeviceInterfacePath(targetEndpointId);
    ScopedHString target(targetMapping);
    if (!target) {
        return makeResult(
            AudioRoutingProbeStatus::ApplyFailed,
            target.hr);
    }

    hr = factory->SetPersistedDefaultAudioEndpoint(
        static_cast<DWORD>(targetProcess.pid),
        eRender,
        eConsole,
        target.value);
    if (FAILED(hr)) {
        return makeResult(AudioRoutingProbeStatus::ApplyFailed, hr);
    }

    OwnedHString appliedRaw;
    const HRESULT verifyApplyHr =
        factory->GetPersistedDefaultAudioEndpoint(
            static_cast<DWORD>(targetProcess.pid),
            eRender,
            eConsole,
            &appliedRaw.value);
    const bool targetVerified =
        SUCCEEDED(verifyApplyHr) &&
        copyHString(appliedRaw.value) == targetMapping;

    // Restore the exact HSTRING returned by Windows. Reusing the captured
    // value avoids any normalization or allocation step during rollback.
    const HRESULT restoreHr =
        factory->SetPersistedDefaultAudioEndpoint(
            static_cast<DWORD>(targetProcess.pid),
            eRender,
            eConsole,
            previousRaw.value);
    if (FAILED(restoreHr)) {
        return makeResult(
            AudioRoutingProbeStatus::RestoreFailed,
            restoreHr,
            targetVerified,
            false);
    }

    OwnedHString restoredRaw;
    const HRESULT verifyRestoreHr =
        factory->GetPersistedDefaultAudioEndpoint(
            static_cast<DWORD>(targetProcess.pid),
            eRender,
            eConsole,
            &restoredRaw.value);
    const bool rollbackVerified =
        SUCCEEDED(verifyRestoreHr) &&
        copyHString(restoredRaw.value) == previousMapping;

    if (!rollbackVerified) {
        return makeResult(
            AudioRoutingProbeStatus::RestoreVerificationFailed,
            FAILED(verifyRestoreHr) ? verifyRestoreHr : E_FAIL,
            targetVerified,
            false);
    }

    if (!targetVerified) {
        return makeResult(
            AudioRoutingProbeStatus::ApplyVerificationFailed,
            FAILED(verifyApplyHr) ? verifyApplyHr : E_FAIL,
            false,
            true);
    }

    return makeResult(
        AudioRoutingProbeStatus::PersistedPolicyRoundTripVerified,
        S_OK,
        true,
        true);
}

AudioRoutingProbeResult AudioRoutingExperiment::probePersistedRoute(
    const hydra::runtime::ProcessIdentity& targetProcess,
    const std::wstring& targetEndpointId)
{
    ScopedHandle process;
    if (!openExactProcess(targetProcess, process)) {
        return makeResult(
            AudioRoutingProbeStatus::InvalidTargetProcess,
            E_INVALIDARG);
    }

    // Keep the exact process handle alive through apply and rollback so the
    // PID cannot silently become ownership evidence for a different process.
    if (!hasExactObservedAudioSession(targetProcess)) {
        return makeResult(
            AudioRoutingProbeStatus::TargetAudioSessionNotObserved,
            E_INVALIDARG);
    }

    if (targetEndpointId.empty() ||
        !validateEndpointExists(targetEndpointId)) {
        return makeResult(
            AudioRoutingProbeStatus::TargetEndpointUnavailable,
            E_INVALIDARG);
    }

    ScopedHString className(
        L"Windows.Media.Internal.AudioPolicyConfig");
    if (!className) {
        return makeResult(
            AudioRoutingProbeStatus::FactoryUnavailable,
            className.hr);
    }

    ComPtr<IInspectable> factoryBase;
    HRESULT hr = RoGetActivationFactory(
        className.value,
        __uuidof(IInspectable),
        reinterpret_cast<void**>(&factoryBase));
    if (FAILED(hr) || !factoryBase) {
        return makeResult(
            AudioRoutingProbeStatus::FactoryUnavailable,
            hr);
    }

    ComPtr<IAudioPolicyConfigFactory21H2> factory21H2;
    if (SUCCEEDED(factoryBase->QueryInterface(
            __uuidof(IAudioPolicyConfigFactory21H2),
            reinterpret_cast<void**>(&factory21H2))) &&
        factory21H2) {
        return probeWithFactory(
            factory21H2.ptr,
            targetProcess,
            targetEndpointId);
    }

    ComPtr<IAudioPolicyConfigFactoryDownlevel> factoryDownlevel;
    hr = factoryBase->QueryInterface(
        __uuidof(IAudioPolicyConfigFactoryDownlevel),
        reinterpret_cast<void**>(&factoryDownlevel));
    if (FAILED(hr) || !factoryDownlevel) {
        return makeResult(
            AudioRoutingProbeStatus::FactoryUnavailable,
            hr);
    }

    return probeWithFactory(
        factoryDownlevel.ptr,
        targetProcess,
        targetEndpointId);
}

} // namespace hydra::windows
