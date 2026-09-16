# Process-Local XInput Adapter Design

Date: 2026-09-16
Status: Approved direction for controlled-process implementation

## Goal

Turn the already-proven Seat-private virtual XInput contract into a real Windows XInput ABI boundary inside a child process, without yet adding automatic DLL injection, DLL search-order deployment, game-specific hooks, or protected-title behavior.

The adapter must let a controlled child process call normal XInput-style functions and observe only its assigned Seat controller:

- `XInputGetState(0)` -> Seat-private state;
- `XInputGetState(1..3)` -> `ERROR_DEVICE_NOT_CONNECTED`;
- `XInputSetState(0)` -> vibration reverse-routed only to the same Seat source;
- stale or invalid Seat/source generations -> fail closed;
- no path falls back to machine-global XInput.

This remains controlled-process evidence, not a real-game compatibility claim.

## Existing Foundation

The branch already has:

- stable controller identity separated from runtime XInput slots;
- SeatRuntime / SessionController controller ownership;
- reconnect source generations;
- `VirtualXInputMapping` for logical slot 0;
- fixed-width bounded virtual-XInput protocol;
- named-pipe transport;
- deterministic two-process isolation harness proving state, vibration, stale generation, and independent restart behavior.

What is missing is the actual XInput ABI inside the target process. The current `xinput_probe_game` calls HydraSeat's protocol client directly, which a real game will not do.

## Chosen Approach

Build an explicitly loaded process-local adapter DLL first.

The controlled probe process loads the adapter by exact path with `LoadLibraryW`, resolves exported XInput functions with `GetProcAddress`, and calls them exactly as a game would call an XInput implementation.

Do not name or deploy the first adapter as a system XInput DLL such as `xinput1_4.dll`. The target is `hydra_xinput_adapter.dll` so the test cannot accidentally shadow Windows XInput for unrelated processes.

This separates two concerns:

1. process-local XInput ABI correctness and Seat isolation — implemented now;
2. automatic game loading/injection/proxy deployment — a later, separately reviewed compatibility mechanism.

## Adapter Configuration

Each launched child receives only process-local environment values created by the host:

- `HYDRA_XINPUT_PIPE`
- `HYDRA_XINPUT_SEAT_ID`
- `HYDRA_XINPUT_ACTIVATION_GENERATION`
- `HYDRA_XINPUT_SOURCE_GENERATION`

The adapter parses this configuration lazily on first API call and keeps an immutable process-local session configuration after successful initialization.

Rules:

- Seat ID must be 1 or 2;
- activation/source generations must be nonzero unsigned integers;
- pipe endpoint must be nonempty;
- malformed or missing configuration fails closed;
- configuration is never taken from persisted Seat configuration;
- no physical persistent controller ID or machine-global XInput slot is exposed to the child;
- changing environment variables after initialization does not retarget a live process.

## DLL ABI

The first DLL exports these Windows-compatible functions:

```cpp
DWORD WINAPI XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState);
DWORD WINAPI XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration);
DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
                                   XINPUT_CAPABILITIES* pCapabilities);
```

Exports use `extern "C"` and explicit `__declspec(dllexport)`.

### `XInputGetState`

For user index 0:

1. validate pointer and immutable process session;
2. send a bounded `GetState` request containing Seat ID, activation generation, source generation, and logical slot 0;
3. on `Ok + state`, translate HydraSeat `GamepadState` field-for-field into `XINPUT_STATE`;
4. otherwise return `ERROR_DEVICE_NOT_CONNECTED` and zero the caller buffer where safe.

For indices 1-3, return `ERROR_DEVICE_NOT_CONNECTED` without sending IPC.

For indices greater than 3, also fail closed with `ERROR_DEVICE_NOT_CONNECTED`.

### `XInputSetState`

For user index 0:

1. validate pointer/session;
2. send bounded `SetVibration` with exact motor values;
3. return `ERROR_SUCCESS` only for an `Ok` response;
4. all other statuses return `ERROR_DEVICE_NOT_CONNECTED`.

Indices other than 0 never send IPC and return `ERROR_DEVICE_NOT_CONNECTED`.

### `XInputGetCapabilities`

Many games probe capabilities before polling state, so the controlled adapter includes it from the start.

For user index 0:

1. require a valid process session;
2. verify the mapping is currently alive by issuing a `GetState` request;
3. if successful, return a conservative generic gamepad capability record with type `XINPUT_DEVTYPE_GAMEPAD` and subtype `XINPUT_DEVSUBTYPE_GAMEPAD`;
4. do not invent battery, audio, wireless, or device-specific extension claims;
5. otherwise return `ERROR_DEVICE_NOT_CONNECTED`.

Indices other than 0 return disconnected without IPC.

## Error Mapping

The game-facing ABI intentionally exposes less detail than HydraSeat internal statuses.

Map all of these to `ERROR_DEVICE_NOT_CONNECTED`:

- invalid/missing adapter configuration;
- invalid Seat or generation;
- `InvalidRequest`;
- `UnsupportedVersion`;
- `InvalidMapping`;
- `Disconnected`;
- `StaleBinding`;
- transport timeout/disconnect;
- backend failure.

The reason is fail-closed compatibility: the child must never interpret a HydraSeat infrastructure failure as permission to query machine-global XInput.

Null output/input pointers use `ERROR_BAD_ARGUMENTS` where the Windows XInput signature permits a returned error code.

## Controlled ABI Probe

Add `xinput_abi_probe_game.exe`.

It must not link against or directly call HydraSeat controller/protocol code. Its only HydraSeat-specific input is the adapter DLL path supplied by the test runner.

Flow:

1. host sets the four process-local `HYDRA_XINPUT_*` environment variables;
2. child loads `hydra_xinput_adapter.dll` by exact path;
3. child resolves `XInputGetState`, `XInputSetState`, and `XInputGetCapabilities` by exported symbol name;
4. child calls capabilities and state for slots 0-3;
5. vibration mode calls `XInputSetState(0)`;
6. child prints deterministic observations and exits with explicit nonzero codes on ABI/load/export/call failures.

No CRT `assert()` is used in process-level tests.

## Process-Level Acceptance Cases

### ABI/export correctness

- DLL loads by exact path in a child process;
- all required exports resolve;
- function calling convention/signatures are callable from the controlled probe;
- missing configuration fails closed instead of touching system XInput.

### Two-Seat isolation

With distinct synthetic states A and B:

- Game A adapter slot 0 reports only A;
- Game B adapter slot 0 reports only B;
- slots 1-3 are disconnected in both processes;
- the child never sees the other Seat endpoint or mapping values.

### Vibration

- Game A `XInputSetState(0)` mutates only Seat A receipt;
- Game B mutates only Seat B receipt;
- slots 1-3 do not mutate either source.

### Stale/lifecycle safety

- stale source generation -> disconnected;
- stale activation generation -> disconnected;
- Seat 1 restart can replace Game A session while a held Game B process continues;
- old Game A process/session cannot regain access by environment mutation.

### Fail-closed behavior

- missing/invalid DLL session configuration -> disconnected;
- pipe timeout/server loss -> disconnected;
- invalid logical user index -> disconnected without IPC;
- no call path invokes Windows system XInput as a fallback.

## Build Boundary

Create a dedicated Windows shared-library target:

- `hydra_xinput_adapter`

and a dedicated controlled probe:

- `xinput_abi_probe_game`

The adapter should link only the minimum HydraSeat IPC/protocol pieces needed for transport plus ordinary Windows system libraries. It must not link the existing native `controller_io.cpp` XInput backend because that would create a machine-global XInput fallback path inside the child.

The host-side process test continues to own synthetic backend state and named-pipe services.

Non-Windows builds do not build the DLL/probe target; portable protocol/service tests remain available.

## Security / Compatibility Boundary

Explicit non-goals for this stage:

- no automatic DLL injection;
- no remote-thread injection;
- no import-table patching;
- no detours/hooks inside arbitrary games;
- no system DLL replacement;
- no DLL copied into game directories;
- no anti-cheat/DRM/protected-process interaction;
- no attempt to bypass game/provider single-instance policy;
- no HidHide/device cloaking;
- no DirectInput or GameInput virtualization;
- no audio changes;
- no production GameLauncher refactor.

The controlled adapter is infrastructure for later compatibility mechanisms, not authorization to inject into arbitrary software.

## Future Deployment Boundary

After the ABI adapter is proven, a separate design may choose among:

1. opt-in per-game proxy DLL deployment where technically and legally appropriate;
2. a controlled launcher-assisted loading mechanism for unprotected titles;
3. title-specific compatibility modules.

Any such mechanism must preserve exact process ownership, reversible setup/cleanup, explicit compatibility evidence, and the existing prohibition on protected/anti-cheat bypasses.

## Test Strategy

1. Unit-test session environment parsing and protocol-to-XInput translation without loading a DLL.
2. Build the Windows DLL and inspect/resolve its required exports.
3. Add a child process that explicitly loads the DLL and calls the XInput ABI.
4. Extend the deterministic two-process host to prove state/vibration/stale/restart behavior through the DLL, not through direct protocol calls.
5. Run Windows MSVC Release full tests.
6. Build Debug, then run only the hardened process-level ABI isolation test with error-dialog suppression.
7. Keep the existing direct-protocol process test as a lower-layer regression test.

## Success Criterion

This stage is complete when Windows automated tests launch two independent child processes that explicitly load `hydra_xinput_adapter.dll`, call actual exported XInput functions, and prove that each child sees only its own Seat-private logical slot 0, vibration reverse-routes to the correct Seat, stale sessions fail closed, and restarting one Seat does not disturb the other.

Passing this criterion proves the process-local XInput ABI boundary. It does not yet prove automatic injection or compatibility with a real commercial game.
