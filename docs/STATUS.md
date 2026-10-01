# HydraSeat Current Status

Snapshot date: **2026-10-01**

This is a dated engineering snapshot of the canonical HydraSeat integration. It distinguishes implemented/automated evidence from physical or real-game acceptance evidence.

## Evidence labels

- **Integrated** — present in the canonical backend integration intended for main.
- **Automated validated** — covered by build/test/acceptance tooling.
- **Pending physical/manual evidence** — implementation exists, but the corresponding real-hardware, real-game, clean-machine, or signing claim has not been demonstrated at that evidence level.

## Canonical backend integration

The canonical backend now converges the previous staged backend work into one authority model instead of reintroducing the old fork runtime.

Implemented integration includes:

- exactly two v1 Seats;
- SessionController / SeatRuntime generation-scoped ownership;
- exact process identity using PID + creation identity;
- cross-Seat process/window/controller ownership rejection;
- stable physical controller identity separated from runtime XInput identity;
- reconnect generation checks and fail-closed stale binding behavior;
- Seat-local controller pairing and virtual/process-local XInput foundations;
- process/group/window/display ownership and recovery foundations;
- crash journal, reset, watchdog, startup/recovery policy;
- installer/update/privilege/support transaction foundations;
- portable two-player setup model;
- hardware/input/display/controller diagnostics;
- provider/profile, compatibility, and community-pipeline foundations;
- release acceptance and release-validation tooling;
- signing/scope/schema fixed-input validation;
- canonical host control authority and IPC protocol v2;
- host-owned Windows audio routing integration.

Legacy fork modules are not automatically treated as authority. In particular, duplicate controller runtime, launcher authority, or UI-owned runtime state is not reintroduced when the canonical ControllerInventory, SessionController, RuntimeHost, and host transport already own that responsibility.

## Canonical host/control boundary

Production mutation authority is owned by hydra_host.exe.

The host/control contract is now:

- versioned host IPC **v2**;
- Control role required for mutation;
- read-only clients may obtain snapshots without mutation authority;
- UiConfiguration and GameProcess leases are distinct and may coexist inside the same Seat generation;
- ending a game lease removes its PID/HWND authority immediately without requiring the UI lease to end;
- stale lease/token/generation evidence remains fail-closed;
- compatibility begin/endSeatActivation APIs remain wrappers over the game-lease path;
- UI leases are scoped to the named-pipe control connection that acquired them;
- disconnecting or killing that control client releases its UI leases automatically;
- UI code does not receive or own a SessionController pointer.

This removes the earlier risk that the program UI could become a second runtime authority.

## Windows audio

Windows audio remains a platform boundary, but production mutation is now routed through the canonical host rather than a separate UI authority.

The implemented path includes:

- render endpoint inventory;
- audio session observation using exact process identity;
- host-side process/Seat validation;
- target-scoped route/reset commands through host IPC;
- Control-role and UI-lease requirements for mutation;
- exact process creation-identity checks before mutation;
- status reporting for process/session/endpoint/identity/routing/OS failures.

Automated validation is not equivalent to proof on two physical render devices. Independent two-Seat physical audio remains a manual acceptance item.

## Automated validation

The canonical backend stack has automated coverage for the authority boundary and release tooling. The Windows host-control validation includes:

- protocol tests;
- runtime lease tests;
- RuntimeHost tests;
- transport-session tests;
- named-pipe end-to-end tests;
- hydra_host.exe build;
- hydraseat_hostctl.exe build.

The current integration also fixes issues found while auditing the old fork:

- missing cstring include in the Gate C external harness;
- a non-Windows include/guard portability defect in the game runtime requirement resolver;
- preserved code that existed on disk but was absent from the CMake build graph;
- missing signing/scope/schema fixed inputs in release tooling.

Automated acceptance/release checks provide reproducible engineering evidence only. They do not upgrade simulated, synthetic, or software-only results into physical or commercial-game evidence.

## UI integration

The UI/UX convergence is required to preserve the host authority boundary:

- polling is read-only through host snapshots;
- all UI mutations use one persistent Control connection;
- UI-lease ownership therefore follows the named-pipe connection lifetime;
- controller pairing is submitted as a host command;
- audio route/reset is submitted as a host command;
- a game-active Seat may independently acquire/release a UI configuration lease;
- the UI must never deactivate a game merely to enter configuration mode;
- another control client's UI lease is displayed as busy rather than impersonated or released.

## Still pending physical/manual evidence

The following remain unclaimed until the appropriate evidence is collected:

- two independent physical keyboard/mouse assignments without bleed;
- controller isolation across reconnect/replug on representative physical hardware and APIs;
- independent audio on two physical render endpoints;
- real multi-display placement/focus behavior;
- exact launcher handoff across representative real launchers;
- two different commercial games running simultaneously under independent Seats;
- lawful same-title/two-instance scenarios where supported by the title;
- crash/reboot/watchdog/emergency recovery on a real Windows installation;
- clean-machine install/update/uninstall;
- production signing trust and verification on released artifacts;
- protected/anti-cheat scenarios refusing unsupported activation safely.

## Collaboration and repository state

Pranshu45883/HydraSeat is the single canonical repository.

The integration policy is:

- one canonical backend/runtime authority;
- UI/UX is a client of that authority;
- Windows/platform code remains behind platform boundaries;
- experimental diagnostics do not become success authority;
- legacy fork code is migrated only when it adds non-duplicated capability to the current design.

See ARCHITECTURE.md, ROADMAP.md, and COLLABORATION_CONTRACT.md for the corresponding invariants and remaining acceptance work.
