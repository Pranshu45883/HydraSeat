#pragma once

#include "hydra/audio_router.hpp"

namespace hydra::windows {

class WindowsAudioRouter final : public hydra::runtime::AudioRouter {
public:
    WindowsAudioRouter() = default;
    ~WindowsAudioRouter() override = default;

    hydra::runtime::AudioRouteStatus assignEndpoint(
        const hydra::runtime::ProcessIdentity& process,
        const hydra::runtime::AudioEndpointIdentity& endpoint) noexcept override;

    hydra::runtime::AudioRouteStatus clearAssignment(
        const hydra::runtime::ProcessIdentity& process) noexcept override;
};

} // namespace hydra::windows
