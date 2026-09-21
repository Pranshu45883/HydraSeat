# HydraSeat Current Status

Snapshot date: **2026-09-21**

This file is a dated engineering snapshot. It distinguishes what is merged into upstream `main`, what has controlled validation on contributor branches, what is research-only, and what still lacks required evidence.

## Evidence labels

- **Merged** — present in upstream `main`.
- **Validated in contributor branch** — implemented and tested in a same-repository contributor branch, but not part of upstream `main` yet.
- **Research / under review** — evidence or implementation is still being reviewed and must not be treated as production capability.
- **Pending physical/real-game evidence** — architecture or controlled tests exist, but the user-facing claim has not been demonstrated at the required evidence level.

## Upstream merged baseline

Current upstream baseline for this snapshot: `a690615` (`fix(audio): harden Windows audio experiment boundaries`).

Merged behavior includes:

- two v1 Seat IDs with configuration limited to Seat 1 / Seat 2;
- stable controller IDs persisted instead of controller enumeration indices;
- UI controller selection using stable physical identity;
- stable physical controller identity separated from runtime-only XInput slot identity;
- controller inventory for physical identity and current runtime sources;
- `SessionController` owning cross-Seat runtime ownership decisions;
- `SeatRuntime` activation generations and stale-token rejection;
- exact runtime process identity using PID + creation identity;
- cross-Seat duplicate process/window/controller ownership rejection;
- Seat-owned transient controller bindings and controller poll/vibration routing;
- reconnect-generation tracking that invalidates stale controller bindings;
- exact `GameLauncher` root-process ownership with suspended launch, retained handle, Seat-local stop/rollback, and ownership publication before resume;
- isolated `ProcessIdentity` contract used across runtime/process ownership;
- registered CTest execution in CI with Release assertions preserved for the focused engine tests;
- merged Windows audio endpoint inventory, session observation, and bounded routing-feasibility experiment/hardening.

These capabilities establish ownership contracts; they do **not** prove complete two-player game isolation.

## Upstream review queue

### Windows audio work

The Windows audio foundation is merged separately from runtime/controller work:

- render endpoint inventory — read-only discovery foundation;
- audio session observation — process/session observation with creation-time identity;
- routing feasibility — research into process-targeted Windows routing without changing the global default.

Current review requirements before treating this work as production-ready include:

- use only documented/real endpoint identity properties; do not fabricate property keys as compatibility fallbacks;
- keep Windows audio code at the Windows/platform boundary rather than making it a second runtime authority;
- preserve endpoint context when observing sessions;
- distinguish complete/authoritative observation from partial scans;
- keep undocumented routing experiments out of the normal product target and ordinary automatic tests;
- never use broad rollback that clears unrelated persisted application audio policy;
- prove that a target session actually moved to the intended endpoint before claiming routing support.

The current routing experiment result is useful **negative/feasibility evidence**, not a production routing solution.

## Validated in contributor branches, not merged

The following work has controlled validation in same-repository `minseong/*` branches and is intentionally integrated only after prerequisites land in `main`:

### Controller pairing and reconnect safety

- explicit pairing of stable physical controller identity to the current XInput runtime source;
- source-generation validation across reconnects;
- stale bindings fail closed.

Current review branch: `minseong/controller-button-pairing` (PR #32). The reconnect-generation prerequisite is already merged.

### Seat-local virtual XInput

- logical Seat-local XInput slot contract;
- Seat source exposed as logical slot 0;
- unrelated logical slots reported disconnected;
- stale activation/source generation rejected.

### Two-process isolation harness

Controlled Windows child processes demonstrated:

- independent Seat A / Seat B state visibility;
- no cross-Seat logical-slot exposure;
- vibration routed back to the correct Seat source;
- stale generation rejection;
- restarting one controlled child while the other remains active.

This is **controlled/synthetic process evidence**, not real-game evidence.

### Process-local XInput ABI adapter

A controlled adapter DLL/probe path has validated XInput ABI behavior through explicit loading and Seat-private configuration. Automatic game injection/interposition is deliberately separate and is not claimed as production-ready.

### Launch/process ownership

A contributor branch validates a Seat launch path that:

- starts the controlled child suspended;
- passes Seat-private runtime configuration through a child environment block;
- captures exact PID + creation identity;
- publishes runtime ownership before resume;
- retains the process handle for Seat-local stop/rollback;
- rejects duplicate launch for the same Seat;
- stops/restarts one Seat without terminating the other controlled Seat;
- rolls back failed process creation.

Process-tree/Job ownership is under review in `minseong/seat-process-tree-ownership` (PR #33). Arbitrary launcher handoff remains follow-up work.

The dependent controller compatibility sequence is preserved under:

- `minseong/staging/controller-virtual-xinput`;
- `minseong/staging/xinput-process-isolation`;
- `minseong/staging/xinput-process-adapter`.

Seat process/XInput launch integration is intentionally deferred until both the process-tree PR and the adapter chain land; the old fork branch conflicts with the newer `GameLauncher` ownership model and is not migrated as authority.

These staging branches are preservation/integration queues only. They are not merge-ready and must be rebuilt on the newest `main` after each prerequisite lands. The former fork `main` and any unlisted legacy fork branches are intentionally not migrated; they are retired prototypes and must not be treated as current implementation or architecture authority.

## Not yet production-proven

The following must remain unclaimed until stronger evidence exists:

- real-game process-local XInput interposition across representative titles;
- complete keyboard/mouse isolation with two physical device sets;
- controller isolation across reconnects and relevant controller APIs on physical hardware;
- independent physical audio routing for two Seats;
- exact process-tree/launcher handoff for real launchers;
- Seat-specific display/window behavior across real multi-display setups;
- two different commercial games running simultaneously under independent Seats;
- lawful same-title/two-instance scenarios where supported by the title;
- crash/reboot/watchdog/emergency recovery restoring ordinary Windows state;
- clean-machine install/uninstall and signed release artifacts.

## Collaboration state

Current parallel work split:

- `ot4562-glitch`: Seat/runtime authority, process ownership/lifecycle, controller/runtime, compatibility;
- `Pranshu45883`: program UI/UX and Windows audio endpoint/session/routing;
- shared: UI/runtime and audio/runtime contracts plus physical/real-game acceptance.

`Pranshu45883/HydraSeat` is now the single canonical repository. Contributor work uses short-lived same-repository branches; the former long-lived contributor-fork workflow is retired. Dependent implementation may move ahead only on clearly named staging branches and must be rebuilt on current `main`, freshly verified, and immediately reviewable/mergeable before a normal PR is opened.

See [COLLABORATION_CONTRACT.md](COLLABORATION_CONTRACT.md) for the invariant-level rules and [ROADMAP.md](ROADMAP.md) for integration order.
