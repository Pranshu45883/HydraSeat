#include "hydra/audio_routing_experiment.hpp"
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
        HRESULT routeHr = hydra::windows::AudioRoutingExperiment::manualRoute(pid, targetEndpoint);
        if (SUCCEEDED(routeHr)) {
            std::wcout << L"SUCCESS (HRESULT: 0x" << std::hex << routeHr << L") - Note: This is an API-level success." << std::endl;
        } else {
            std::wcerr << L"FAILED (HRESULT: 0x" << std::hex << routeHr << L")" << std::endl;
        }
    }
    else {
        std::cerr << "Unknown command." << std::endl;
        printUsage();
    }

    RoUninitialize();
    return 0;
}
