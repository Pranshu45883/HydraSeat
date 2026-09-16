# Process-Local XInput Adapter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and prove a Windows DLL that exposes real XInput ABI calls to a controlled child process while forwarding only that process's Seat-private slot-0 state/vibration through the existing bounded HydraSeat IPC contract.

**Architecture:** Add a small immutable adapter-session/config layer, a Windows shared-library ABI surface, and an explicitly loading ABI probe. Extend controlled process testing so two child processes load the same DLL with different process-local Seat environments and prove state/vibration/stale/restart isolation without automatic injection or system-XInput fallback.

**Tech Stack:** C++20, Win32, XInput ABI types from `Xinput.h`, CMake/CTest, Windows named pipes, existing HydraSeat virtual-XInput protocol/service.

**Spec:** `docs/superpowers/specs/2026-09-16-xinput-process-adapter-design.md`

## Global Constraints

- Controlled-process evidence only; do not claim real-game compatibility.
- DLL target name is `hydra_xinput_adapter`, not a Windows system XInput DLL name.
- No automatic DLL injection, import patching, remote thread, detour, game-directory deployment, protected-title/anti-cheat/DRM behavior, HidHide, DirectInput/GameInput virtualization, audio changes, or GameLauncher rewrite.
- The child adapter never links or calls the native machine-global XInput backend (`controller_io.cpp` / `Xinput.lib`).
- Only logical slot 0 can be connected; all other indices fail closed without IPC.
- Invalid/missing configuration, stale generations, transport failure, and backend failure map to `ERROR_DEVICE_NOT_CONNECTED`.
- Process-level tests use explicit exit codes and suppress interactive Windows error/debug dialogs.

---

### Task 1: Immutable Adapter Session Configuration

**Files:**
- Create: `include/hydra/xinput_adapter_session.hpp`
- Create: `src/xinput_adapter_session.cpp`
- Create: `tests/test_xinput_adapter_session.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/test_hydra.cpp`

**Interfaces:**
- Produces:
```cpp
namespace hydra::controller::adapter {
struct SessionConfig {
    std::wstring pipeEndpoint;
    std::uint32_t seatId{0};
    std::uint64_t activationGeneration{0};
    std::uint64_t sourceGeneration{0};
    bool valid() const noexcept;
};

std::optional<SessionConfig> parseSessionConfig(
    std::wstring pipeEndpoint,
    std::wstring_view seatId,
    std::wstring_view activationGeneration,
    std::wstring_view sourceGeneration) noexcept;

std::optional<SessionConfig> loadSessionConfigFromEnvironment() noexcept;
}
```

- [ ] Write tests proving valid Seat 1/2 parsing, zero/overflow/non-decimal rejection, empty pipe rejection, and invalid Seat rejection.
- [ ] Run portable `hydra_tests`; expected RED because the new API does not exist.
- [ ] Implement strict unsigned decimal parsing without locale/exception dependence.
- [ ] On Windows, read the four `HYDRA_XINPUT_*` environment variables with bounded `GetEnvironmentVariableW`; on non-Windows return `std::nullopt`.
- [ ] Wire the unit test into `hydra_tests` and its runner.
- [ ] Run WSL/GCC and Windows MSVC unit tests; expected PASS.
- [ ] Commit: `feat: define process-local XInput adapter session`.

### Task 2: XInput ABI Adapter DLL

**Files:**
- Create: `src/xinput_adapter_dll.cpp`
- Create: `src/hydra_xinput_adapter.def`
- Create: `tests/test_xinput_adapter_exports.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/test_hydra.cpp`

**Interfaces:**
- Consumes: `adapter::SessionConfig`, `sendVirtualXInputRequest()`.
- Produces exact exports:
```cpp
DWORD WINAPI XInputGetState(DWORD, XINPUT_STATE*);
DWORD WINAPI XInputSetState(DWORD, XINPUT_VIBRATION*);
DWORD WINAPI XInputGetCapabilities(DWORD, DWORD, XINPUT_CAPABILITIES*);
```

- [ ] Write a Windows export-load test that expects `hydra_xinput_adapter.dll` to load by exact path and all three symbol names to resolve; first run must fail because the DLL target does not exist.
- [ ] Add `hydra_xinput_adapter` as a Windows `SHARED` target containing only `xinput_adapter_dll.cpp`, adapter-session code, virtual-XInput protocol/pipe code, and the `.def` file. Do not link `Xinput.lib` and do not compile `controller_io.cpp` into this target.
- [ ] Implement a once-initialized immutable session config. First API call freezes success or failure for that process.
- [ ] Implement `XInputGetState`: index 0 sends `GetState`, translates packet/buttons/triggers/thumb axes exactly; all failures/other indices return disconnected and zero output when non-null.
- [ ] Implement `XInputSetState`: index 0 sends `SetVibration`; all failures/other indices return disconnected.
- [ ] Implement `XInputGetCapabilities`: validate `dwFlags` as 0 or `XINPUT_FLAG_GAMEPAD`, verify the live mapping with `GetState`, then return zeroed conservative standard gamepad capabilities with `Type=XINPUT_DEVTYPE_GAMEPAD` and `SubType=XINPUT_DEVSUBTYPE_GAMEPAD`.
- [ ] Null pointer arguments return `ERROR_BAD_ARGUMENTS`.
- [ ] Build the DLL in Windows Release and run the export-load test; expected PASS.
- [ ] Inspect target link inputs/build definition to verify no `Xinput.lib` dependency is introduced into `hydra_xinput_adapter`.
- [ ] Commit: `feat: expose fail-closed XInput adapter ABI`.

### Task 3: Explicit-Load ABI Probe Process

**Files:**
- Create: `tools/xinput_abi_probe_game.cpp`
- Create: `tests/test_xinput_abi_probe.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/test_hydra.cpp`

**Interfaces:**
- Produces executable `xinput_abi_probe_game`.
- CLI:
```text
xinput_abi_probe_game --dll <absolute-path> --mode snapshot
xinput_abi_probe_game --dll <absolute-path> --mode vibrate --low <0..65535> --high <0..65535>
```
- The probe does not link HydraSeat protocol/controller code and does not link XInput. It uses `LoadLibraryW` + `GetProcAddress` only.

- [ ] Write parser/format tests for missing DLL, invalid mode, motor overflow, and deterministic snapshot/vibration output.
- [ ] Run unit tests; expected RED because probe parsing/API is missing.
- [ ] Implement strict CLI parsing.
- [ ] Implement exact-path DLL loading and symbol resolution for all three XInput exports.
- [ ] Snapshot mode: call `XInputGetCapabilities(0)`, then `XInputGetState(0..3)` and print deterministic status/state lines; require slot 0 success and slots 1-3 disconnected.
- [ ] Vibration mode: call `XInputSetState(0)` with exact motor values and print deterministic result.
- [ ] Return explicit nonzero exit codes for load/export/API/output failures; do not use `assert()`.
- [ ] Build and run probe unit tests; expected PASS.
- [ ] Commit: `test: add explicit-load XInput ABI probe`.

### Task 4: Two-Process ABI Isolation Evidence

**Files:**
- Create: `tests/test_xinput_adapter_process_isolation.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `hydra_xinput_adapter.dll`, `xinput_abi_probe_game.exe`, existing `VirtualXInputService` and `NamedPipeVirtualXInputServer`.
- Produces CTest: `XInputAdapterProcessIsolation`.

- [ ] Start with a Windows RED placeholder that returns an explicit nonzero code after setting `SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX`.
- [ ] Register the CTest with `$<TARGET_FILE:xinput_abi_probe_game>` and `$<TARGET_FILE:hydra_xinput_adapter>`; build Debug without running it, then run only this test once and observe the deliberate RED without a desktop dialog.
- [ ] Implement a test-local synthetic backend with unique Seat A/B states and independent vibration receipts.
- [ ] Launch Game A and Game B as distinct child processes with separate named-pipe endpoints and separate environment blocks containing the four `HYDRA_XINPUT_*` values.
- [ ] Verify Game A slot 0 shows only A and Game B slot 0 only B; both show slots 1-3 disconnected; PIDs are distinct.
- [ ] Run vibration probes with distinct values and verify reverse routing touches only the correct synthetic source.
- [ ] Launch stale-source and stale-activation children and verify nonzero probe exit plus zero backend mutation.
- [ ] Hold Game B inside its first state request, restart Seat A with a new activation generation/service, prove old A fails and new A passes, then release B and prove B completes unchanged.
- [ ] Add a child with missing adapter environment and verify it fails closed without requiring any server request.
- [ ] Run `XInputAdapterProcessIsolation` repeatedly at least 20 times in Debug to detect transport/stdout/process races.
- [ ] Commit: `test: prove process-local XInput ABI isolation`.

### Task 5: Final Verification and Branch Hygiene

**Files:**
- Update this plan's checkboxes only after evidence exists.

**Interfaces:**
- Produces a locally verified research branch only; no upstream PR yet.

- [ ] Run `cmake -S . -B build-wsl`, build `hydra_tests`, and run WSL CTest; expected portable suite PASS and Windows-only tests absent.
- [ ] Build Windows MSVC Release targets: `hydra_tests`, `hydra_xinput_adapter`, `xinput_abi_probe_game`, existing direct probe/isolation target, and new ABI isolation target.
- [ ] Run full Windows Release CTest; expected all tests PASS.
- [ ] Build only adapter/probe/ABI process targets in Debug, then run only `XInputAdapterProcessIsolation`; expected PASS without interactive dialogs.
- [ ] Run `git diff --check` and inspect branch diff for accidental audio, GameLauncher, injection, HidHide, DirectInput, or GameInput changes.
- [ ] Verify `git status --short` is clean after commits.
- [ ] Keep `research/xinput-process-adapter` local/fork-only until lower upstream controller PRs progress; do not open a new upstream PR in this task.
