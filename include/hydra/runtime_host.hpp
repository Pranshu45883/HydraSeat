#pragma once

#include "hydra/audio_router.hpp"
#include "hydra/host_protocol.hpp"
#include "hydra/runtime_authority.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace hydra::runtime {

// RuntimeHost is the process-local owner that will live inside hydra_host.exe.
// It deliberately does not expose SessionController by reference: trusted
// backend components must pass through these methods so authority revision and
// cross-Seat serialization remain coherent.
class RuntimeHost final {
public:
    RuntimeHost() noexcept = default;

    RuntimeHost(const RuntimeHost&) = delete;
    RuntimeHost& operator=(const RuntimeHost&) = delete;

    hostipc::HostSnapshot snapshot() const noexcept;
    std::optional<SeatRuntimeSnapshot> seatSnapshot(std::uint32_t seatId) const noexcept;
    controller::InventorySnapshot controllerInventorySnapshot() noexcept;
    std::optional<std::uint32_t> seatForProcess(
        const ProcessIdentity& process) const noexcept;

    ActivationToken acquireUiLease(std::uint32_t seatId) noexcept;
    bool releaseUiLease(const ActivationToken& token) noexcept;
    bool pairController(const ActivationToken& uiLease,
                        const std::string& persistentControllerId,
                        std::uint8_t runtimeXInputSlot) noexcept;
    AudioRouteStatus routeAudio(const ActivationToken& uiLease,
                                const ProcessIdentity& expectedProcess,
                                const AudioEndpointIdentity& endpoint,
                                AudioRouter& router) noexcept;
    AudioRouteStatus resetAudio(const ActivationToken& uiLease,
                                const ProcessIdentity& expectedProcess,
                                AudioRouter& router) noexcept;

    ActivationToken beginSeatActivation(std::uint32_t seatId) noexcept;
    bool publishProcess(const ActivationToken& token,
                        const ProcessIdentity& process) noexcept;
    bool bindTargetWindow(const ActivationToken& token,
                          const ProcessIdentity& owner,
                          std::uintptr_t hwnd) noexcept;
    bool bindController(const ActivationToken& token,
                        const controller::SeatBinding& binding,
                        const controller::InventorySnapshot& inventory) noexcept;
    controller::PollResult pollController(
        const ActivationToken& token,
        const controller::InventorySnapshot& inventory) noexcept;
    controller::IoStatus setControllerVibration(
        const ActivationToken& token,
        const controller::InventorySnapshot& inventory,
        std::uint16_t lowFrequencyMotor,
        std::uint16_t highFrequencyMotor) noexcept;
    std::optional<controller::VirtualXInputMapping> virtualXInputMapping(
        const ActivationToken& token) const noexcept;
    bool endSeatActivation(const ActivationToken& token) noexcept;

private:
    void noteMutationLocked(bool changed) noexcept;

    mutable std::mutex mutex_;
    SessionController controller_;
    controller::ControllerInventory controllerInventory_;
    std::uint64_t authorityRevision_{1};
};

} // namespace hydra::runtime
