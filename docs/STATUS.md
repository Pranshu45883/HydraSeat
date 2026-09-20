# HydraSeat Current Status

Snapshot date: **2026-09-18**

This file is a dated engineering snapshot. It distinguishes what is merged into upstream `main`, what has controlled validation on contributor branches, what is research-only, and what still lacks required evidence.

## Evidence labels

- **Merged** — present in upstream `main`.
- **Validated in contributor branch** — implemented and tested in a contributor fork/branch, but not part of upstream `main` yet.
- **Research / under review** — evidence or implementation is still being reviewed and must not be treated as production capability.
- **Pending physical/real-game evidence** — architecture or controlled tests exist, but the user-facing claim has not been demonstrated at the required evidence level.

## Upstream merged baseline

Current upstream baseline for this snapshot: `6258c61` (`feat: make SeatRuntime own controller bindings`).

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
- Seat-owned transient controller bindings and controller poll/vibration routing.

These capabilities establish ownership contracts; they do **not** prove complete two-player game isolation.

## Upstream review queue

### Reconnect-safe controller binding

A merge-ready controller follow-up tracks runtime source generation across disconnect/reconnect and invalidates stale bindings instead of trusting a reused XInput slot.

Until merged, current `main` should not be documented as reconnect-generation-safe.

### Windows audio work

The Windows audio track is being developed separately from runtime/controller work:

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

The current routing experiment result is useful **negative evidence**, not a production routing solution.

## Validated in contributor branches, not merged

The following work has controlled validation in the `ot4562-glitch` fork/branches but is intentionally being integrated only after prerequisites land in upstream `main`:

### Controller pairing and reconnect safety

- explicit pairing of stable physical controller identity to the current XInput runtime source;
- source-generation validation across reconnects;
- stale bindings fail closed.

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

Process-tree/Job ownership and arbitrary launcher handoff remain follow-up work.

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
- `Pranshu45883`: Windows audio endpoint/session/routing research.

Dependent implementation may move ahead in contributor forks, but upstream pull requests should be rebuilt on current `main`, freshly verified, and immediately reviewable/mergeable when opened.

See [COLLABORATION_CONTRACT.md](COLLABORATION_CONTRACT.md) for the invariant-level rules and [ROADMAP.md](ROADMAP.md) for integration order.
