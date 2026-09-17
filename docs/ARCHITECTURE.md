# HydraSeat Architecture

Status: canonical high-level architecture for collaboration. The implementation is converging toward this structure incrementally; this document distinguishes the merged repository from the target production shape.

## 1. Product concepts

HydraSeat keeps four concepts separate:

- **Player** — a person/profile;
- **Seat** — one local gaming station and its owned hardware/runtime resources;
- **Game** — a user-facing/catalog identity;
- **LaunchTarget** — the executable/launcher contract HydraSeat can actually start and own.

A v1 Seat may own:

- display assignment and placement state;
- keyboard/mouse assignment;
- controller identity and transient runtime binding;
- audio endpoint/session routing state;
- active process tree;
- target window;
- compatibility activation;
- Seat-local rollback and stop/restart state.

The v1 product limit is exactly two active Seats. Seat 1 lifecycle changes must not implicitly tear down Seat 2, and vice versa.

## 2. Authority model

The production destination is one runtime authority:

```text
HydraSeat UI / control surface
            |
            | bounded, versioned IPC
            v
      hydra_host.exe
      SessionController
       /            \
      v              v
 SeatRuntime 1   SeatRuntime 2
      |              |
      +-- process    +-- process
      +-- window     +-- window
      +-- input      +-- input
      +-- controller +-- controller
      +-- audio      +-- audio
      +-- rollback   +-- rollback
```

`hydra_host.exe` is intended to be the sole runtime authority. The UI expresses intent; it does not become a second runtime state machine.

`SessionController` owns cross-Seat decisions and exactly two v1 `SeatRuntime` objects. A `SeatRuntime` owns only the mutable state for its Seat.

The current merged repository has introduced the `SessionController` / `SeatRuntime` authority contracts but still builds the existing `HydraSeat` application rather than a complete host/client process split. That split remains an incremental migration target, not a claim about current `main`.

## 3. Current merged runtime contracts

Current `main` contains:

- activation tokens with Seat ID and generation;
- exact runtime `ProcessIdentity` as PID plus creation identity;
- cross-Seat rejection of duplicate process/window/controller ownership;
- stable controller identity separated from runtime-only XInput slot identity;
- Seat-owned transient controller bindings;
- persisted controller selection by stable physical identity rather than enumeration index;
- a two-Seat configuration/UI model.

Runtime authority is fail-closed: stale or mismatched ownership evidence must not be converted into success.

## 4. Persisted state vs runtime state

Persisted configuration may contain stable/user-selected information such as:

- Seat ID;
- display/device identifiers;
- keyboard/mouse device paths;
- stable controller identity;
- Player/Game/LaunchTarget metadata when those schemas are introduced.

Persisted configuration must not use process-lifetime values as durable identity. In particular, do not persist:

- PID;
- HWND;
- Raw Input handles;
- process/Job handles;
- COM/interface pointers;
- enumeration-order controller indices;
- active runtime generations.

Runtime-only state is reconstructed from current evidence for each activation.

## 5. Process and window ownership

The ownership rule is stronger than "the process name looks right."

A production launch path should follow:

```text
begin Seat activation
  -> create/observe exact process ownership
  -> publish exact ProcessIdentity
  -> accept only windows owned by that identity/process tree
  -> activate Seat-local compatibility resources
  -> run
  -> reverse Seat-local rollback on stop/failure
```

Normal descendants should be owned from process-tree/Job evidence. Launcher handoff outside the owned tree requires an explicit bounded handoff contract. Failure to prove ownership means activation fails closed.

## 6. Controller boundary

Controller identity has two distinct layers:

- **stable physical identity** for Seat assignment/persistence;
- **runtime API identity** such as an XInput slot for the current session.

Enumeration order or friendly name is not stable identity. Controller ownership is transient runtime state scoped to the active Seat generation.

Reconnect-generation validation, explicit pairing, virtual XInput, process-local XInput compatibility, and related work must preserve this separation as those capabilities move from validated branches into `main`.

## 7. Audio boundary

Windows audio is a separate platform subsystem until a supported Seat-level integration contract is proven.

The intended order is:

1. read-only endpoint inventory;
2. read-only session observation with exact process identity and endpoint context;
3. isolated routing feasibility work;
4. only after support and rollback are proven, integration through the Seat runtime boundary.

Audio code must not become a second runtime authority. Experimental or undocumented routing mechanisms must stay out of the production path until they can prove target-scoped mutation, verification, rollback, and no impact on unrelated application/global audio state.

The eventual ownership direction is:

```text
SessionController -> SeatRuntime -> audio contract -> Windows audio backend
```

not `Windows audio backend -> runtime authority`.

## 8. Compatibility boundary

Game/process-specific compatibility belongs at the runtime edge. Normal launch orchestration should remain understandable without reading hook/interposition code.

Required compatibility capabilities are explicit. Missing required isolation means unsupported; HydraSeat must not silently fall back to global input, controller, audio, or window behavior.

HydraSeat does not bypass DRM, anti-cheat, protected processes, credentials, authentication, or deliberate single-instance/security restrictions.

## 9. Windows mutation rule

A risky mutation is a transaction:

```text
capture -> apply -> verify
```

Failure is handled by:

```text
reverse rollback -> verify safe state
```

Broad cleanup such as killing by process name, clearing unrelated persisted application settings, resetting global devices, or sweeping unrelated state is not an acceptable rollback mechanism.

## 10. Source/build boundaries

The long-term responsibility-level component model is documented in [MAINTAINABLE_ARCHITECTURE.md](MAINTAINABLE_ARCHITECTURE.md). The current repository does not need a one-shot rewrite to match that graph.

Key dependency rules remain:

- core/runtime contracts should not depend on UI;
- OS-specific Windows code stays at the Windows boundary;
- UI does not become runtime authority;
- diagnostics/experiments do not become production success authority;
- a new library/interface exists only for a real process, ABI, platform, security, optional-capability, or independently changing responsibility boundary.

## 11. Evidence discipline

Architecture is not proof of compatibility. Keep evidence labels literal:

- unit/pure;
- controlled/synthetic;
- controlled real process/open-source target;
- physical hardware;
- commercial/real game;
- community report.

Passing a controlled process test is not equivalent to physical two-Seat or real-game acceptance.

See [STATUS.md](STATUS.md) for the dated implementation snapshot and [COLLABORATION_CONTRACT.md](COLLABORATION_CONTRACT.md) for the engineering rules applied to changes.
