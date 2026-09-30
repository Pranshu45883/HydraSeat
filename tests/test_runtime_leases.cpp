#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/runtime_authority.hpp"

#include <cassert>
#include <cstdint>

int main() {
    using namespace hydra::runtime;

    SessionController controller;

    const auto ui = controller.acquireSeatLease(1, LeaseClass::UiConfiguration);
    assert(ui.valid());
    assert(ui.leaseClass == LeaseClass::UiConfiguration);

    auto snapshot = controller.snapshot(1);
    assert(snapshot);
    assert(snapshot->active);
    assert(snapshot->uiLeaseActive);
    assert(!snapshot->gameLeaseActive);
    assert(snapshot->generation == ui.generation);

    // Duplicate authority of the same class is rejected.
    assert(!controller.acquireSeatLease(1, LeaseClass::UiConfiguration).valid());

    // UI configuration authority cannot claim a game process/window.
    const ProcessIdentity process{4242u, 1001u};
    assert(!controller.publishProcess(ui, process));
    assert(!controller.bindTargetWindow(ui, process, 0x100u));

    const auto game = controller.acquireSeatLease(1, LeaseClass::GameProcess);
    assert(game.valid());
    assert(game.generation == ui.generation);
    assert(game.leaseClass == LeaseClass::GameProcess);
    assert(!controller.acquireSeatLease(1, LeaseClass::GameProcess).valid());

    assert(controller.publishProcess(game, process));
    assert(controller.bindTargetWindow(game, process, 0x100u));

    snapshot = controller.snapshot(1);
    assert(snapshot);
    assert(snapshot->uiLeaseActive && snapshot->gameLeaseActive);
    assert(snapshot->process == process);
    assert(snapshot->targetHwnd == 0x100u);

    // Releasing the game lease must clear game-owned runtime state even while the
    // UI lease remains alive.
    assert(controller.releaseSeatLease(game));
    snapshot = controller.snapshot(1);
    assert(snapshot);
    assert(snapshot->active);
    assert(snapshot->uiLeaseActive);
    assert(!snapshot->gameLeaseActive);
    assert(!snapshot->process);
    assert(snapshot->targetHwnd == 0u);

    // A new game lease inside the same still-live UI epoch keeps the generation.
    const auto gameAgain =
        controller.acquireSeatLease(1, LeaseClass::GameProcess);
    assert(gameAgain.valid());
    assert(gameAgain.generation == ui.generation);

    // Releasing the UI lease must not invalidate the active game lease.
    assert(controller.releaseSeatLease(ui));
    snapshot = controller.snapshot(1);
    assert(snapshot);
    assert(snapshot->active);
    assert(!snapshot->uiLeaseActive);
    assert(snapshot->gameLeaseActive);

    assert(controller.publishProcess(gameAgain, process));
    assert(controller.releaseSeatLease(gameAgain));

    snapshot = controller.snapshot(1);
    assert(snapshot);
    assert(!snapshot->active);
    assert(!snapshot->uiLeaseActive);
    assert(!snapshot->gameLeaseActive);
    assert(!snapshot->process);
    assert(snapshot->targetHwnd == 0u);
    assert(!snapshot->controllerBinding);

    // Final release closes the epoch. Reacquisition gets a fresh generation and
    // old tokens never regain authority.
    const auto next = controller.beginSeatActivation(1);
    assert(next.valid());
    assert(next.generation > ui.generation);
    assert(!controller.publishProcess(gameAgain, process));
    assert(!controller.releaseSeatLease(gameAgain));
    assert(controller.endSeatActivation(next));

    // Cross-Seat exact process ownership remains exclusive.
    const auto seat1 = controller.beginSeatActivation(1);
    const auto seat2 = controller.beginSeatActivation(2);
    assert(seat1.valid() && seat2.valid());
    assert(controller.publishProcess(seat1, process));
    assert(!controller.publishProcess(seat2, process));
    assert(controller.endSeatActivation(seat1));
    assert(controller.endSeatActivation(seat2));

    return 0;
}
