#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/host_transport.hpp"

#include <cassert>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

int main() {
#if defined(_WIN32)
    using namespace hydra::hostipc;
    using namespace hydra::runtime;

    // Production default: the undocumented Windows AudioPolicyConfig path is
    // unavailable until explicitly enabled for controlled physical validation.
    assert(SetEnvironmentVariableW(
        L"HYDRA_EXPERIMENTAL_AUDIO_POLICY", nullptr) != FALSE ||
        GetLastError() == ERROR_ENVVAR_NOT_FOUND);

    RuntimeHost host;
    const auto activation = host.beginSeatActivation(1);
    assert(activation.valid());
    const ProcessIdentity process{4321, 987654321};
    assert(host.publishProcess(activation, process));

    HostPipeServer server(host);
    bool serverResult = false;
    std::string serverError;
    std::thread serverThread([&] {
        serverResult = server.serveOne(5000, &serverError);
    });

    HostPipeClient client;
    std::string clientError;
    assert(client.connect(ClientRole::Control, 5000, &clientError));
    assert(client.connected());

    auto snapshot = client.getSnapshot(5000, &clientError);
    assert(snapshot.has_value());
    assert(snapshot->seats[0].active);
    assert(snapshot->seats[0].gameLeaseActive);
    assert(snapshot->seats[0].generation == activation.generation);
    assert(!snapshot->seats[1].active);

    snapshot = client.acquireUiLease(1, 5000, &clientError);
    assert(snapshot.has_value());
    assert(snapshot->seats[0].uiLeaseActive);
    assert(snapshot->seats[0].gameLeaseActive);

    clientError.clear();
    const auto audioStatus = client.routeAudio(
        process.pid, process.creationIdentity,
        "{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}",
        5000, &clientError);
    assert(!audioStatus.has_value());
    assert(!clientError.empty());

    snapshot = client.releaseUiLease(1, 5000, &clientError);
    assert(snapshot.has_value());
    assert(!snapshot->seats[0].uiLeaseActive);
    assert(snapshot->seats[0].gameLeaseActive);

    snapshot = client.acquireUiLease(2, 5000, &clientError);
    assert(snapshot.has_value());
    assert(snapshot->seats[1].active);
    assert(snapshot->seats[1].uiLeaseActive);
    assert(!snapshot->seats[1].gameLeaseActive);

    assert(client.ping(0x12345678u, 5000, &clientError));

    // Closing the pipe destroys the server-side connection session. Its
    // connection-scoped UI lease must be released automatically.
    client.close();
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
