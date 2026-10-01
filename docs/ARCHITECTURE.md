# HydraSeat Architecture

Status: canonical high-level architecture as of **2026-10-01**. The host/client authority split is implemented in the canonical integration; physical and real-game acceptance remain separate evidence gates.

## 1. Product concepts

HydraSeat keeps four concepts separate:

- **Player** — a person/profile;
- **Seat** — one local gaming station and its owned hardware/runtime resources;
- **Game** — a user-facing/catalog identity;
- **LaunchTarget** — the executable/launcher contract HydraSeat can actually start and own.

A v1 Seat may own display placement, input assignments, controller binding, process/window identity, audio routing state, compatibility activation, and Seat-local rollback state. The v1 limit is exactly two active Seats.

## 2. Canonical authority model

Production mutation authority is centralized:

    HydraSeat UI / control clients
                |
                | bounded host IPC v2
                v
          hydra_host.exe
          RuntimeHost
          SessionController
           /            \
          v              v
     SeatRuntime 1   SeatRuntime 2

hydra_host.exe is the sole production runtime authority. UI and diagnostic clients express intent through the host protocol; they do not construct a second runtime state machine.

SessionController owns cross-Seat decisions and exactly two v1 SeatRuntime objects. A SeatRuntime owns only the mutable runtime state for its Seat.

Host IPC v2 exposes read-only snapshots and bounded control commands. Mutation requires the Control role.

## 3. Lease model

Runtime ownership distinguishes two independent leases inside one Seat generation:

- **UiConfiguration** — temporary authority for supported configuration mutations;
- **GameProcess** — process/window authority for the running game.

The leases may coexist. Releasing UiConfiguration must not stop the game. Ending GameProcess removes PID/HWND authority immediately even when a UI lease remains.

Compatibility begin/end activation entry points are wrappers over the game-lease path; they are not a second authority model.

A UI lease acquired through host IPC belongs to that named-pipe connection. Disconnect or client death automatically releases the connection-owned UI leases. A different client must not impersonate or release them.

## 4. Runtime identity and fail-closed rules

Runtime identity is exact, not name-based:

- process identity is PID + creation identity;
- target windows must belong to the owned process/process tree;
- controller persistence uses stable physical identity, not enumeration order;
- XInput slots are runtime-only identities;
- active generations and runtime handles are never persisted as durable identity.

Stale generations, stale process identity, ambiguous controller evidence, cross-Seat ownership conflicts, or unsupported mutation paths fail closed.

## 5. Process and window ownership

The launch path follows:

    begin GameProcess lease
      -> create/observe exact process ownership
      -> publish exact ProcessIdentity
      -> accept only owned windows/process tree
      -> activate Seat-local compatibility resources
      -> run
      -> reverse Seat-local rollback on stop/failure

For the implemented custom-executable path, hydra_host owns GameLauncher. A launch command requires the Control role and that connection's UiConfiguration lease. GameLauncher acquires the GameProcess lease through RuntimeHost, creates the target suspended inside a kill-on-close Seat Job, publishes exact PID + creation identity before resume, and ends the GameProcess lease only after the exact owned Job/process has been stopped and verified safe.

Normal descendants are owned from process-tree/Job evidence. Out-of-tree launcher handoff requires an explicit bounded contract; process-name scanning is not ownership evidence.

## 6. Controller boundary

Controller identity has two layers:

- stable physical identity for Seat assignment and persistence;
- runtime API identity such as the current XInput slot.

Controller inventory and pairing preserve reconnect/source generations. Controller pairing is a host mutation. UI clients submit the selected stable physical identity plus the current XInput runtime slot; RuntimeHost validates the current inventory before publishing the binding.

Seat-local virtual/process-local XInput compatibility must preserve the same ownership and generation rules.

## 7. Audio boundary

Windows audio remains a Windows platform subsystem, but production route/reset mutation is host-owned.

The direction is:

    SessionController / RuntimeHost
        -> host audio contract
        -> Windows audio backend

Read-only endpoint/session observation may be consumed by UI diagnostics. A route/reset request must be tied to an exact host-owned process identity and an authorized UI configuration lease.

Audio code must not become an independent runtime authority. Target-scoped verification, rollback, and physical two-endpoint evidence remain required before claiming broad production compatibility.

## 8. UI boundary

The Qt UI is a client:

- polling uses a read-only host connection;
- mutations use one persistent Control connection;
- UI configuration leases therefore follow that control connection lifetime;
- Seats UI acquires/releases only UiConfiguration leases;
- it never deactivates a GameProcess lease to enter configuration mode;
- controller pairing, audio mutation, and launch/stop intent are host commands;
- launch/stop require the connection-owned UiConfiguration lease and do not transfer GameProcess authority into the UI;
- another control client's UI lease is shown as busy instead of being treated as locally owned.

The UI does not receive SessionController or RuntimeHost pointers.

## 9. Compatibility boundary

Game/process-specific compatibility belongs at the runtime edge. Missing required isolation means unsupported; HydraSeat must not silently fall back to global input, controller, audio, or window behavior.

HydraSeat does not bypass DRM, anti-cheat, protected processes, credentials, authentication, or deliberate security restrictions.

## 10. Windows mutation rule

Risky mutation is a transaction:

    capture -> apply -> verify

Failure is handled by:

    reverse rollback -> verify safe state

Crash journal, reset, watchdog, startup/recovery, installer/update, privilege, and support tooling exist to preserve this rule. Broad cleanup of unrelated process/device/application state is not an acceptable rollback mechanism.

## 11. Source/build boundaries

Key dependency rules are:

- core/runtime contracts do not depend on UI;
- Windows-specific code stays at the platform boundary;
- UI does not become runtime authority;
- diagnostics and experiments do not become production success authority;
- legacy fork modules are not reintroduced when canonical modules already own the responsibility;
- stale source that still targets obsolete authority contracts is removed rather than kept as unbuilt pseudo-implementation;
- non-duplicated preserved utilities must be registered in the build graph and tested instead of merely existing on disk;
- a new interface/library exists only for a real process, ABI, platform, security, optional-capability, or independently changing responsibility boundary.

## 12. Evidence discipline

Architecture and automated tests are not physical compatibility proof. Evidence labels remain literal:

- unit/pure;
- controlled/synthetic;
- controlled real process/open-source target;
- physical hardware;
- commercial/real game;
- community report.

Passing host IPC, release acceptance, or controlled process tests does not by itself prove two physical Seats, two physical audio devices, or commercial-game compatibility.

See STATUS.md for the dated implementation snapshot, ROADMAP.md for remaining acceptance work, and COLLABORATION_CONTRACT.md for collaboration invariants.
