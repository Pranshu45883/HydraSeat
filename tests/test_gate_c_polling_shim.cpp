#include "hydra/gate_c_adapter.h"
#include "hydra/gate_c_shim_api.h"
#include "hydra/win32_iat_patch.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

static_assert(sizeof(HydraGateCShimConfigV1) ==
              HYDRA_GATE_C_SHIM_CONFIG_V1_BYTES);
static_assert(sizeof(HydraGateCShimConfigV2) ==
              HYDRA_GATE_C_SHIM_CONFIG_V2_BYTES);
static_assert(sizeof(HydraGateCShimConfigV3) ==
              HYDRA_GATE_C_SHIM_CONFIG_V3_BYTES);
static_assert(sizeof(HydraGateCShimStatusV1) ==
              HYDRA_GATE_C_SHIM_STATUS_V1_BYTES);

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct FakeWriter {
    std::size_t calls{0};
    std::size_t failCall{0};
    std::size_t secondFailCall{0};
    bool protectionFailure{false};
    bool protectionRestored{true};
};

hydra::gatec::IatWriteResult fakeWrite(
    std::uintptr_t* address, std::uintptr_t expected,
    std::uintptr_t replacement, void* opaque) noexcept {
    auto& fake = *static_cast<FakeWriter*>(opaque);
    ++fake.calls;
    if ((fake.failCall != 0 && fake.calls == fake.failCall) ||
        (fake.secondFailCall != 0 && fake.calls == fake.secondFailCall)) {
        hydra::gatec::IatWriteResult result;
        result.protectionFailure = fake.protectionFailure;
        result.protectionRestored = fake.protectionRestored;
        result.systemError = 5;
        return result;
    }
    if (address == nullptr || *address != expected) {
        hydra::gatec::IatWriteResult result;
        result.systemError = 13;
        return result;
    }
    *address = replacement;
    hydra::gatec::IatWriteResult result;
    result.success = true;
    return result;
}

std::vector<hydra::gatec::PollingIatSlot> slotsFor(
    std::array<std::uintptr_t, 3>& values) {
    using hydra::gatec::PollingIatSlot;
    using hydra::gatec::PollingImport;
    return {
        PollingIatSlot{PollingImport::GetAsyncKeyState, &values[0], 101, 201},
        PollingIatSlot{PollingImport::GetKeyState, &values[1], 102, 202},
        PollingIatSlot{PollingImport::GetKeyboardState, &values[2], 103, 203},
    };
}

void testTransactionLifecycle() {
    std::array<std::uintptr_t, 3> values{101, 102, 103};
    auto slots = slotsFor(values);
    FakeWriter writer;
    hydra::gatec::PollingIatPatchSet patches;
    auto report = patches.install(slots, fakeWrite, &writer);
    check(report && report.patchedMask == hydra::gatec::kPollingImportMask &&
              values == std::array<std::uintptr_t, 3>{201, 202, 203},
          "all polling imports install transactionally");
    report = patches.install(slots, fakeWrite, &writer);
    check(report.status == hydra::gatec::IatPatchStatus::AlreadyInstalled,
          "repeated install is idempotent");
    report = patches.uninstall(fakeWrite, &writer);
    check(report && report.restoredMask == hydra::gatec::kPollingImportMask &&
              values == std::array<std::uintptr_t, 3>{101, 102, 103},
          "uninstall restores every original pointer exactly");
    report = patches.uninstall(fakeWrite, &writer);
    check(static_cast<bool>(report), "repeated uninstall is idempotent");

    report = patches.install(slots, fakeWrite, &writer);
    check(report && patches.installed(), "install succeeds after uninstall");
    check(static_cast<bool>(patches.uninstall(fakeWrite, &writer)),
          "reinstalled patch set uninstalls cleanly");

    writer = {};
    check(static_cast<bool>(patches.install(slots, fakeWrite, &writer)),
          "patch set installs for uninstall-failure coverage");
    writer.failCall = 4;
    report = patches.uninstall(fakeWrite, &writer);
    check(report.status == hydra::gatec::IatPatchStatus::RollbackFailure &&
              !report.rollbackComplete && patches.installed(),
          "uninstall failure remains visible and retryable");
    writer.failCall = 0;
    check(static_cast<bool>(patches.uninstall(fakeWrite, &writer)) &&
              values == std::array<std::uintptr_t, 3>{101, 102, 103},
          "repeated uninstall retries and restores the remaining pointer");
}

void testMalformedAndAmbiguousSets() {
    using hydra::gatec::IatPatchStatus;
    std::array<std::uintptr_t, 3> values{101, 102, 103};
    auto slots = slotsFor(values);
    FakeWriter writer;

    hydra::gatec::PollingIatPatchSet missing;
    check(missing.install(std::span(slots).first(2), fakeWrite, &writer).status ==
              IatPatchStatus::MissingImport,
          "missing polling import is rejected");

    auto duplicateSlots = slots;
    duplicateSlots.push_back(slots.front());
    hydra::gatec::PollingIatPatchSet duplicate;
    check(duplicate.install(duplicateSlots, fakeWrite, &writer).status ==
              IatPatchStatus::DuplicateImport,
          "duplicate polling import is rejected");

    auto invalidSlots = slots;
    invalidSlots[1].address = nullptr;
    hydra::gatec::PollingIatPatchSet invalid;
    check(invalid.install(invalidSlots, fakeWrite, &writer).status ==
              IatPatchStatus::InvalidImage,
          "invalid import slot metadata is rejected");

    values[0] = 201;
    hydra::gatec::PollingIatPatchSet alreadyPatched;
    check(alreadyPatched.install(slots, fakeWrite, &writer).status ==
              IatPatchStatus::AlreadyPatched,
          "an already patched import is rejected");
}

void testFailureRollback() {
    using hydra::gatec::IatPatchStatus;
    std::array<std::uintptr_t, 3> values{101, 102, 103};
    const auto slots = slotsFor(values);
    FakeWriter writer;
    writer.failCall = 3;
    hydra::gatec::PollingIatPatchSet patches;
    const auto report = patches.install(slots, fakeWrite, &writer);
    check(report.status == IatPatchStatus::PatchFailure &&
              report.rollbackComplete && report.patchedMask == 0 &&
              values == std::array<std::uintptr_t, 3>{101, 102, 103} &&
              !patches.installed(),
          "partial install failure rolls back all prior pointers");

    writer = {};
    writer.failCall = 1;
    writer.protectionFailure = true;
    const auto protection = patches.install(slots, fakeWrite, &writer);
    check(protection.status == IatPatchStatus::ProtectionFailure &&
              protection.rollbackComplete &&
              values == std::array<std::uintptr_t, 3>{101, 102, 103},
          "protection failure is visible and leaves imports unchanged");

    writer = {};
    writer.failCall = 1;
    writer.protectionFailure = true;
    writer.protectionRestored = false;
    hydra::gatec::PollingIatPatchSet protectionRestoreFailure;
    const auto restoreProtection = protectionRestoreFailure.install(
        slots, fakeWrite, &writer);
    check(restoreProtection.status == IatPatchStatus::RollbackFailure &&
              !restoreProtection.rollbackComplete &&
              values == std::array<std::uintptr_t, 3>{101, 102, 103},
          "protection-restore failure is reported as incomplete rollback");

    writer = {};
    writer.failCall = 3;
    writer.secondFailCall = 4;
    hydra::gatec::PollingIatPatchSet retryableRollback;
    const auto incomplete = retryableRollback.install(
        slots, fakeWrite, &writer);
    check(incomplete.status == IatPatchStatus::RollbackFailure &&
              !incomplete.rollbackComplete && retryableRollback.installed(),
          "partial-install rollback failure retains a retryable record");
    writer = {};
    check(static_cast<bool>(retryableRollback.uninstall(
              fakeWrite, &writer)) &&
              values == std::array<std::uintptr_t, 3>{101, 102, 103},
          "incomplete install rollback can be retried to exact restoration");
}

void testProfiledSubsetTransactions() {
    using hydra::gatec::CursorFocusIatSlot;
    using hydra::gatec::CursorFocusImport;
    using hydra::gatec::PollingIatSlot;
    using hydra::gatec::PollingImport;
    using hydra::gatec::RawInputIatSlot;
    using hydra::gatec::RawInputImport;

    std::uintptr_t keyState = 101;
    const std::array<PollingIatSlot, 1> polling{
        PollingIatSlot{PollingImport::GetKeyState, &keyState, 101, 201}};
    FakeWriter writer;
    hydra::gatec::PollingIatPatchSet pollingPatches;
    const auto pollingMask =
        hydra::gatec::pollingImportBit(PollingImport::GetKeyState);
    auto report = pollingPatches.installProfiled(
        polling, pollingMask, fakeWrite, &writer);
    check(report && report.discoveredMask ==
              hydra::gatec::pollingImportBit(PollingImport::GetKeyState) &&
              keyState == 201,
          "profiled polling transaction patches only the declared import");
    writer = {};
    report = pollingPatches.uninstall(fakeWrite, &writer);
    check(report && keyState == 101,
          "profiled polling transaction restores its exact subset");

    std::array<std::uintptr_t, 2> cursorValues{301, 302};
    const std::array<CursorFocusIatSlot, 2> cursor{
        CursorFocusIatSlot{CursorFocusImport::ClipCursor,
                           &cursorValues[0], 301, 401},
        CursorFocusIatSlot{CursorFocusImport::ReleaseCapture,
                           &cursorValues[1], 302, 402}};
    hydra::gatec::CursorFocusIatPatchSet cursorPatches;
    writer = {};
    const auto cursorMask =
        hydra::gatec::cursorFocusImportBit(CursorFocusImport::ClipCursor) |
        hydra::gatec::cursorFocusImportBit(CursorFocusImport::ReleaseCapture);
    report = cursorPatches.installProfiled(
        cursor, cursorMask, fakeWrite, &writer);
    check(report && cursorValues ==
              std::array<std::uintptr_t, 2>{401, 402},
          "profiled cursor transaction accepts a strict allowlisted subset");
    writer = {};
    check(cursorPatches.uninstall(fakeWrite, &writer) &&
              cursorValues == std::array<std::uintptr_t, 2>{301, 302},
          "profiled cursor subset restores exactly");

    std::array<std::uintptr_t, 2> rawValues{501, 502};
    const std::array<RawInputIatSlot, 2> raw{
        RawInputIatSlot{RawInputImport::RegisterRawInputDevices,
                        &rawValues[0], 501, 601},
        RawInputIatSlot{RawInputImport::GetRawInputData,
                        &rawValues[1], 502, 602}};
    hydra::gatec::RawInputIatPatchSet rawPatches;
    writer = {};
    const auto rawMask =
        hydra::gatec::rawInputImportBit(RawInputImport::RegisterRawInputDevices) |
        hydra::gatec::rawInputImportBit(RawInputImport::GetRawInputData);
    report = rawPatches.installProfiled(
        raw, rawMask, fakeWrite, &writer);
    check(report && rawValues == std::array<std::uintptr_t, 2>{601, 602},
          "profiled Raw Input transaction accepts the declared subset");
    writer = {};
    check(rawPatches.uninstall(fakeWrite, &writer) &&
              rawValues == std::array<std::uintptr_t, 2>{501, 502},
          "profiled Raw Input subset restores exactly");
}

#ifdef _WIN32

HydraGateCAdapterInputEventV1 keyEvent(std::uint32_t vkey) {
    HydraGateCAdapterInputEventV1 event{};
    event.struct_size = sizeof(event);
    event.kind = HYDRA_GATE_C_ADAPTER_INPUT_KEYBOARD;
    event.key_transition = HYDRA_GATE_C_ADAPTER_KEY_DOWN;
    event.vkey = vkey;
    return event;
}

void testRealPollingSemantics() {
    HydraGateCAdapterHandle adapter = hydra_gate_c_adapter_create();
    check(adapter != nullptr, "polling shim test adapter is created");
    const HWND targetWindow = CreateWindowExW(
        0, L"STATIC", L"HydraSeat polling shim import surface",
        WS_OVERLAPPED, 0, 0, 100, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(targetWindow != nullptr,
          "controlled polling test owns a target window");
    auto event = keyEvent(0x41);
    check(hydra_gate_c_adapter_apply_input(adapter, 1, &event) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "adapter receives a key-down edge");

    HydraGateCShimConfigV2 config{};
    config.struct_size = sizeof(config);
    config.api_version = HYDRA_GATE_C_SHIM_API_VERSION;
    config.seat_id = 1;
    config.process_id = GetCurrentProcessId();
    config.target_window = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(targetWindow));
    auto legacyConfig = config;
    legacyConfig.struct_size = HYDRA_GATE_C_SHIM_CONFIG_V1_BYTES;
    legacyConfig.api_version = 1;
    check(hydra_gate_c_shim_install(adapter, &legacyConfig) ==
              HYDRA_GATE_C_SHIM_STRUCT_VERSION_MISMATCH,
          "legacy shim config version fails closed before any patch");
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto nativeGetKeyState = reinterpret_cast<SHORT(WINAPI*)(int)>(
        GetProcAddress(user32, "GetKeyState"));
    check(nativeGetKeyState != nullptr,
          "native GetKeyState address is available before install");
    check(hydra_gate_c_shim_install(adapter, &config) ==
              HYDRA_GATE_C_SHIM_OK,
          "real current-process polling imports install");
    HydraGateCAdapterControlStateV1 control{};
    control.struct_size = sizeof(control);
    control.cursor_x = 10;
    control.cursor_y = 20;
    control.clip_enabled = 1;
    control.virtual_foreground = 1;
    control.virtual_capture = 1;
    control.clip_right = 100;
    control.clip_bottom = 100;
    check(hydra_gate_c_adapter_apply_control(adapter, 2, &control) ==
              HYDRA_GATE_C_ADAPTER_OK,
          "polling regression configures the shared cursor/focus adapter state");
    check((static_cast<std::uint16_t>(GetAsyncKeyState(0x41)) & 0xffffu) ==
              0x8001u &&
              (static_cast<std::uint16_t>(GetAsyncKeyState(0x41)) & 0xffffu) ==
                  0x8000u,
          "ordinary GetAsyncKeyState preserves high bit and one-shot edge");
    check((static_cast<std::uint16_t>(GetKeyState(0x41)) & 0xffffu) ==
              0x8000u,
          "ordinary GetKeyState exposes down state with toggle bit clear");
    std::array<BYTE, 256> keyboard{};
    check(GetKeyboardState(keyboard.data()) != FALSE &&
              keyboard[0x41] == 0x80u && keyboard[0x42] == 0u &&
              std::all_of(keyboard.begin(), keyboard.end(),
                          [](BYTE value) { return (value & 0x01u) == 0; }),
          "ordinary GetKeyboardState returns the complete Seat-local array");
    check(GetKeyState(300) == nativeGetKeyState(300),
          "out-of-domain virtual keys pass through to the exact original");
    SetLastError(ERROR_SUCCESS);
    check(GetKeyboardState(nullptr) == FALSE &&
              GetLastError() == ERROR_INVALID_PARAMETER,
          "active GetKeyboardState rejects null without a partial write");
    // Cursor/focus APIs are intentionally not exercised in polling-only mode;
    // they remain native pass-through unless the P3-API-03 capability is enabled.

    HydraGateCShimStatusV1 status{};
    status.struct_size = sizeof(status);
    check(hydra_gate_c_shim_get_status(&status) == HYDRA_GATE_C_SHIM_OK &&
              status.lifecycle == HYDRA_GATE_C_SHIM_ACTIVE &&
              status.generation == 1 &&
              status.expected_api_mask == HYDRA_GATE_C_SHIM_POLLING_API_MASK &&
               status.patched_api_mask == HYDRA_GATE_C_SHIM_POLLING_API_MASK,
          "active shim diagnostics expose generation and function masks");

    check(hydra_gate_c_shim_mark_adapter_unavailable() ==
              HYDRA_GATE_C_SHIM_ADAPTER_UNAVAILABLE,
          "adapter loss switches the installed shim to fail-closed mode");
    keyboard.fill(0x55u);
    check(GetKeyState(0x41) == 0 &&
              GetKeyboardState(keyboard.data()) == FALSE &&
              keyboard[0] == 0x55u && keyboard[0x41] == 0x55u,
          "fail-closed polling calls return no state and never partially write caller buffers");

    check(hydra_gate_c_shim_uninstall() == HYDRA_GATE_C_SHIM_OK,
          "real polling imports uninstall after fail-closed transition");
    status = {};
    status.struct_size = sizeof(status);
    check(hydra_gate_c_shim_get_status(&status) == HYDRA_GATE_C_SHIM_OK &&
              status.lifecycle == HYDRA_GATE_C_SHIM_INACTIVE &&
              status.restored_api_mask == HYDRA_GATE_C_SHIM_POLLING_API_MASK &&
              status.rollback_complete == 1,
          "uninstall diagnostics prove exact complete restoration");
    check(hydra_gate_c_shim_uninstall() == HYDRA_GATE_C_SHIM_OK,
          "real repeated uninstall is idempotent");

    check(hydra_gate_c_shim_install(adapter, &config) ==
              HYDRA_GATE_C_SHIM_OK,
          "real polling imports reinstall after restoration");
    status = {};
    status.struct_size = sizeof(status);
    check(hydra_gate_c_shim_get_status(&status) == HYDRA_GATE_C_SHIM_OK &&
              status.lifecycle == HYDRA_GATE_C_SHIM_ACTIVE &&
              status.generation == 2,
          "each successful reinstall advances the diagnostics generation");
    check((static_cast<std::uint16_t>(GetKeyState(0x41)) & 0x8000u) != 0,
          "reinstalled ordinary polling call still uses adapter state");
    check(hydra_gate_c_shim_uninstall() == HYDRA_GATE_C_SHIM_OK,
          "reinstalled real polling imports restore again");
    hydra_gate_c_adapter_destroy(adapter);
    DestroyWindow(targetWindow);
}

#endif

} // namespace

int main() {
    testTransactionLifecycle();
    testMalformedAndAmbiguousSets();
    testFailureRollback();
    testProfiledSubsetTransactions();
#ifdef _WIN32
    testRealPollingSemantics();
#endif
    std::cout << "Gate C controlled polling shim tests passed.\n";
    return EXIT_SUCCESS;
}
