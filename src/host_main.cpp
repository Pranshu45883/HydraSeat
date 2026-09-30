#include "hydra/host_transport.hpp"

#include <iostream>
#include <string>

int main() {
#if defined(_WIN32)
    hydra::runtime::RuntimeHost host;
    hydra::hostipc::HostPipeServer server(host);

    std::string error;
    if (!server.serve(&error)) {
        std::cerr << "hydra_host failed: " << error << '\n';
        return 1;
    }
    return 0;
#else
    std::cerr << "hydra_host is supported only on Windows.\n";
    return 2;
#endif
}
