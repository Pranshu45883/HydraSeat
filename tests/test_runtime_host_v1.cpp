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

    const auto emptyPresentation = host.seatRuntimeInfo(1);
    assert(emptyPresentation.has_value());
    assert(emptyPresentation->seatId == 1);
    assert(emptyPresentation->titleUtf8.empty());
    assert(emptyPresentation->providerIdUtf8.empty());
    assert(host.setSeatGamePresentation(1, "Controlled Game", "custom"));
    const auto presentation = host.seatRuntimeInfo(1);
    assert(presentation.has_value());
    assert(presentation->titleUtf8 == "Controlled Game");
    assert(presentation->providerIdUtf8 == "custom");
    assert(!host.setSeatGamePresentation(1, "Controlled Game", "unknown"));
    assert(!host.setSeatGamePresentation(2, "No process", "custom"));

#if defined(_WIN32)
    const auto idleKeyboard = host.inputActivity(
        hydra::hostipc::HardwareDeviceKind::Keyboard);
    assert(idleKeyboard.has_value());
    assert(idleKeyboard->sequence == 0);
    assert(idleKeyboard->stableIdUtf8.empty());
    host.observePhysicalInput(
        hydra::DeviceType::Keyboard,
        LR"(\\?\HID#VID_1234&PID_5678#7&11111111&0&0000#{4D1E55B2-F16F-11CF-88CB-001111000030})");
    const auto activeKeyboard = host.inputActivity(
        hydra::hostipc::HardwareDeviceKind::Keyboard);
    assert(activeKeyboard.has_value());
    assert(activeKeyboard->sequence > 0);
    assert(!activeKeyboard->stableIdUtf8.empty());

    host.observePhysicalInput(
        hydra::DeviceType::Mouse,
        LR"(\\?\HID#VID_1234&PID_5678#7&22222222&0&0000#{4D1E55B2-F16F-11CF-88CB-001111000030})");
    const auto activeMouse = host.inputActivity(
        hydra::hostipc::HardwareDeviceKind::Mouse);
    assert(activeMouse.has_value());
    assert(activeMouse->sequence > activeKeyboard->sequence);
    assert(!activeMouse->stableIdUtf8.empty());
#endif
    assert(!host.inputActivity(
        hydra::hostipc::HardwareDeviceKind::Display).has_value());

    const auto seat2 = host.beginSeatActivation(2);
    assert(seat2.valid());
    const auto beforeDuplicate = host.snapshot();
    assert(!host.publishProcess(seat2, process));
    assert(host.snapshot().authorityRevision == beforeDuplicate.authorityRevision);

    assert(host.bindTargetWindow(seat1, process, static_cast<std::uintptr_t>(0x1000)));
    const auto afterWindow = host.snapshot();
    assert(afterWindow.seats[0].windowOwned);

    assert(host.endSeatActivation(seat1));
    const auto clearedPresentation = host.seatRuntimeInfo(1);
    assert(clearedPresentation.has_value());
    assert(clearedPresentation->titleUtf8.empty());
    assert(clearedPresentation->providerIdUtf8.empty());
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
