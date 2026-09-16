#pragma once

#include "hydra/virtual_xinput_protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace hydra::controller {

class VirtualXInputService;

struct PipeServerResult {
    bool served{false};
    ipc::ProtocolStatus status{ipc::ProtocolStatus::InvalidRequest};
};

class NamedPipeVirtualXInputServer final {
public:
    NamedPipeVirtualXInputServer(std::wstring endpoint,
                                 VirtualXInputService& service) noexcept;

    PipeServerResult serveOne(std::uint32_t timeoutMs) noexcept;

private:
    std::wstring endpoint_;
    VirtualXInputService& service_;
};

std::optional<ipc::VirtualXInputResponse> sendVirtualXInputRequest(
    const std::wstring& endpoint,
    const ipc::VirtualXInputRequest& request,
    std::uint32_t timeoutMs) noexcept;

} // namespace hydra::controller
