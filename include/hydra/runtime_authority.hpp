#pragma once

#include "hydra/controller_io.hpp"
#include "hydra/process_identity.hpp"

#include <cstdint>
#include <mutex>
#include <optional>

namespace hydra::runtime {

struct ActivationToken {
    std::uint32_t seatId{0};
    std::uint64_t generation{0};

    bool valid() const noexcept {
        return (seatId == 1 || seatId == 2) && generation != 0;
    }

    bool operator==(const ActivationToken&) const = default;
};

struct SeatRuntimeSnapshot {
    std::uint32_t seatId{0};
    std::uint64_t generation{0};
    bool active{false};
    std::optional<ProcessIdentity> process;
    std::uintptr_t targetHwnd{0};
    std::optional<controller::SeatBinding> controllerBinding;

    bool operator==(const SeatRuntimeSnapshot&) const = default;
};

class SeatRuntime final {
public:
    explicit SeatRuntime(std::uint32_t seatId) noexcept;

    ActivationToken beginActivation() noexcept;
    bool publishProcess(const ActivationToken& token,
                        const ProcessIdentity& process) noexcept;
    bool bindTargetWindow(const ActivationToken& token,
                          const ProcessIdentity& owner,
                          std::uintptr_t hwnd) noexcept;
    bool bindController(const ActivationToken& token,
                        const controller::SeatBinding& binding) noexcept;
    bool endActivation(const ActivationToken& token) noexcept;
    SeatRuntimeSnapshot snapshot() const noexcept;

private:
    bool ownsTokenLocked(const ActivationToken& token) const noexcept;

    const std::uint32_t seatId_;
    mutable std::mutex mutex_;
    std::uint64_t generation_{0};
    bool active_{false};
    std::optional<ProcessIdentity> process_;
    std::uintptr_t targetHwnd_{0};
    std::optional<controller::SeatBinding> controllerBinding_;
};

class SessionController final {
public:
    SessionController() noexcept = default;

    ActivationToken beginSeatActivation(std::uint32_t seatId) noexcept;
    bool publishProcess(const ActivationToken& token,
                        const ProcessIdentity& process) noexcept;
    bool bindTargetWindow(const ActivationToken& token,
                          const ProcessIdentity& owner,
                          std::uintptr_t hwnd) noexcept;
    bool bindController(const ActivationToken& token,
                        const controller::SeatBinding& binding) noexcept;
    controller::PollResult pollController(const ActivationToken& token) noexcept;
    controller::IoStatus setControllerVibration(
        const ActivationToken& token,
        std::uint16_t lowFrequencyMotor,
        std::uint16_t highFrequencyMotor) noexcept;
    bool endSeatActivation(const ActivationToken& token) noexcept;
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
