#pragma once

#include "hydra/runtime_authority.hpp"
#include <memory>
#include <mutex>
#include <optional>

namespace hydra::runtime {

class AuthorityBridge final {
public:
    explicit AuthorityBridge(std::shared_ptr<SessionController> controller) noexcept;
    ~AuthorityBridge();

    // UI Configuration Lease API
    bool requestUiLease(std::uint32_t seatId) noexcept;
    bool releaseUiLease(std::uint32_t seatId) noexcept;
    bool isUiLeaseActive(std::uint32_t seatId) const noexcept;
    std::uint64_t currentGeneration(std::uint32_t seatId) const noexcept;

    // GameLauncher Lease API
    std::optional<ActivationToken> acquireGameLease(std::uint32_t seatId) noexcept;
    bool releaseGameLease(const ActivationToken& token) noexcept;

    // Mutators
    bool publishProcess(const ActivationToken& token, const ProcessIdentity& process) noexcept;
    std::optional<std::uint32_t> findSeatForProcess(const ProcessIdentity& process) const noexcept;
    
    // Audio Routing (V5 Contract)
    AudioRouteStatus routeAudio(
        std::uint32_t seatId,
        const ProcessIdentity& expectedProcess,
        const AudioEndpointIdentity& targetEndpoint,
        AudioRouter& router) noexcept;
        
    AudioRouteStatus resetAudio(
        std::uint32_t seatId,
        const ProcessIdentity& expectedProcess,
        AudioRouter& router) noexcept;

    // Controller Pairing
    bool pairController(
        std::uint32_t seatId,
        const controller::SeatBinding& binding,
        const controller::InventorySnapshot& inventory) noexcept;

private:
    std::shared_ptr<SessionController> controller_;
    mutable std::mutex mutex_;
    std::optional<ActivationToken> uiTokens_[2];

    std::optional<ActivationToken> getUiTokenLocked(std::uint32_t seatId) const noexcept;
};

} // namespace hydra::runtime
