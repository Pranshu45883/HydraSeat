#include "hydra/runtime_authority.hpp"

#include <cassert>

void testControllerVirtualXInput() {
    using namespace hydra;

    controller::InventorySnapshot inventory;
    inventory.authoritative = true;
    inventory.sources.push_back({
        "xinput-slot:0", std::nullopt, L"Runtime XInput Slot 0",
        controller::ApiSurface::XInput,
        controller::IdentityQuality::RuntimeOnly,
        std::uint8_t{0}, true, std::uint64_t{1}});

    runtime::SessionController session;
    const auto token = session.beginSeatActivation(1);
    assert(token.valid());

    const controller::SeatBinding binding{
        1, controller::ApiSurface::XInput, "xinput-slot:0",
        std::nullopt, std::uint8_t{0}, std::uint64_t{1}};
    assert(session.bindController(token, binding, inventory));

    const auto mapping = session.virtualXInputMapping(token);
    assert(mapping.has_value());
    assert(mapping->valid());
    assert(mapping->seatId == 1);
    assert(mapping->activationGeneration == token.generation);

    auto wrongApi = *mapping;
    wrongApi.source.api = controller::ApiSurface::GameInput;
    assert(!wrongApi.valid());
    assert(controller::pollVirtualXInput(
               wrongApi, controller::kSeatLogicalXInputSlot, inventory).status ==
           controller::IoStatus::InvalidBinding);

    auto missingRuntimeSlot = *mapping;
    missingRuntimeSlot.source.runtimeXInputSlot.reset();
    assert(!missingRuntimeSlot.valid());

    auto zeroSourceGeneration = *mapping;
    zeroSourceGeneration.source.sourceGeneration = 0;
    assert(!zeroSourceGeneration.valid());

    assert(controller::pollVirtualXInput(
               *mapping, std::uint8_t{1}, inventory).status ==
           controller::IoStatus::Disconnected);
    assert(controller::setVirtualXInputVibration(
               *mapping, std::uint8_t{1}, inventory,
               std::uint16_t{0}, std::uint16_t{0}) ==
           controller::IoStatus::Disconnected);

    auto staleInventory = inventory;
    ++staleInventory.sources[0].sourceGeneration;
    assert(controller::pollVirtualXInput(
               *mapping, controller::kSeatLogicalXInputSlot,
               staleInventory).status == controller::IoStatus::StaleBinding);

    assert(session.endSeatActivation(token));
    assert(!session.virtualXInputMapping(token).has_value());

    controller::InventorySnapshot gameInputInventory;
    gameInputInventory.authoritative = true;
    gameInputInventory.sources.push_back({
        "gameinput:pad-a", std::wstring{L"container-a"}, L"Stable Pad A",
        controller::ApiSurface::GameInput,
        controller::IdentityQuality::Stable,
        std::nullopt, true, std::uint64_t{1}});
    gameInputInventory.physicalControllers.push_back(
        {L"container-a", L"Stable Pad A", L"hid-path-a"});

    const auto gameInputActivation = session.beginSeatActivation(1);
    assert(gameInputActivation.valid());
    const controller::SeatBinding gameInputBinding{
        1, controller::ApiSurface::GameInput, "gameinput:pad-a",
        std::wstring{L"container-a"}, std::nullopt, std::uint64_t{1}};
    assert(session.bindController(
        gameInputActivation, gameInputBinding, gameInputInventory));
    assert(!session.virtualXInputMapping(gameInputActivation).has_value());
    assert(session.endSeatActivation(gameInputActivation));
}
