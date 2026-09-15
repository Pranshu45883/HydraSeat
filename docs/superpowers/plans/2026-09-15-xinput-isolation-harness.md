# XInput Isolation Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove with a Windows native automated test that two independent child processes each see only their Seat-private logical XInput slot 0, while vibration and stale-generation handling remain correctly isolated.

**Architecture:** Keep the existing Seat/runtime/controller contracts unchanged. Use a bounded fixed-size protocol, a transport-neutral `VirtualXInputService`, Windows named pipes, and a small `xinput_probe_game` child executable. The CTest integration host owns two synthetic controller backends and launches two real child processes; no DLL injection, real-game hook, audio change, or production `GameLauncher` rewrite is part of this work.

**Tech Stack:** C++20, CMake/CTest, Win32 named pipes and process APIs, existing HydraSeat controller/runtime contracts.

**Spec:** `docs/superpowers/specs/2026-09-15-xinput-isolation-harness-design.md`

## Global Constraints

- Controlled-process evidence only; do not claim real-game compatibility.
- No DLL injection/hooking, anti-cheat/DRM/protected-process work, global device cloaking, audio changes, or production `GameLauncher` rewrite.
- One Seat-private controller is exposed only as logical XInput slot 0; logical slots 1-3 are disconnected.
- Unknown protocol versions/opcodes, malformed messages, invalid activation generations, and stale controller source generations fail closed.
- No error path may fall back to the machine-global XInput view.
- Automated process-level validation must not require physical controllers.
- Windows child-process verification must be non-interactive: tests use explicit exit codes instead of CRT `assert`, and the parent sets `SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX` before launching children so failures do not open desktop debug/error dialogs.
- Do not open another upstream PR while the existing #8-#16 stack is awaiting review.

---

### Task 1: Bounded Virtual XInput Protocol

**Files:**
- Create: `include/hydra/virtual_xinput_protocol.hpp`
- Create: `src/virtual_xinput_protocol.cpp`
- Create: `tests/test_virtual_xinput_protocol.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: protocol version 1, fixed encoded request size 28 bytes, fixed encoded response size 20 bytes; `Ping`, `GetState`, `SetVibration`; encode/decode helpers returning `std::optional` on malformed input.

- [x] Write protocol round-trip and malformed-input tests.
- [x] Verify the tests fail before protocol implementation.
- [x] Implement bounded encode/decode logic with explicit little-endian fields and enum validation.
- [x] Verify protocol tests and existing unit tests pass.
- [x] Commit: `8b737dc test: define bounded virtual XInput protocol`.

### Task 2: Transport-Neutral Virtual XInput Service

**Files:**
- Create: `include/hydra/virtual_xinput_service.hpp`
- Create: `src/virtual_xinput_service.cpp`
- Create: `tests/test_virtual_xinput_service.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `VirtualXInputMapping`, `InventorySnapshot`, Task 1 request/response types.
- Produces: `IVirtualControllerBackend`, `NativeVirtualControllerBackend`, and `VirtualXInputService::handle()`.

- [x] Write failing tests for Seat/generation validation, slot-0 state, slots 1-3 disconnect, vibration routing, and stale source generation.
- [x] Implement the minimal backend seam and dispatcher.
- [x] Verify service tests and existing controller tests pass.
- [x] Commit: `031921d feat: add transport-neutral virtual XInput service`.

### Task 3: Bounded Windows Named-Pipe Transport

**Files:**
- Create: `include/hydra/virtual_xinput_pipe.hpp`
- Create: `src/virtual_xinput_pipe.cpp`
- Create: `tests/test_virtual_xinput_pipe.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 protocol and Task 2 service.
- Produces: `NamedPipeVirtualXInputServer::serveOne(timeoutMs)` and `sendVirtualXInputRequest(endpoint, request, timeoutMs)`.

- [x] Write failing transport tests for one request/one response, timeout, malformed payload rejection, and non-Windows fail-closed behavior.
- [x] Implement one-message bounded named-pipe transport with fixed payload size and finite timeout.
- [x] Verify transport tests and unit suite pass.
- [x] Commit: `4f83548 feat: add bounded virtual XInput pipe transport`.

### Task 4: Probe Game Process

**Files:**
- Create: `include/hydra/xinput_probe.hpp`
- Create: `src/xinput_probe.cpp`
- Create: `tools/xinput_probe_game.cpp`
- Create: `tests/test_xinput_probe_game.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 3 client transport.
- Produces: `xinput_probe_game.exe`; snapshot mode queries logical slots 0-3 and vibration mode sends slot-0 vibration; all failures return explicit nonzero exit codes.

- [x] Write failing parser/output/run-probe tests.
- [x] Implement probe argument parsing and deterministic output.
- [x] Add the `xinput_probe_game` executable target.
- [x] Verify probe tests pass.
- [x] Commit: `8445ed6 test: add isolated XInput probe process`.

### Task 5: Two-Process Isolation RED Test and Safe Windows Launcher

**Files:**
- Modify: `tests/test_xinput_process_isolation.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `xinput_probe_game.exe`, `VirtualXInputService`, `NamedPipeVirtualXInputServer`.
- Produces: CTest target `XInputProcessIsolation` that acts as the controlled `xinput_isolation_host`.

- [x] Replace the current Windows placeholder return 99 with a test host scaffold that still intentionally fails after proving it can launch one probe without a dialog.

Required Windows scaffold behavior:
```cpp
SetErrorMode(SEM_FAILCRITICALERRORS |
             SEM_NOGPFAULTERRORBOX |
             SEM_NOOPENFILEERRORBOX);
```
Use explicit `if (!condition) { std::cerr << ...; return N; }` checks only; do not use `assert()` in this process-level test.

- [x] Add a CMake executable target from `tests/test_xinput_process_isolation.cpp` and a CTest entry that passes `$<TARGET_FILE:xinput_probe_game>` as argv[1].
- [x] Build the integration target in Windows Debug configuration without running it. Expected: compile/link PASS.
- [x] Run only `XInputProcessIsolation` once. Expected RED: nonzero exit from the deliberate final failure, with no desktop debugger/error dialog.
- [x] Remove the deliberate failure only after the safe-launch RED behavior has been observed.

### Task 6: Deterministic Two-Seat State Isolation

**Files:**
- Modify: `tests/test_xinput_process_isolation.cpp`

**Interfaces:**
- Test-local synthetic backend records per-source state and vibration receipts; no production class gains test-only state.
- Each probe child receives only its own endpoint, Seat ID, activation generation, and source generation.

- [x] Add synthetic source A state with a unique `buttons`/`thumbLX` pair and source B state with a different pair.
- [x] Start one named-pipe server endpoint per Seat and launch two distinct `xinput_probe_game` child processes in snapshot mode.
- [x] Capture each child stdout through an inherited anonymous pipe; close all unused pipe ends in parent and child.
- [x] Verify Game A output contains Seat A slot-0 values and slots 1-3 `Disconnected`, and contains no Seat B values.
- [x] Verify Game B output contains Seat B slot-0 values and slots 1-3 `Disconnected`, and contains no Seat A values.
- [x] Verify both child process IDs are nonzero and distinct.
- [x] Run `XInputProcessIsolation`; expected PASS.

### Task 7: Reverse Vibration and Stale-Generation Isolation

**Files:**
- Modify: `tests/test_xinput_process_isolation.cpp`

**Interfaces:**
- Reuses the Task 6 synthetic backend; vibration receipts are keyed by the exact Seat binding/runtime source.

- [x] Launch Game A in vibration mode with one motor pair and Game B with a different pair.
- [x] Verify source A receives only Game A's vibration values and source B receives only Game B's values.
- [x] Verify a request using a stale source generation returns a nonzero probe exit and does not mutate either vibration receipt.
- [x] Verify a request using a stale activation generation is rejected without mutating either source.
- [x] Run `XInputProcessIsolation`; expected PASS.

### Task 8: Independent Child Termination and Restart

**Files:**
- Modify: `tests/test_xinput_process_isolation.cpp`

**Interfaces:**
- Process restart uses a new Seat 1 activation generation and a new endpoint; Seat 2 service/process remains unchanged.

- [x] Launch both probes and keep the Seat 2 server/service alive.
- [x] Terminate or let Game A exit, then launch a restarted Game A against a new Seat 1 activation generation.
- [x] Verify an old-generation Game A request is rejected.
- [x] Verify restarted Game A sees only Seat A and Game B still sees only Seat B.
- [x] Verify Game B process/service state was not torn down or reassigned during Seat 1 restart.
- [x] Run `XInputProcessIsolation`; expected PASS.

### Task 9: Verification, Documentation, and Research-Branch Commit

**Files:**
- Modify only if needed for test registration/comments: `CMakeLists.txt`, `tests/test_xinput_process_isolation.cpp`
- Update: `docs/superpowers/specs/2026-09-15-xinput-isolation-harness-design.md` only if implementation reveals a concrete contract correction.

**Interfaces:**
- Produces final controlled-process evidence; no upstream PR or production compatibility hook.

- [x] Run portable unit build/tests under WSL/GCC; expected all portable tests PASS and Windows-only process test explicitly skipped/not registered.
- [x] Configure/build Windows MSVC **Release** targets first; expected PASS.
- [x] Run full Windows CTest in Release with `CTEST_OUTPUT_ON_FAILURE=1`; expected PASS without interactive error/debug dialogs.
- [x] Build Windows Debug targets without running the full Debug suite.
- [x] Run only the now-hardened `XInputProcessIsolation` Debug test once; expected PASS without interactive dialogs.
- [x] Review `git diff --check`, branch diff, and ensure no audio/GameLauncher/injection files changed.
- [x] Commit the completed harness locally on `research/xinput-isolation-harness`.
- [x] Do not push or open a PR unless explicitly requested later.
