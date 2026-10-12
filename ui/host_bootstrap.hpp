#pragma once

#include "hydra/host_transport.hpp"

#include <string>

namespace hydra::ui {

// Connect to the canonical per-session host. If it is not running yet, start
// the signed/sibling hydra_host.exe beside the current UI executable and retry.
// This is intentionally shared by the read-only poller and the control client
// so GUI startup cannot depend on a manual host launch.
bool connectCanonicalHost(
    hydra::hostipc::HostPipeClient& client,
    hydra::hostipc::ClientRole role,
    std::string* error = nullptr);

} // namespace hydra::ui
