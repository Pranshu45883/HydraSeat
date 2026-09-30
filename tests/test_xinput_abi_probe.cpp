#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/xinput_abi_probe.hpp"

#include <cassert>
#include <string>
#include <vector>

void testXInputAbiProbe() {
    using namespace hydra::controller::abi_probe;

    const std::vector<std::wstring> snapshotArgs{
        L"--dll", L"C:\\test\\hydra_xinput_adapter.dll",
        L"--mode", L"snapshot",
    };
    const auto snapshot = parseProbeArgs(snapshotArgs);
    assert(snapshot.has_value());
    assert(snapshot->dllPath == L"C:\\test\\hydra_xinput_adapter.dll");
    assert(snapshot->mode == ProbeMode::Snapshot);

    const std::vector<std::wstring> vibrationArgs{
        L"--dll", L"adapter.dll",
        L"--mode", L"vibrate",
        L"--low", L"123",
        L"--high", L"65535",
    };
    const auto vibration = parseProbeArgs(vibrationArgs);
    assert(vibration.has_value());
    assert(vibration->mode == ProbeMode::Vibrate);
    assert(vibration->lowFrequencyMotor == 123);
    assert(vibration->highFrequencyMotor == 65535);

    const std::vector<std::wstring> missingDll{L"--mode", L"snapshot"};
    const std::vector<std::wstring> badMode{
        L"--dll", L"a.dll", L"--mode", L"bad"};
    const std::vector<std::wstring> motorOverflow{
        L"--dll", L"a.dll", L"--mode", L"vibrate",
        L"--low", L"65536", L"--high", L"1"};
    const std::vector<std::wstring> snapshotWithMotor{
        L"--dll", L"a.dll", L"--mode", L"snapshot", L"--low", L"1"};
    assert(!parseProbeArgs(missingDll).has_value());
    assert(!parseProbeArgs(badMode).has_value());
    assert(!parseProbeArgs(motorOverflow).has_value());
    assert(!parseProbeArgs(snapshotWithMotor).has_value());

    assert(formatCapabilitiesLine(0, 1, 1) ==
           "capabilities status=0 type=1 subtype=1");
    assert(formatStateLine(0, 0, 7, 64, 1234) ==
           "slot=0 status=0 packet=7 buttons=64 lx=1234");
    assert(formatStateLine(1, 1167, 0, 0, 0) ==
           "slot=1 status=1167");
    assert(formatVibrationLine(0) == "vibration status=0");
}
