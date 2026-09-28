#pragma once

#include "hydra/controller_io.hpp"
#include "hydra/process_identity.hpp"
#include "hydra/audio_router.hpp"

#include <cstdint>
#include <mutex>
#include <optional>

namespace hydra::runtime {

enum class LeaseClass : std::uint8_t {
    UiConfiguration = 1,
    GameProcess = 2,
};

// Every Seat activation receives a new generation. Async/stale work must present
// the exact token and lease class before it can publish process or window state.
struct ActivationToken {
    std::uint32_t seatId{0};
    std::uint64_t generation{0};
    LeaseClass leaseClass{LeaseClass::UiConfiguration};

    bool valid() const noexcept {
        return (seatId == 1 || seatId == 2) && generation != 0;
    }

    bool operator==(const ActivationToken&) const = default;
};

struct SeatRuntimeSnapshot {
    std::uint32_t seatId{0};
    std::uint64_t generation{0};
    bool uiLeaseActive{false};
    bool gameLeaseActive{false};
    std::optional<ProcessIdentity> process;
    std::uintptr_t targetHwnd{0};
    std::optional<controller::SeatBinding> controllerBinding;
    std::optional<AudioEndpointIdentity> audioEndpoint;

    bool active() const noexcept { return uiLeaseActive || gameLeaseActive; }
    bool operator==(const SeatRuntimeSnapshot&) const = default;
};

class SeatRuntime final {
public:
    explicit SeatRuntime(std::uint32_t seatId) noexcept;

    ActivationToken acquireLease(LeaseClass leaseClass) noexcept;
    bool publishProcess(const ActivationToken& token,
                        const ProcessIdentity& process) noexcept;
    bool bindTargetWindow(const ActivationToken& token,
                          const ProcessIdentity& owner,
                          std::uintptr_t hwnd) noexcept;
    bool bindController(const ActivationToken& token,
                        const controller::SeatBinding& binding) noexcept;
    bool bindAudioEndpoint(const ActivationToken& token,
                           const AudioEndpointIdentity& endpoint) noexcept;
    bool clearAudioEndpoint(const ActivationToken& token) noexcept;
    bool releaseLease(const ActivationToken& token) noexcept;
    SeatRuntimeSnapshot snapshot() const noexcept;

private:
    bool ownsTokenLocked(const ActivationToken& token) const noexcept;

    const std::uint32_t seatId_;
    mutable std::mutex mutex_;
    std::uint64_t generation_{0};
    bool uiLeaseActive_{false};
    bool gameLeaseActive_{false};
    std::optional<ProcessIdentity> process_;
    std::uintptr_t targetHwnd_{0};
    std::optional<controller::SeatBinding> controllerBinding_;
    std::optional<AudioEndpointIdentity> audioEndpoint_;
};

class SessionController final {
public:
    SessionController() noexcept = default;

    ActivationToken acquireSeatLease(std::uint32_t seatId, LeaseClass leaseClass) noexcept;
    bool publishProcess(const ActivationToken& token,
                        const ProcessIdentity& process) noexcept;
    bool bindTargetWindow(const ActivationToken& token,
                          const ProcessIdentity& owner,
                          std::uintptr_t hwnd) noexcept;
    bool bindController(const ActivationToken& token,
                        const controller::SeatBinding& binding,
                        const controller::InventorySnapshot& inventory) noexcept;
    bool bindAudioEndpoint(const ActivationToken& token,
                           const AudioEndpointIdentity& endpoint) noexcept;
    
    AudioRouteStatus applyAudioRoute(const ActivationToken& token, AudioRouter& router) noexcept;
    AudioRouteStatus clearAudioRoute(const ActivationToken& token, AudioRouter& router) noexcept;
    
    controller::PollResult pollController(
        const ActivationToken& token,
        const controller::InventorySnapshot& inventory) noexcept;
    controller::IoStatus setControllerVibration(
        const ActivationToken& token,
        const controller::InventorySnapshot& inventory,
        std::uint16_t lowFrequencyMotor,
        std::uint16_t highFrequencyMotor) noexcept;
    bool releaseSeatLease(const ActivationToken& token) noexcept;
    std::optional<SeatRuntimeSnapshot> snapshot(std::uint32_t seatId) const noexcept;

private:
    SeatRuntime* seat(std::uint32_t seatId) noexcept;
    const SeatRuntime* seat(std::uint32_t seatId) const noexcept;
    SeatRuntime* otherSeat(std::uint32_t seatId) noexcept;
    const SeatRuntime* otherSeat(std::uint32_t seatId) const noexcept;

    // Serializes cross-Seat ownership decisions. SeatRuntime keeps its own lock
    // for Seat-local state, while this lock makes process/window/controller claims
    // atomic across both v1 Seats.
    mutable std::mutex mutex_;
    SeatRuntime seat1_{1};
    SeatRuntime seat2_{2};
};

} // namespace hydra::runtime
