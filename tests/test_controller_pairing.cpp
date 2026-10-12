#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/controller_pairing.hpp"

#include <cassert>

void testControllerPairing() {
    using namespace hydra::controller;

    InventorySnapshot inventory;
    inventory.authoritative = true;
    inventory.sources.push_back({
        "xinput-slot:0", std::nullopt, L"Runtime XInput Slot 0",
        ApiSurface::XInput, IdentityQuality::RuntimeOnly,
        std::uint8_t{0}, true, std::uint64_t{1}});
    inventory.physicalControllers.push_back(
        {L"container-a", L"Physical Pad A", L"hid-path-a"});

    XInputPairingSnapshot before;
    XInputPairingSnapshot after;
    before.authoritative = true;
    after.authoritative = true;
    before.slotStates[0].connected = true;
    after.slotStates[0].connected = true;
    after.slotStates[0].state.buttons = std::uint16_t{1};

    const auto unique = detectUniqueXInputButtonPress(before, after);
    assert(unique.status == PairingProbeStatus::UniqueButtonPress);
    assert(unique.runtimeSlot == std::optional<std::uint8_t>{0});

    const auto paired = pairPhysicalControllerFromButtonPress(
        1, L"container-a", before, after, inventory);
    assert(paired.status == PairingStatus::Ok);
    assert(paired.binding.has_value());
    assert(paired.binding->persistentControllerId ==
           std::optional<std::wstring>{L"container-a"});
    assert(paired.binding->runtimeXInputSlot ==
           std::optional<std::uint8_t>{0});

    auto nonAuthoritative = inventory;
    nonAuthoritative.authoritative = false;
    assert(pairPhysicalControllerFromButtonPress(
               1, L"container-a", before, after, nonAuthoritative).status ==
           PairingStatus::InventoryNotAuthoritative);

    auto analogOnly = after;
    analogOnly.slotStates[0].state.buttons = 0;
    analogOnly.slotStates[0].state.thumbLX = 100;
    assert(detectUniqueXInputButtonPress(before, analogOnly).status ==
           PairingProbeStatus::NoButtonPress);

    before.slotStates[1].connected = true;
    auto ambiguous = after;
    ambiguous.slotStates[1].connected = true;
    ambiguous.slotStates[1].state.buttons = std::uint16_t{2};
    assert(detectUniqueXInputButtonPress(before, ambiguous).status ==
           PairingProbeStatus::AmbiguousButtonPress);
    assert(pairPhysicalControllerFromButtonPress(
               1, L"container-a", before, ambiguous, inventory).status ==
           PairingStatus::PairingGestureAmbiguous);
}
