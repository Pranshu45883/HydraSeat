#include "hydra/host_transport.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int printSnapshot() {
    hydra::hostipc::HostPipeClient client;
    std::string error;
    if (!client.connect(hydra::hostipc::ClientRole::ReadOnly, 5000, &error)) {
        std::cerr << "connect failed: " << error << '\n';
        return 1;
    }

    const auto snapshot = client.getSnapshot(5000, &error);
    if (!snapshot) {
        std::cerr << "snapshot failed: " << error << '\n';
        return 1;
    }

    std::cout << "authority_revision=" << snapshot->authorityRevision << '\n';
    for (const auto& seat : snapshot->seats) {
        std::cout
            << "seat=" << seat.seatId
            << " generation=" << seat.generation
            << " active=" << (seat.active ? 1 : 0)
            << " process_owned=" << (seat.processOwned ? 1 : 0)
            << " window_owned=" << (seat.windowOwned ? 1 : 0)
            << " controller_bound=" << (seat.controllerBound ? 1 : 0)
            << '\n';
    }
    return 0;
}

int runPing() {
    hydra::hostipc::HostPipeClient client;
    std::string error;
    if (!client.connect(hydra::hostipc::ClientRole::ReadOnly, 5000, &error)) {
        std::cerr << "connect failed: " << error << '\n';
        return 1;
    }
    constexpr std::uint64_t nonce = 0x4859445241ull;
    if (!client.ping(nonce, 5000, &error)) {
        std::cerr << "ping failed: " << error << '\n';
        return 1;
    }
    std::cout << "pong\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view command =
        argc >= 2 && argv[1] != nullptr ? argv[1] : "snapshot";
    if (command == "snapshot") return printSnapshot();
    if (command == "ping") return runPing();

    std::cerr << "usage: hydraseat_hostctl [snapshot|ping]\n";
    return 2;
}
