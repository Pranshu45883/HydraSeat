#pragma once

#include "hydra/audio_router.hpp"
#include "hydra/host_protocol.hpp"
#include "hydra/hardware_detector.hpp"
#include "hydra/runtime_authority.hpp"
#include "hydra/seat_hardware_configuration.hpp"
#include "hydra/seat_hardware_store.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace hydra::runtime {

// RuntimeHost is the process-local owner that will live inside hydra_host.exe.
// It deliberately does not expose SessionController by reference: trusted
// backend components must pass through these methods so authority revision and
// cross-Seat serialization remain coherent.
class RuntimeHost final {
public:
    RuntimeHost() = default;
    explicit RuntimeHost(std::filesystem::path seatConfigPath);

    bool loadPersistentSeatHardware(std::string* error = nullptr);

    RuntimeHost(const RuntimeHost&) = delete;
    RuntimeHost& operator=(const RuntimeHost&) = delete;

    hostipc::HostSnapshot snapshot() const noexcept;
    std::optional<SeatRuntimeSnapshot> seatSnapshot(std::uint32_t seatId) const noexcept;
    controller::InventorySnapshot controllerInventorySnapshot() noexcept;
    std::vector<DeviceInfo> hardwareInventory();
    void observePhysicalInput(
        DeviceType type,
        const std::wstring& devicePath);
    std::optional<SeatHardwareConfiguration> seatHardwareConfiguration(
        std::uint32_t seatId) const;
    bool configureSeatHardware(
        const ActivationToken& uiLease,
        const SeatHardwareConfiguration& configuration,
        std::string* error = nullptr);
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
    bool clearTargetWindow(const ActivationToken& token,
                           const ProcessIdentity& owner,
                           std::uintptr_t expectedHwnd) noexcept;
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
    mutable std::mutex controllerInventoryMutex_;
    mutable std::mutex audioMutationMutex_;
    SessionController controller_;
    controller::ControllerInventory controllerInventory_;
    HardwareDetector hardwareDetector_;
    std::unordered_set<std::wstring> confirmedKeyboardIds_;
    std::unordered_set<std::wstring> confirmedMouseIds_;
    SeatHardwareConfigurations hardwareConfigurations_{{
        SeatHardwareConfiguration{1},
        SeatHardwareConfiguration{2},
    }};
    std::optional<SeatHardwareStore> seatHardwareStore_;
    std::uint64_t authorityRevision_{1};
};

} // namespace hydra::runtime
