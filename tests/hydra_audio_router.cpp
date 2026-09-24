#include "../src/windows_audio_router.hpp"
#include "hydra/audio_endpoint_inventory.hpp"
#include "hydra/audio_session_observer.hpp"

#include <iostream>
#include <string>
#include <windows.h>
#include <objbase.h>
#include <roapi.h>

void printUsage() {
    std::cout << "Usage:" << std::endl;
    std::cout << "  hydra_audio_router --list" << std::endl;
    std::cout << "  hydra_audio_router --inspect <PID>" << std::endl;
    std::cout << "  hydra_audio_router --route <PID> <EndpointID>" << std::endl;
    std::cout << "  hydra_audio_router --reset <PID>" << std::endl;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "COM/WinRT initialization failed." << std::endl;
        return 1;
    }

    std::string command = argv[1];

    if (command == "--list") {
        auto inventory = hydra::windows::AudioEndpointInventory::enumerateRenderEndpoints();
        if (!inventory.isSuccess()) {
            std::cerr << "Failed to enumerate endpoints." << std::endl;
            return 1;
        }

        std::wcout << L"Available Endpoints:" << std::endl;
        for (const auto& ep : *inventory.endpoints) {
            std::wcout << L"  ID: " << ep.endpointId << std::endl;
            std::wcout << L"  Name: " << ep.friendlyName << std::endl;
            std::wcout << L"  State: " << (ep.state == hydra::windows::AudioEndpointState::Active ? L"Active" : L"Inactive") << std::endl;
            std::wcout << L"  -----------------------" << std::endl;
        }
    }
    else if (command == "--inspect") {
        if (argc < 3) {
            std::cerr << "Missing PID." << std::endl;
            printUsage();
            return 1;
        }

        DWORD pid = std::stoul(argv[2]);
        auto result = hydra::windows::AudioSessionObserver::enumerateSessions();
        if (!result.isSuccess()) {
            std::cerr << "Failed to enumerate sessions." << std::endl;
            return 1;
        }

        std::wcout << L"Inspecting audio sessions for PID: " << pid << std::endl;
        bool found = false;
        for (const auto& session : result.sessions) {
            if (session.processId == pid) {
                found = true;
                std::wcout << L"  Process Identity (CreationTime): "
                           << (session.processIdentity ? std::to_wstring(session.processIdentity->creationIdentity) : L"<Unknown>") << std::endl;
                std::wcout << L"  Endpoint ID: " << (session.endpointId.empty() ? L"<Empty>" : session.endpointId) << std::endl;
                std::wcout << L"  State: " << static_cast<int>(session.state) << std::endl;
                std::wcout << L"  Display Name: " << (session.displayName ? *session.displayName : L"<None>") << std::endl;
                std::wcout << L"  -----------------------" << std::endl;
            }
        }
        if (!found) {
            std::wcout << L"  No audio sessions found for this PID." << std::endl;
        }
    }
    else if (command == "--route") {
        if (argc < 4) {
            std::cerr << "Missing PID or EndpointID." << std::endl;
            printUsage();
            return 1;
        }

        DWORD pid = std::stoul(argv[2]);
        std::string endpointIdStr = argv[3];
        std::wstring targetEndpoint(endpointIdStr.begin(), endpointIdStr.end());

        std::wcout << L"Attempting to route PID " << pid << L" to endpoint..." << std::endl;
        
        auto sessionResult = hydra::windows::AudioSessionObserver::enumerateSessions();
        uint64_t creationId = 0;
        if (sessionResult.isSuccess()) {
            for (const auto& session : sessionResult.sessions) {
                if (session.processId == pid && session.processIdentity) {
                    creationId = session.processIdentity->creationIdentity;
                    break;
                }
            }
        }
        
        hydra::runtime::ProcessIdentity procIdentity{pid, creationId};
        hydra::runtime::AudioEndpointIdentity epIdentity{targetEndpoint, std::nullopt};
        
        hydra::windows::WindowsAudioRouter router;
        auto routeStatus = router.assignEndpoint(procIdentity, epIdentity);
        
        if (routeStatus == hydra::runtime::AudioRouteStatus::Success) {
            std::wcout << L"SUCCESS - Note: This is an API-level success." << std::endl;
        } else {
            std::wcerr << L"FAILED (Status: " << static_cast<int>(routeStatus) << L")" << std::endl;
        }
    }
    else if (command == "--reset") {
        if (argc < 3) {
            std::cerr << "Missing PID." << std::endl;
            printUsage();
            return 1;
        }
        
        DWORD pid = std::stoul(argv[2]);
        std::wcout << L"Attempting to reset PID " << pid << L" to default audio routing..." << std::endl;

        auto sessionResult = hydra::windows::AudioSessionObserver::enumerateSessions();
        uint64_t creationId = 0;
        if (sessionResult.isSuccess()) {
            for (const auto& session : sessionResult.sessions) {
                if (session.processId == pid && session.processIdentity) {
                    creationId = session.processIdentity->creationIdentity;
                    break;
                }
            }
        }

        hydra::runtime::ProcessIdentity procIdentity{pid, creationId};
        hydra::windows::WindowsAudioRouter router;
        auto resetStatus = router.clearAssignment(procIdentity);
        
        if (resetStatus == hydra::runtime::AudioRouteStatus::Success) {
            std::wcout << L"SUCCESS - Note: This is an API-level success." << std::endl;
        } else {
            std::wcerr << L"FAILED (Status: " << static_cast<int>(resetStatus) << L")" << std::endl;
        }
    }
    else {
        std::cerr << "Unknown command: " << command << std::endl;
        printUsage();
    }

    RoUninitialize();
    return 0;
}
