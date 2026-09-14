#include "hydra/runtime_authority.hpp"

#include <limits>

namespace hydra::runtime {

SeatRuntime::SeatRuntime(std::uint32_t seatId) noexcept : seatId_(seatId) {}

ActivationToken SeatRuntime::beginActivation() noexcept {
    std::lock_guard lock(mutex_);
    if ((seatId_ != 1 && seatId_ != 2) ||
        active_ ||
        generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return {};
    }

    ++generation_;
    active_ = true;
    process_.reset();
    targetHwnd_ = 0;
    return {seatId_, generation_};
}

bool SeatRuntime::publishProcess(const ActivationToken& token,
                                 const ProcessIdentity& process) noexcept {
    if (!process.valid()) return false;

    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token)) return false;
    if (process_ && *process_ != process) return false;

    process_ = process;
    return true;
}

bool SeatRuntime::bindTargetWindow(const ActivationToken& token,
                                   const ProcessIdentity& owner,
                                   std::uintptr_t hwnd) noexcept {
    if (!owner.valid() || hwnd == 0) return false;

    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token) || !process_ || *process_ != owner) return false;

    targetHwnd_ = hwnd;
    return true;
}

bool SeatRuntime::endActivation(const ActivationToken& token) noexcept {
    std::lock_guard lock(mutex_);
    if (!ownsTokenLocked(token)) return false;

    active_ = false;
    process_.reset();
    targetHwnd_ = 0;
    return true;
}

SeatRuntimeSnapshot SeatRuntime::snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    return {seatId_, generation_, active_, process_, targetHwnd_};
}

bool SeatRuntime::ownsTokenLocked(const ActivationToken& token) const noexcept {
    return active_ && token.valid() && token.seatId == seatId_ &&
           token.generation == generation_;
}

ActivationToken SessionController::beginSeatActivation(std::uint32_t seatId) noexcept {
    std::lock_guard lock(mutex_);
    const auto runtime = seat(seatId);
    return runtime ? runtime->beginActivation() : ActivationToken{};
}

bool SessionController::publishProcess(const ActivationToken& token,
                                       const ProcessIdentity& process) noexcept {
    if (!process.valid()) return false;

    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    const auto other = otherSeat(token.seatId);
    if (!runtime || !other) return false;

    const auto otherSnapshot = other->snapshot();
    if (otherSnapshot.active && otherSnapshot.process == process) return false;

    return runtime->publishProcess(token, process);
}

bool SessionController::bindTargetWindow(const ActivationToken& token,
                                         const ProcessIdentity& owner,
                                         std::uintptr_t hwnd) noexcept {
    if (!owner.valid() || hwnd == 0) return false;

    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    const auto other = otherSeat(token.seatId);
    if (!runtime || !other) return false;

    const auto otherSnapshot = other->snapshot();
    if (otherSnapshot.active && otherSnapshot.targetHwnd == hwnd) return false;

    return runtime->bindTargetWindow(token, owner, hwnd);
}

bool SessionController::endSeatActivation(const ActivationToken& token) noexcept {
    std::lock_guard lock(mutex_);
    const auto runtime = seat(token.seatId);
    return runtime && runtime->endActivation(token);
}

std::optional<SeatRuntimeSnapshot> SessionController::snapshot(
    std::uint32_t seatId) const noexcept {
    const auto runtime = seat(seatId);
    if (!runtime) return std::nullopt;
    return runtime->snapshot();
}

SeatRuntime* SessionController::seat(std::uint32_t seatId) noexcept {
    if (seatId == 1) return &seat1_;
    if (seatId == 2) return &seat2_;
    return nullptr;
}

const SeatRuntime* SessionController::seat(std::uint32_t seatId) const noexcept {
    if (seatId == 1) return &seat1_;
    if (seatId == 2) return &seat2_;
    return nullptr;
}

SeatRuntime* SessionController::otherSeat(std::uint32_t seatId) noexcept {
    if (seatId == 1) return &seat2_;
    if (seatId == 2) return &seat1_;
    return nullptr;
}

const SeatRuntime* SessionController::otherSeat(std::uint32_t seatId) const noexcept {
    if (seatId == 1) return &seat2_;
    if (seatId == 2) return &seat1_;
    return nullptr;
}

} // namespace hydra::runtime
