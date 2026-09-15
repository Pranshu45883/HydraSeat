#include "hydra/virtual_xinput_service.hpp"

#include <cassert>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct FakeBackend final : hydra::controller::IVirtualControllerBackend {
    std::map<std::string, hydra::controller::GamepadState> states;
    std::vector<std::tuple<std::string, std::uint16_t, std::uint16_t>> vibrations;
    std::size_t pollCalls{0};

    hydra::controller::PollResult poll(
        const hydra::controller::SeatBinding& binding,
        const hydra::controller::InventorySnapshot&) noexcept override {
        ++pollCalls;
        const auto it = states.find(binding.runtimeKey);
        if (it == states.end()) {
            return {hydra::controller::IoStatus::Disconnected, std::nullopt};
        }
        return {hydra::controller::IoStatus::Ok, it->second};
    }

    hydra::controller::IoStatus vibrate(
        const hydra::controller::SeatBinding& binding,
        const hydra::controller::InventorySnapshot&,
        std::uint16_t low,
        std::uint16_t high) noexcept override {
        vibrations.emplace_back(binding.runtimeKey, low, high);
        return hydra::controller::IoStatus::Ok;
    }
};

hydra::controller::InventorySnapshot inventoryFor(
    std::string runtimeKey,
    std::uint8_t slot,
    std::uint64_t sourceGeneration) {
    hydra::controller::InventorySnapshot inventory;
    inventory.authoritative = true;

    hydra::controller::SourceDescriptor source;
    source.runtimeKey = std::move(runtimeKey);
    source.api = hydra::controller::ApiSurface::XInput;
    source.identityQuality = hydra::controller::IdentityQuality::RuntimeOnly;
    source.runtimeXInputSlot = slot;
    source.connected = true;
    source.sourceGeneration = sourceGeneration;
    inventory.sources.push_back(std::move(source));
    return inventory;
}

hydra::controller::VirtualXInputMapping mappingFor(
    std::uint32_t seatId,
    std::uint64_t activationGeneration,
    std::string runtimeKey,
    std::uint8_t slot,
    std::uint64_t sourceGeneration) {
    hydra::controller::SeatBinding binding;
    binding.seatId = seatId;
    binding.api = hydra::controller::ApiSurface::XInput;
    binding.runtimeKey = std::move(runtimeKey);
    binding.runtimeXInputSlot = slot;
    binding.sourceGeneration = sourceGeneration;
    return {seatId, activationGeneration, std::move(binding)};
}

hydra::controller::ipc::VirtualXInputRequest getStateRequest(
    std::uint32_t seatId,
    std::uint64_t activationGeneration,
    std::uint64_t sourceGeneration,
    std::uint8_t logicalSlot = 0) {
    hydra::controller::ipc::VirtualXInputRequest request;
    request.opcode = hydra::controller::ipc::ProtocolOpcode::GetState;
    request.seatId = seatId;
    request.activationGeneration = activationGeneration;
    request.sourceGeneration = sourceGeneration;
    request.logicalSlot = logicalSlot;
    return request;
}

} // namespace

void testVirtualXInputService() {
    using namespace hydra::controller;
    using namespace hydra::controller::ipc;

    FakeBackend backend;
    GamepadState stateA;
    stateA.buttons = 0x0001;
    stateA.thumbLX = 111;
    GamepadState stateB;
    stateB.buttons = 0x0002;
    stateB.thumbLX = 222;
    backend.states.emplace("source-a", stateA);
    backend.states.emplace("source-b", stateB);

    const auto inventoryA = inventoryFor("source-a", 0, 4);
    const auto inventoryB = inventoryFor("source-b", 1, 9);
    VirtualXInputService serviceA(mappingFor(1, 10, "source-a", 0, 4), inventoryA, backend);
    VirtualXInputService serviceB(mappingFor(2, 20, "source-b", 1, 9), inventoryB, backend);

    const auto responseA = serviceA.handle(getStateRequest(1, 10, 4));
    assert(responseA.status == ProtocolStatus::Ok);
    assert(responseA.hasState);
    assert(responseA.state.buttons == 0x0001);
    assert(responseA.state.thumbLX == 111);

    const auto responseB = serviceB.handle(getStateRequest(2, 20, 9));
    assert(responseB.status == ProtocolStatus::Ok);
    assert(responseB.hasState);
    assert(responseB.state.buttons == 0x0002);
    assert(responseB.state.thumbLX == 222);

    const auto callsBeforeDisconnectedSlot = backend.pollCalls;
    const auto disconnected = serviceA.handle(getStateRequest(1, 10, 4, 1));
    assert(disconnected.status == ProtocolStatus::Disconnected);
    assert(!disconnected.hasState);
    assert(backend.pollCalls == callsBeforeDisconnectedSlot);

    const auto callsBeforeWrongSeat = backend.pollCalls;
    const auto wrongSeat = serviceA.handle(getStateRequest(2, 10, 4));
    assert(wrongSeat.status == ProtocolStatus::InvalidMapping);
    assert(backend.pollCalls == callsBeforeWrongSeat);

    const auto wrongActivation = serviceA.handle(getStateRequest(1, 11, 4));
    assert(wrongActivation.status == ProtocolStatus::InvalidMapping);

    const auto staleSource = serviceA.handle(getStateRequest(1, 10, 5));
    assert(staleSource.status == ProtocolStatus::StaleBinding);

    auto vibration = getStateRequest(2, 20, 9);
    vibration.opcode = ProtocolOpcode::SetVibration;
    vibration.lowFrequencyMotor = 100;
    vibration.highFrequencyMotor = 200;
    const auto vibrationResponse = serviceB.handle(vibration);
    assert(vibrationResponse.status == ProtocolStatus::Ok);
    assert(!vibrationResponse.hasState);
    assert(backend.vibrations.size() == 1);
    const auto expectedVibration =
        std::tuple<std::string, std::uint16_t, std::uint16_t>{"source-b", 100, 200};
    assert(backend.vibrations[0] == expectedVibration);

    auto staleInventoryA = inventoryA;
    staleInventoryA.sources[0].sourceGeneration = 5;
    VirtualXInputService staleServiceA(
        mappingFor(1, 10, "source-a", 0, 4), staleInventoryA, backend);
    const auto staleAgainstInventory = staleServiceA.handle(getStateRequest(1, 10, 4));
    assert(staleAgainstInventory.status == ProtocolStatus::StaleBinding);

    VirtualXInputRequest ping;
    ping.opcode = ProtocolOpcode::Ping;
    const auto pingResponse = serviceA.handle(ping);
    assert(pingResponse.status == ProtocolStatus::Ok);
}
