#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/runtime_host.hpp"

#include <cassert>
#include <cstdint>

int main() {
    using namespace hydra::runtime;

    RuntimeHost host;

    const auto initial = host.snapshot();
    assert(initial.authorityRevision == 1);
    assert(initial.seats[0].seatId == 1);
    assert(initial.seats[1].seatId == 2);
    assert(!initial.seats[0].active);
    assert(!initial.seats[1].active);

    const auto invalid = host.beginSeatActivation(3);
    assert(!invalid.valid());
    assert(host.snapshot().authorityRevision == initial.authorityRevision);

    const auto seat1 = host.beginSeatActivation(1);
    assert(seat1.valid());
    const auto afterBegin = host.snapshot();
    assert(afterBegin.authorityRevision == initial.authorityRevision + 1);
    assert(afterBegin.seats[0].active);
    assert(afterBegin.seats[0].generation == seat1.generation);

    const ProcessIdentity process{4321, 987654321};
    assert(host.publishProcess(seat1, process));
    const auto afterProcess = host.snapshot();
    assert(afterProcess.authorityRevision == afterBegin.authorityRevision + 1);
    assert(afterProcess.seats[0].processOwned);

    const auto seat2 = host.beginSeatActivation(2);
    assert(seat2.valid());
    const auto beforeDuplicate = host.snapshot();
    assert(!host.publishProcess(seat2, process));
    assert(host.snapshot().authorityRevision == beforeDuplicate.authorityRevision);

    assert(host.bindTargetWindow(seat1, process, static_cast<std::uintptr_t>(0x1000)));
    const auto afterWindow = host.snapshot();
    assert(afterWindow.seats[0].windowOwned);

    assert(host.endSeatActivation(seat1));
    const auto afterEnd = host.snapshot();
    assert(!afterEnd.seats[0].active);
    assert(!afterEnd.seats[0].processOwned);
    assert(!afterEnd.seats[0].windowOwned);
    assert(!host.endSeatActivation(seat1));
    assert(host.snapshot().authorityRevision == afterEnd.authorityRevision);

    assert(host.endSeatActivation(seat2));
    assert(!host.snapshot().seats[1].active);

    return 0;
}
