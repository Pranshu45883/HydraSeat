#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/host_transport.hpp"

#include <cassert>
#include <string>
#include <thread>

int main() {
#if defined(_WIN32)
    using namespace hydra::hostipc;
    using namespace hydra::runtime;

    RuntimeHost host;
    const auto activation = host.beginSeatActivation(1);
    assert(activation.valid());

    HostPipeServer server(host);
    bool serverResult = false;
    std::string serverError;
    std::thread serverThread([&] {
        serverResult = server.serveOne(5000, &serverError);
    });

    HostPipeClient client;
    std::string clientError;
    assert(client.connect(ClientRole::ReadOnly, 5000, &clientError));
    assert(client.connected());

    const auto snapshot = client.getSnapshot(5000, &clientError);
    assert(snapshot.has_value());
    assert(snapshot->seats[0].active);
    assert(snapshot->seats[0].generation == activation.generation);
    assert(!snapshot->seats[1].active);

    assert(client.ping(0x12345678u, 5000, &clientError));
    client.close();
    serverThread.join();

    assert(serverResult);
    assert(serverError.empty());
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
