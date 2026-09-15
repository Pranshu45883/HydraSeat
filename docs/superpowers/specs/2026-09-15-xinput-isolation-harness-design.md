# XInput Isolation Harness Design

Date: 2026-09-15
Status: Approved direction, implementation pending

## Goal

Prove the controller isolation contract across real Windows process boundaries before adding game DLL injection or title-specific compatibility code.

The harness must demonstrate that two independent child processes can each observe a private logical XInput namespace where only logical slot 0 is connected, while HydraSeat retains physical/runtime controller ownership and reverse-routes vibration to the correct Seat source.

This is controlled-process evidence only. It is not a real-game compatibility claim.

## Existing Contract

The current controller stack already defines:

- stable physical identity separately from runtime-only XInput slots;
- Seat-local controller ownership in `SessionController` / `SeatRuntime`;
- reconnect generations that invalidate stale runtime bindings;
- `VirtualXInputMapping`, where one Seat-owned controller projects to logical XInput slot 0 and logical slots 1-3 are disconnected.

What is missing is process isolation. Today `pollVirtualXInput()` and `setVirtualXInputVibration()` are normal in-process calls. They do not prove that Game A and Game B can each see a different logical slot-0 controller namespace.

## Chosen Approach

Build a deterministic host + two probe-process harness before implementing DLL injection.

### Components

1. `xinput_isolation_host`
   - owns the two Seat test mappings;
   - launches two child probe processes;
   - exposes one bounded, versioned local IPC endpoint per child;
   - answers logical-XInput queries using the mapping assigned to that child;
   - records vibration commands and routes them through the same Seat mapping;
   - rejects stale activation/source generations.

2. `xinput_probe_game`
   - acts like a minimal game process;
   - knows nothing about physical controller identities or other Seats;
   - asks only for logical XInput slots 0-3;
   - can issue vibration to a logical slot;
   - emits deterministic observations for the test runner.

3. `VirtualXInputProtocol`
   - small fixed-width request/response messages;
   - explicit protocol version;
   - no pointers, handles, or arbitrary payload lengths;
   - request types limited to `GetState`, `SetVibration`, and `Ping` for the first harness;
   - each request carries the expected Seat activation generation so stale clients fail closed.

The first implementation is a test/diagnostic harness, not the production injection transport. Its interfaces should be reusable where sensible, but no production claim depends on reuse.

## Data Flow

For Seat 1 / Game A:

`physical/runtime source A -> SeatRuntime(1) -> VirtualXInputMapping(Seat 1) -> host IPC endpoint A -> probe Game A logical slot 0`

For Seat 2 / Game B:

`physical/runtime source B -> SeatRuntime(2) -> VirtualXInputMapping(Seat 2) -> host IPC endpoint B -> probe Game B logical slot 0`

Logical slots 1-3 return disconnected for both processes.

A probe never receives the other Seat's source key, stable ID, runtime XInput slot, or mapping object.

## Deterministic Test Sources

The automated harness must not require two physical controllers to be attached in CI.

Introduce a narrow controller-I/O test seam behind the host-side virtual-XInput service:

- production/native path uses the existing controller I/O adapter;
- harness path uses two deterministic synthetic Seat sources;
- synthetic source A and B expose distinguishable button/axis states and independent vibration receipts;
- the seam must not be exposed to normal application/UI code.

Physical-controller validation remains a separate manual/Windows acceptance layer after the deterministic harness is green.

## Required Acceptance Cases

### Isolation

- Game A logical slot 0 receives only Seat 1 source A state.
- Game B logical slot 0 receives only Seat 2 source B state.
- State changes on source A do not appear in Game B.
- State changes on source B do not appear in Game A.
- logical slots 1-3 are disconnected in both child processes.

### Reverse vibration routing

- Game A `SetVibration(0)` records/applies vibration only to source A.
- Game B `SetVibration(0)` records/applies vibration only to source B.
- vibration to slots 1-3 returns disconnected and touches no source.

### Lifecycle / stale safety

- ending Seat 1 invalidates Game A requests without interrupting Game B;
- restarting Seat 1 gives a new activation generation;
- requests carrying the old generation are rejected;
- changing the runtime controller source generation makes the old mapping stale;
- stale state requests and stale vibration requests both fail closed.

### Process isolation

- each probe is a distinct OS process;
- killing Game A does not stop or corrupt Game B;
- restarting Game A receives only the new Seat 1 mapping/generation;
- malformed/unknown protocol messages are rejected without mutating controller state.

## IPC Boundary

Use Windows named pipes for the first Windows harness because they provide a local process boundary without introducing networking or external dependencies.

Rules:

- one server endpoint per launched probe;
- endpoint name generated by the host for the activation, not globally predictable configuration;
- child receives only the endpoint identifier and expected protocol/version information needed to connect;
- fixed maximum message size;
- bounded request timeout;
- one request -> one response;
- unknown versions/opcodes fail closed;
- disconnect is treated as child-session termination, not as permission to reassign a Seat.

The protocol is test-harness infrastructure, not a commitment that production injection must use named pipes forever.

## Production Compatibility Boundary

The future process-local compatibility adapter will implement the same observable contract:

- `XInputGetState(0)` -> Seat-private slot-0 state;
- `XInputGetState(1..3)` -> `ERROR_DEVICE_NOT_CONNECTED`;
- `XInputSetState(0)` -> reverse route to the Seat-owned controller;
- stale/invalid mappings -> fail closed;
- no global XInput-slot reshuffling;
- no physical controller identity persisted as an XInput user index.

The adapter/injection mechanism itself is explicitly out of scope for this harness task.

## GameLauncher Relationship

Do not refactor `GameLauncher` in the first harness implementation.

The harness may use a dedicated controlled-process launcher so the test can prove process boundaries without mixing production launch ownership changes into the same patch.

After the harness is green, the next separate design/PR should converge production `GameLauncher` on:

`launch -> retain exact process handle/creation identity -> publish to SessionController -> create process-local compatibility session`.

This prevents the current `CreateProcessW()`-then-immediate-handle-close behavior from being silently mixed into the controller proof.

## Error Handling

Fail closed on:

- invalid Seat ID;
- invalid/stale activation generation;
- invalid/stale controller source generation;
- unknown protocol version/opcode;
- wrong logical slot for a Seat-private mapping;
- child connection on the wrong endpoint;
- disconnected transport;
- malformed message size;
- controller I/O backend failure.

No error path may fall back to the machine-global XInput view.

## Test Strategy

1. Pure unit tests for protocol encoding/validation and virtual-XInput service dispatch.
2. Process-level Windows integration test launching two probe children against deterministic synthetic sources.
3. Lifecycle integration tests for child kill/restart and Seat generation rollover.
4. Windows native build/CTest as the authoritative process-boundary verification.
5. Non-Windows build keeps protocol/service unit tests where portable, while Windows process/named-pipe tests are skipped explicitly rather than faked.

## Non-goals

- no real-game support claim;
- no DLL injection/hooking;
- no anti-cheat/DRM/protected-process interaction;
- no global device cloaking;
- no DirectInput/GameInput-native game virtualization;
- no audio changes;
- no production GameLauncher rewrite;
- no new upstream PR until the existing review stack progresses.

## Success Criterion

The task is complete when a Windows native automated test launches two independent probe processes and proves, with deterministic evidence, that each process sees only its own Seat-private logical XInput slot 0, vibration reverse-routes to the correct Seat source, stale mappings fail closed, and one child can terminate/restart without disturbing the other.
