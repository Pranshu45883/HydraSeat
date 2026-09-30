#include "hydra/virtual_xinput_service.hpp"

namespace hydra::controller {

PollResult NativeVirtualControllerBackend::poll(
    const SeatBinding& binding,
    const InventorySnapshot& inventory) noexcept {
    return pollBoundController(binding, inventory);
}

IoStatus NativeVirtualControllerBackend::vibrate(
    const SeatBinding& binding,
    const InventorySnapshot& inventory,
    std::uint16_t lowFrequencyMotor,
    std::uint16_t highFrequencyMotor) noexcept {
    return setBoundControllerVibration(
        binding, inventory, lowFrequencyMotor, highFrequencyMotor);
}

VirtualXInputService::VirtualXInputService(
    VirtualXInputMapping mapping,
    InventorySnapshot inventory,
    IVirtualControllerBackend& backend) noexcept
    : mapping_(std::move(mapping)),
      inventory_(std::move(inventory)),
      backend_(backend) {}

ipc::ProtocolStatus VirtualXInputService::protocolStatus(IoStatus status) noexcept {
    switch (status) {
        case IoStatus::Ok:
            return ipc::ProtocolStatus::Ok;
        case IoStatus::Disconnected:
            return ipc::ProtocolStatus::Disconnected;
        case IoStatus::StaleBinding:
            return ipc::ProtocolStatus::StaleBinding;
        case IoStatus::InvalidBinding:
        case IoStatus::UnsupportedApi:
            return ipc::ProtocolStatus::InvalidMapping;
        case IoStatus::PlatformUnavailable:
        case IoStatus::NativeFailure:
            return ipc::ProtocolStatus::BackendFailure;
    }
    return ipc::ProtocolStatus::BackendFailure;
}

ipc::ProtocolStatus VirtualXInputService::validateCurrentBinding() const noexcept {
    if (!mapping_.valid() || !inventory_.authoritative) {
        return ipc::ProtocolStatus::InvalidMapping;
    }

    const SourceDescriptor* matched = nullptr;
    std::size_t matches = 0;
    for (const auto& source : inventory_.sources) {
        if (source.api == mapping_.source.api &&
            source.runtimeKey == mapping_.source.runtimeKey) {
            ++matches;
            matched = &source;
        }
    }

    if (matches != 1 || matched == nullptr) {
        return ipc::ProtocolStatus::InvalidMapping;
    }
    if (!matched->connected) {
        return ipc::ProtocolStatus::Disconnected;
    }
    if (matched->sourceGeneration != mapping_.source.sourceGeneration) {
        return ipc::ProtocolStatus::StaleBinding;
    }
    if (matched->runtimeXInputSlot != mapping_.source.runtimeXInputSlot) {
        return ipc::ProtocolStatus::InvalidMapping;
    }
    if (!bindingMatchesInventory(mapping_.source, inventory_)) {
        return ipc::ProtocolStatus::InvalidMapping;
    }
    return ipc::ProtocolStatus::Ok;
}

ipc::VirtualXInputResponse VirtualXInputService::handle(
    const ipc::VirtualXInputRequest& request) noexcept {
    if (request.opcode == ipc::ProtocolOpcode::Ping) {
        return {ipc::ProtocolStatus::Ok, {}, false};
    }

    if (!mapping_.valid() || request.seatId != mapping_.seatId ||
        request.activationGeneration != mapping_.activationGeneration) {
        return {ipc::ProtocolStatus::InvalidMapping, {}, false};
    }
    if (request.sourceGeneration != mapping_.source.sourceGeneration) {
        return {ipc::ProtocolStatus::StaleBinding, {}, false};
    }
    if (request.logicalSlot != kSeatLogicalXInputSlot) {
        return {ipc::ProtocolStatus::Disconnected, {}, false};
    }

    const auto validation = validateCurrentBinding();
    if (validation != ipc::ProtocolStatus::Ok) {
        return {validation, {}, false};
    }

    if (request.opcode == ipc::ProtocolOpcode::GetState) {
        const auto result = backend_.poll(mapping_.source, inventory_);
        const auto status = protocolStatus(result.status);
        if (status != ipc::ProtocolStatus::Ok || !result.state) {
            return {status == ipc::ProtocolStatus::Ok
                        ? ipc::ProtocolStatus::BackendFailure
                        : status,
                    {}, false};
        }
        return {ipc::ProtocolStatus::Ok, *result.state, true};
    }

    if (request.opcode == ipc::ProtocolOpcode::SetVibration) {
        const auto status = backend_.vibrate(
            mapping_.source, inventory_,
            request.lowFrequencyMotor, request.highFrequencyMotor);
        return {protocolStatus(status), {}, false};
    }

    return {ipc::ProtocolStatus::InvalidRequest, {}, false};
}

} // namespace hydra::controller
