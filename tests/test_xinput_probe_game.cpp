#include "hydra/xinput_probe.hpp"

#include <cassert>
#include <string>
#include <vector>

void testXInputProbeGame() {
    using namespace hydra::controller;
    using namespace hydra::controller::ipc;
    using namespace hydra::controller::probe;

    const std::vector<std::wstring> snapshotArgs{
        L"--pipe", L"\\\\.\\pipe\\seat-a",
        L"--seat", L"1",
        L"--activation-generation", L"10",
        L"--source-generation", L"4",
        L"--mode", L"snapshot",
    };
    const auto snapshot = parseProbeArgs(snapshotArgs);
    assert(snapshot.has_value());
    assert(snapshot->pipeEndpoint == L"\\\\.\\pipe\\seat-a");
    assert(snapshot->seatId == 1);
    assert(snapshot->activationGeneration == 10);
    assert(snapshot->sourceGeneration == 4);
    assert(snapshot->mode == ProbeMode::Snapshot);

    auto invalidSeatArgs = snapshotArgs;
    invalidSeatArgs[3] = L"3";
    assert(!parseProbeArgs(invalidSeatArgs).has_value());

    auto missingPipeArgs = snapshotArgs;
    missingPipeArgs.erase(missingPipeArgs.begin(), missingPipeArgs.begin() + 2);
    assert(!parseProbeArgs(missingPipeArgs).has_value());

    const std::vector<std::wstring> vibrationArgs{
        L"--pipe", L"\\\\.\\pipe\\seat-b",
        L"--seat", L"2",
        L"--activation-generation", L"20",
        L"--source-generation", L"9",
        L"--mode", L"vibrate",
        L"--low", L"100",
        L"--high", L"200",
    };
    const auto vibration = parseProbeArgs(vibrationArgs);
    assert(vibration.has_value());
    assert(vibration->mode == ProbeMode::Vibrate);
    assert(vibration->lowFrequencyMotor == 100);
    assert(vibration->highFrequencyMotor == 200);

    auto incompleteVibrationArgs = vibrationArgs;
    incompleteVibrationArgs.resize(incompleteVibrationArgs.size() - 2);
    assert(!parseProbeArgs(incompleteVibrationArgs).has_value());

    VirtualXInputResponse connected;
    connected.status = ProtocolStatus::Ok;
    connected.hasState = true;
    connected.state.buttons = 1;
    connected.state.thumbLX = 111;
    assert(formatSnapshotLine(0, connected) ==
           "slot=0 status=Ok buttons=1 lx=111");

    VirtualXInputResponse disconnected;
    disconnected.status = ProtocolStatus::Disconnected;
    assert(formatSnapshotLine(1, disconnected) ==
           "slot=1 status=Disconnected");

    VirtualXInputResponse okVibration;
    okVibration.status = ProtocolStatus::Ok;
    assert(formatVibrationLine(okVibration) == "vibration status=Ok");
}
