#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/host_transport.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    using namespace hydra::hostipc;
    using namespace hydra::runtime;

    assert(argc >= 2);
    assert(argv[1] != nullptr);
    const std::string controlledChildPath = argv[1];

    RuntimeHost host;
    const auto activation = host.beginSeatActivation(1);
    assert(activation.valid());
    const ProcessIdentity process{4321, 987654321};
    assert(host.publishProcess(activation, process));

    HostPipeServer server(host);
    bool serverResult = false;
    std::string serverError;
    std::thread serverThread([&] {
        serverResult = server.serve(&serverError);
    });

    HostPipeClient client;
    std::string clientError;
    assert(client.connect(ClientRole::Control, 5000, &clientError));
    assert(client.connected());

    HostPipeClient observer;
    std::string observerError;
    assert(observer.connect(ClientRole::ReadOnly, 5000, &observerError));
    assert(observer.connected());

    auto snapshot = client.getSnapshot(5000, &clientError);
    if (!snapshot) {
        std::cerr << "control snapshot failed: " << clientError << "\n";
    }
    assert(snapshot.has_value());
    assert(snapshot->seats[0].active);
    assert(snapshot->seats[0].gameLeaseActive);
    assert(snapshot->seats[0].generation == activation.generation);
    assert(!snapshot->seats[1].active);

    // Regression: the production server used to pass its 250 ms accept timeout
    // into the connected client read loop. That silently disconnected an idle
    // UI client between the 2-second poll intervals and made the UI alternate
    // between "connected" and "unavailable". Two persistent clients must remain
    // usable well beyond that listener timeout.
    Sleep(750);
    const auto observed = observer.getSnapshot(5000, &observerError);
    assert(observed.has_value());
    assert(observer.ping(0xA11CEu, 5000, &observerError));
    assert(client.ping(0xC0117u, 5000, &clientError));

    snapshot = client.acquireUiLease(1, 5000, &clientError);
    assert(snapshot.has_value());
    assert(snapshot->seats[0].uiLeaseActive);
    assert(snapshot->seats[0].gameLeaseActive);

    clientError.clear();
    const auto audioStatus = client.routeAudio(
        process.pid, process.creationIdentity,
        "{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}",
        5000, &clientError);
    assert(audioStatus.has_value());
    assert(*audioStatus != AudioMutationStatus::Success);
    assert(clientError.empty());

    snapshot = client.releaseUiLease(1, 5000, &clientError);
    assert(snapshot.has_value());
    assert(!snapshot->seats[0].uiLeaseActive);
    assert(snapshot->seats[0].gameLeaseActive);

    snapshot = client.acquireUiLease(2, 5000, &clientError);
    assert(snapshot.has_value());
    assert(snapshot->seats[1].active);
    assert(snapshot->seats[1].uiLeaseActive);
    assert(!snapshot->seats[1].gameLeaseActive);

    const std::string readyEventName =
        "Local\\HydraSeatHostLaunchPipeTest_" +
        std::to_string(GetCurrentProcessId());
    HANDLE readyEvent = CreateEventA(
        nullptr, TRUE, FALSE, readyEventName.c_str());
    assert(readyEvent != nullptr);

    LaunchGameRequest launchRequest;
    launchRequest.seatId = 2;
    launchRequest.titleUtf8 = "Host launch E2E";
    launchRequest.executablePathUtf8 = controlledChildPath;
    launchRequest.launchArgumentsUtf8 =
        "--ready-event " + readyEventName + " --lifetime-ms 30000";

    snapshot = client.launchGame(launchRequest, 5000, &clientError);
    if (!snapshot) {
        std::cerr << "launch failed: " << clientError << "\n";
    }
    assert(snapshot.has_value());
    assert(snapshot->seats[1].uiLeaseActive);
    assert(snapshot->seats[1].gameLeaseActive);
    assert(snapshot->seats[1].processOwned);
    assert(snapshot->seats[1].processId != 0);
    assert(snapshot->seats[1].processCreationIdentity != 0);
    assert(WaitForSingleObject(readyEvent, 5000) == WAIT_OBJECT_0);

    snapshot = client.stopGame(2, 10000, &clientError);
    assert(snapshot.has_value());
    assert(snapshot->seats[1].uiLeaseActive);
    assert(!snapshot->seats[1].gameLeaseActive);
    assert(!snapshot->seats[1].processOwned);
    CloseHandle(readyEvent);

    assert(client.ping(0x12345678u, 5000, &clientError));

    // Closing the pipe destroys the server-side connection session. Its
    // connection-scoped UI lease must be released automatically.
    client.close();
    observer.close();
    server.requestStop();
    serverThread.join();

    assert(serverResult);
    assert(serverError.empty());
    const auto afterDisconnect = host.snapshot();
    assert(afterDisconnect.seats[0].active);
    assert(afterDisconnect.seats[0].gameLeaseActive);
    assert(!afterDisconnect.seats[1].active);
    assert(!afterDisconnect.seats[1].uiLeaseActive);

    assert(host.endSeatActivation(activation));
#else
    hydra::runtime::RuntimeHost host;
    hydra::hostipc::HostPipeClient client;
    std::string error;
    assert(!client.connect(
        hydra::hostipc::ClientRole::ReadOnly, 1, &error));
    assert(!error.empty());
#endif
    return 0;
}
