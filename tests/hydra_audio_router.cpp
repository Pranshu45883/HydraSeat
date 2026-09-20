#include "hydra/audio_routing_experiment.hpp"
#include "hydra/audio_endpoint_inventory.hpp"
#include "hydra/audio_session_observer.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <windows.h>
#include <roapi.h>

namespace {

using hydra::runtime::ProcessIdentity;
using hydra::windows::AudioRoutingProbeStatus;

void printUsage() {
    std::cout << "Usage:" << std::endl;
    std::cout << "  HydraAudioRouter --list" << std::endl;
    std::cout << "  HydraAudioRouter --inspect <PID>" << std::endl;
    std::cout << "  HydraAudioRouter --probe <PID> <EndpointID>" << std::endl;
    std::cout << std::endl;
    std::cout
        << "--probe is a manual feasibility check only. It verifies a "
           "persisted per-process policy round trip and exact rollback; "
           "it does NOT prove that a live audio session moved endpoints."
        << std::endl;
}

const char* statusName(AudioRoutingProbeStatus status) {
    switch (status) {
    case AudioRoutingProbeStatus::PersistedPolicyRoundTripVerified:
        return "PERSISTED_POLICY_ROUND_TRIP_VERIFIED";
    case AudioRoutingProbeStatus::InvalidTargetProcess:
        return "INVALID_TARGET_PROCESS";
    case AudioRoutingProbeStatus::TargetAudioSessionNotObserved:
        return "TARGET_AUDIO_SESSION_NOT_OBSERVED";
    case AudioRoutingProbeStatus::TargetEndpointUnavailable:
        return "TARGET_ENDPOINT_UNAVAILABLE";
    case AudioRoutingProbeStatus::FactoryUnavailable:
        return "FACTORY_UNAVAILABLE";
    case AudioRoutingProbeStatus::UnsafeWithoutRestorableBaseline:
        return "UNSAFE_WITHOUT_RESTORABLE_BASELINE";
    case AudioRoutingProbeStatus::ApplyFailed:
        return "APPLY_FAILED";
    case AudioRoutingProbeStatus::ApplyVerificationFailed:
        return "APPLY_VERIFICATION_FAILED";
    case AudioRoutingProbeStatus::RestoreFailed:
        return "RESTORE_FAILED";
    case AudioRoutingProbeStatus::RestoreVerificationFailed:
        return "RESTORE_VERIFICATION_FAILED";
    }
    return "UNKNOWN";
}

std::optional<ProcessIdentity> findExactObservedIdentity(
    std::uint32_t pid,
    const hydra::windows::AudioSessionInventoryResult& sessions)
{
    std::optional<ProcessIdentity> identity;

    for (const auto& session : sessions.sessions) {
        if (session.processId != pid || !session.processIdentity ||
            !session.processIdentity->valid()) {
            continue;
        }

        if (!identity) {
            identity = session.processIdentity;
            continue;
        }

        if (*identity != *session.processIdentity) {
            return std::nullopt;
        }
    }

    return identity;
}

int runInspect(std::uint32_t pid) {
    const auto result =
        hydra::windows::AudioSessionObserver::enumerateSessions();
    if (!result.isSuccess()) {
        std::cerr << "Failed to enumerate sessions." << std::endl;
        return 1;
    }

    std::cout << "Enumeration completeness: "
              << (result.isComplete ? "COMPLETE" : "PARTIAL")
              << std::endl;
    std::wcout << L"Inspecting audio sessions for PID: " << pid << std::endl;

    bool found = false;
    for (const auto& session : result.sessions) {
        if (session.processId != pid) {
            continue;
        }

        found = true;
        std::wcout << L"  Process Identity (CreationTime): "
                   << (session.processIdentity
                           ? std::to_wstring(
                                 session.processIdentity->creationIdentity)
                           : L"<Unknown>")
                   << std::endl;
        std::wcout << L"  Endpoint ID: "
                   << (session.endpointId.empty()
                           ? L"<Empty>"
                           : session.endpointId)
                   << std::endl;
        std::wcout << L"  State: "
                   << static_cast<int>(session.state)
                   << std::endl;
        std::wcout << L"  Display Name: "
                   << (session.displayName
                           ? *session.displayName
                           : L"<None>")
                   << std::endl;
        std::wcout << L"  -----------------------" << std::endl;
    }

    if (!found) {
        std::wcout << L"  No audio sessions found for this PID." << std::endl;
    }

    if (!result.isComplete) {
        std::cout
            << "WARNING: observation was partial; absence is not authoritative."
            << std::endl;
    }

    return 0;
}

int runProbe(std::uint32_t pid, const std::wstring& targetEndpoint) {
    const auto observed =
        hydra::windows::AudioSessionObserver::enumerateSessions();
    if (!observed.isSuccess()) {
        std::cerr << "Failed to enumerate sessions; refusing mutation."
                  << std::endl;
        return 2;
    }

    if (!observed.isComplete) {
        std::cerr
            << "Session observation is partial; refusing to treat it as "
               "authoritative ownership evidence."
            << std::endl;
        return 2;
    }

    const auto identity = findExactObservedIdentity(pid, observed);
    if (!identity) {
        std::cerr
            << "No single valid exact process identity was observed for PID "
            << pid << "; refusing mutation."
            << std::endl;
        return 2;
    }

    const auto probe =
        hydra::windows::AudioRoutingExperiment::probePersistedRoute(
            *identity,
            targetEndpoint);

    std::cout << "Probe status: " << statusName(probe.status) << std::endl;
    std::cout << "Target persisted policy verified: "
              << (probe.targetPolicyVerified ? "yes" : "no")
              << std::endl;
    std::cout << "Exact prior policy restored: "
              << (probe.rollbackVerified ? "yes" : "no")
              << std::endl;
    std::cout << "HRESULT: 0x"
              << std::hex
              << static_cast<std::uint32_t>(probe.hresult)
              << std::dec
              << std::endl;

    if (probe.status ==
        AudioRoutingProbeStatus::PersistedPolicyRoundTripVerified) {
        std::cout
            << "Persisted policy round trip verified. "
               "ACTIVE AUDIO ROUTING IS STILL UNVERIFIED."
            << std::endl;
        return 0;
    }

    if (probe.status ==
        AudioRoutingProbeStatus::UnsafeWithoutRestorableBaseline) {
        std::cout
            << "No exact restorable per-process baseline was available; "
               "no routing mutation was attempted."
            << std::endl;
    }

    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    const HRESULT initHr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initHr)) {
        std::cerr << "COM/WinRT initialization failed: 0x"
                  << std::hex
                  << static_cast<std::uint32_t>(initHr)
                  << std::dec
                  << std::endl;
        return 1;
    }

    int exitCode = 0;
    const std::string command = argv[1];

    if (command == "--list") {
        const auto inventory =
            hydra::windows::AudioEndpointInventory::enumerateRenderEndpoints();
        if (!inventory.isSuccess()) {
            std::cerr << "Failed to enumerate endpoints." << std::endl;
            exitCode = 1;
        } else {
            std::wcout << L"Available Endpoints:" << std::endl;
            for (const auto& ep : *inventory.endpoints) {
                std::wcout << L"  ID: " << ep.endpointId << std::endl;
                std::wcout << L"  Name: " << ep.friendlyName << std::endl;
                std::wcout
                    << L"  State: "
                    << (ep.state ==
                                hydra::windows::AudioEndpointState::Active
                            ? L"Active"
                            : L"Inactive")
                    << std::endl;
                std::wcout << L"  -----------------------" << std::endl;
            }
        }
    } else if (command == "--inspect") {
        if (argc < 3) {
            std::cerr << "Missing PID." << std::endl;
            printUsage();
            exitCode = 1;
        } else {
            const auto pid =
                static_cast<std::uint32_t>(std::stoul(argv[2]));
            exitCode = runInspect(pid);
        }
    } else if (command == "--probe") {
        if (argc < 4) {
            std::cerr << "Missing PID or EndpointID." << std::endl;
            printUsage();
            exitCode = 1;
        } else {
            const auto pid =
                static_cast<std::uint32_t>(std::stoul(argv[2]));
            const std::string endpointId(argv[3]);
            const std::wstring targetEndpoint(
                endpointId.begin(), endpointId.end());
            exitCode = runProbe(pid, targetEndpoint);
        }
    } else {
        std::cerr << "Unknown command: " << command << std::endl;
        printUsage();
        exitCode = 1;
    }

    RoUninitialize();
    return exitCode;
}
