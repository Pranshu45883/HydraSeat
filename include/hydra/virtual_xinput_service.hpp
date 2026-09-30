#pragma once

#include "hydra/controller_virtual_xinput.hpp"
#include "hydra/virtual_xinput_protocol.hpp"

namespace hydra::controller {

class IVirtualControllerBackend {
public:
    virtual ~IVirtualControllerBackend() = default;

    virtual PollResult poll(const SeatBinding& binding,
                            const InventorySnapshot& inventory) noexcept = 0;
    virtual IoStatus vibrate(const SeatBinding& binding,
                             const InventorySnapshot& inventory,
                             std::uint16_t lowFrequencyMotor,
                             std::uint16_t highFrequencyMotor) noexcept = 0;
};

class NativeVirtualControllerBackend final : public IVirtualControllerBackend {
public:
    PollResult poll(const SeatBinding& binding,
                    const InventorySnapshot& inventory) noexcept override;
    IoStatus vibrate(const SeatBinding& binding,
                     const InventorySnapshot& inventory,
                     std::uint16_t lowFrequencyMotor,
                     std::uint16_t highFrequencyMotor) noexcept override;
};

class VirtualXInputService final {
public:
    VirtualXInputService(VirtualXInputMapping mapping,
                         InventorySnapshot inventory,
                         IVirtualControllerBackend& backend) noexcept;

    ipc::VirtualXInputResponse handle(
        const ipc::VirtualXInputRequest& request) noexcept;

private:
    ipc::ProtocolStatus validateCurrentBinding() const noexcept;
    static ipc::ProtocolStatus protocolStatus(IoStatus status) noexcept;

    VirtualXInputMapping mapping_;
    InventorySnapshot inventory_;
    IVirtualControllerBackend& backend_;
};

} // namespace hydra::controller
