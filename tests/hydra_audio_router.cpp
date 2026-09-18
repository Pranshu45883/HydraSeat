#include "hydra/audio_routing_experiment.hpp"
#include "hydra/audio_endpoint_inventory.hpp"

#include <iostream>
#include <string>
#include <windows.h>
#include <objbase.h>

void printUsage() {
    std::cout << "Usage:" << std::endl;
    std::cout << "  hydra_audio_router --list" << std::endl;
    std::cout << "  hydra_audio_router --route <PID> <EndpointID>" << std::endl;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "COM initialization failed." << std::endl;
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
    else if (command == "--route") {
        if (argc < 4) {
            std::cerr << "Missing PID or EndpointID." << std::endl;
            printUsage();
            return 1;
        }

        DWORD pid = std::stoul(argv[2]);
        std::string endpointIdStr = argv[3];
        std::wstring targetEndpoint(endpointIdStr.begin(), endpointIdStr.end());

        std::cout << "Attempting to route PID " << pid << " to endpoint..." << std::endl;
        HRESULT routeHr = hydra::windows::AudioRoutingExperiment::manualRoute(pid, targetEndpoint);
        if (SUCCEEDED(routeHr)) {
            std::cout << "SUCCESS (HRESULT: " << std::hex << routeHr << ")" << std::endl;
        } else {
            std::cerr << "FAILED (HRESULT: " << std::hex << routeHr << ")" << std::endl;
        }
    }
    else {
        std::cerr << "Unknown command." << std::endl;
        printUsage();
    }

    CoUninitialize();
    return 0;
}
