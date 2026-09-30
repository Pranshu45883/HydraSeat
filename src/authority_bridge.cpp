#include "hydra/authority_bridge.hpp"

namespace hydra::runtime {

AuthorityBridge::AuthorityBridge(std::shared_ptr<SessionController> controller) noexcept
    : controller_(std::move(controller)) {}

AuthorityBridge::~AuthorityBridge() {
    if (controller_) {
        std::lock_guard lock(mutex_);
        if (uiTokens_[0]) controller_->releaseSeatLease(*uiTokens_[0]);
        if (uiTokens_[1]) controller_->releaseSeatLease(*uiTokens_[1]);
    }
}

bool AuthorityBridge::requestUiLease(std::uint32_t seatId) noexcept {
    if (!controller_ || (seatId != 1 && seatId != 2)) return false;

    std::lock_guard lock(mutex_);
    const auto token = controller_->acquireSeatLease(seatId, LeaseClass::UiConfiguration);
    if (!token.valid()) return false;

    uiTokens_[seatId - 1] = token;
    return true;
}

bool AuthorityBridge::releaseUiLease(std::uint32_t seatId) noexcept {
    if (!controller_ || (seatId != 1 && seatId != 2)) return false;

    std::lock_guard lock(mutex_);
    auto& tokenOpt = uiTokens_[seatId - 1];
    if (!tokenOpt) return false;

    const bool result = controller_->releaseSeatLease(*tokenOpt);
    tokenOpt.reset();
    return result;
}

bool AuthorityBridge::isUiLeaseActive(std::uint32_t seatId) const noexcept {
    if (!controller_ || (seatId != 1 && seatId != 2)) return false;
    std::lock_guard lock(mutex_);
    return uiTokens_[seatId - 1].has_value();
}

std::uint64_t AuthorityBridge::currentGeneration(std::uint32_t seatId) const noexcept {
    if (!controller_ || (seatId != 1 && seatId != 2)) return 0;
    std::lock_guard lock(mutex_);
    const auto& tokenOpt = uiTokens_[seatId - 1];
    return tokenOpt ? tokenOpt->generation : 0;
}

std::optional<ActivationToken> AuthorityBridge::getUiTokenLocked(std::uint32_t seatId) const noexcept {
    if (seatId != 1 && seatId != 2) return std::nullopt;
    return uiTokens_[seatId - 1];
}

std::optional<ActivationToken> AuthorityBridge::acquireGameLease(std::uint32_t seatId) noexcept {
    if (!controller_ || (seatId != 1 && seatId != 2)) return std::nullopt;
    
    // We do NOT store GameLauncher's lease inside AuthorityBridge.
    // GameLauncher manages its own token.
    const auto token = controller_->acquireSeatLease(seatId, LeaseClass::GameProcess);
    if (!token.valid()) return std::nullopt;
    
    return token;
}

bool AuthorityBridge::releaseGameLease(const ActivationToken& token) noexcept {
    if (!controller_) return false;
    return controller_->releaseSeatLease(token);
}

bool AuthorityBridge::publishProcess(const ActivationToken& token, const ProcessIdentity& process) noexcept {
    if (!controller_) return false;
    return controller_->publishProcess(token, process);
}

std::optional<std::uint32_t> AuthorityBridge::findSeatForProcess(const ProcessIdentity& process) const noexcept {
    if (!controller_) return std::nullopt;
    for (std::uint32_t seatId = 1; seatId <= 2; ++seatId) {
        auto snapshot = controller_->snapshot(seatId);
        if (snapshot && snapshot->active() && snapshot->process && *snapshot->process == process) {
            return seatId;
        }
    }
    return std::nullopt;
}

AudioRouteStatus AuthorityBridge::routeAudio(
    std::uint32_t seatId,
    const ProcessIdentity& expectedProcess,
    const AudioEndpointIdentity& targetEndpoint,
    AudioRouter& router) noexcept 
{
    if (!controller_) return AudioRouteStatus::InvalidProcess;
    
    std::lock_guard lock(mutex_);
    auto token = getUiTokenLocked(seatId);
    if (!token) return AudioRouteStatus::InvalidProcess; // Missing authority

    // Validate that the ProcessIdentity matches what the controller holds for this Seat.
    auto snapshot = controller_->snapshot(seatId);
    if (!snapshot || !snapshot->process || *snapshot->process != expectedProcess) {
        return AudioRouteStatus::InvalidProcess;
    }

    if (!controller_->bindAudioEndpoint(*token, targetEndpoint)) {
        return AudioRouteStatus::InvalidProcess;
    }
    
    return controller_->applyAudioRoute(*token, router);
}

AudioRouteStatus AuthorityBridge::resetAudio(
    std::uint32_t seatId,
    const ProcessIdentity& expectedProcess,
    AudioRouter& router) noexcept 
{
    if (!controller_) return AudioRouteStatus::InvalidProcess;

    std::lock_guard lock(mutex_);
    auto token = getUiTokenLocked(seatId);
    if (!token) return AudioRouteStatus::InvalidProcess;

    auto snapshot = controller_->snapshot(seatId);
    if (!snapshot || !snapshot->process || *snapshot->process != expectedProcess) {
        return AudioRouteStatus::InvalidProcess;
    }

    return controller_->clearAudioRoute(*token, router);
}

bool AuthorityBridge::pairController(
    std::uint32_t seatId,
    const controller::SeatBinding& binding,
    const controller::InventorySnapshot& inventory) noexcept 
{
    if (!controller_ || binding.seatId != seatId) return false;

    std::lock_guard lock(mutex_);
    auto token = getUiTokenLocked(seatId);
    if (!token) return false;

    return controller_->bindController(*token, binding, inventory);
}

} // namespace hydra::runtime
