#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <iostream>
#include <string>
#include <cmath>
#include <cstdint>

const double PI = 3.14159265358979323846;

template <typename T>
struct ComPtr {
    T* ptr{nullptr};
    ~ComPtr() { if (ptr) ptr->Release(); }
    T** operator&() { return &ptr; }
    T* operator->() { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

void PrintProcessIdentity() {
    DWORD pid = GetCurrentProcessId();
    uint64_t cid = 0;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        FILETIME ct, et, kt, ut;
        if (GetProcessTimes(hProcess, &ct, &et, &kt, &ut)) {
            cid = (static_cast<uint64_t>(ct.dwHighDateTime) << 32) | static_cast<uint64_t>(ct.dwLowDateTime);
        }
        CloseHandle(hProcess);
    }
    
    std::cout << "Process Identity:" << std::endl;
    std::cout << "  PID: " << pid << std::endl;
    std::cout << "  CreationIdentity: " << cid << std::endl;
}

void PrintDefaultEndpoint() {
    ComPtr<IMMDeviceEnumerator> pEnumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) return;

    ComPtr<IMMDevice> pDevice;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) return;

    LPWSTR strId = nullptr;
    if (SUCCEEDED(pDevice->GetId(&strId))) {
        std::wcout << L"  Global Default Endpoint: " << strId << std::endl;
        CoTaskMemFree(strId);
    }
}

int main(int argc, char** argv) {
    double frequency = 440.0; // Default A4
    if (argc > 1) {
        frequency = std::stod(argv[1]);
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return -1;

    PrintProcessIdentity();
    PrintDefaultEndpoint();

    std::cout << "Playing sine wave at " << frequency << " Hz. Press Ctrl+C to stop." << std::endl;

    ComPtr<IMMDeviceEnumerator> pEnumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    
    ComPtr<IMMDevice> pDevice;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);

    ComPtr<IAudioClient> pAudioClient;
    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);

    WAVEFORMATEX* pwfx = nullptr;
    hr = pAudioClient->GetMixFormat(&pwfx);

    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        10000000, // 1 second buffer
        0,
        pwfx,
        nullptr
    );

    ComPtr<IAudioRenderClient> pRenderClient;
    hr = pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient);

    UINT32 bufferFrameCount;
    hr = pAudioClient->GetBufferSize(&bufferFrameCount);

    BYTE* pData;
    hr = pRenderClient->GetBuffer(bufferFrameCount, &pData);

    double phase = 0.0;
    double phaseIncrement = (2.0 * PI * frequency) / pwfx->nSamplesPerSec;

    // Fill initial buffer with silence
    hr = pRenderClient->ReleaseBuffer(bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
    hr = pAudioClient->Start();

    while (true) {
        Sleep(bufferFrameCount * 1000 / pwfx->nSamplesPerSec / 2);
        
        UINT32 numFramesPadding;
        hr = pAudioClient->GetCurrentPadding(&numFramesPadding);
        UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;

        if (numFramesAvailable > 0) {
            hr = pRenderClient->GetBuffer(numFramesAvailable, &pData);
            
            float* pFloatData = (float*)pData;
            for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                float sample = (float)(0.2 * std::sin(phase)); // 20% volume
                for (WORD c = 0; c < pwfx->nChannels; ++c) {
                    *pFloatData++ = sample;
                }
                phase += phaseIncrement;
                if (phase >= 2.0 * PI) phase -= 2.0 * PI;
            }

            hr = pRenderClient->ReleaseBuffer(numFramesAvailable, 0);
        }
    }

    CoTaskMemFree(pwfx);
    CoUninitialize();
    return 0;
}
