# HydraSeat Maintainable Architecture

Status: proposed migration destination for the production codebase.

This document defines the structure HydraSeat should converge toward. It is deliberately smaller than the historical build graph. It is not a request for a one-shot rewrite: migrations should be incremental and preserve verified behavior.

## 1. Design rule

A component exists because it hides a design decision that changes for a different reason than the components around it. It does not exist because a roadmap packet, test milestone, helper class, or source file exists.

This follows the information-hiding criterion described by D. L. Parnas: module boundaries should improve flexibility and comprehensibility by hiding likely-to-change design decisions, rather than mirroring the sequence of processing steps.

For HydraSeat this means six production responsibilities are enough as the long-term default:

1. `hydra_core`
2. `hydra_runtime`
3. `hydra_windows`
4. `hydra_launch`
5. `hydra_compatibility`
6. `hydra_ui`

A new library or interface needs a concrete reason to change independently. One implementation, one caller, or a hypothetical future backend is not enough.

## 2. Authority model

`hydra_host.exe` is the only runtime authority.

The host owns one `SessionController`. The controller owns:

- the current `RuntimeSession`;
- exactly two v1 `SeatRuntime` objects;
- machine-wide recovery state;
- the transitions that affect both Seats.

Each `SeatRuntime` owns only its own mutable runtime state:

- selected launch target and immutable activation plan;
- exact process tree/Job ownership;
- target window identity and placement state;
- input compatibility activation;
- controller mapping;
- audio routing;
- Seat-local rollback and stop/restart lifecycle.

The UI, discovery providers, community data, and diagnostics never become runtime authorities.

## 3. Production components

### `hydra_core`

Contains stable product data and bounded contracts only:

- `SeatId` and stable hardware identifiers;
- persisted Seat, Player and Game metadata schemas;
- `LaunchTarget` data contract;
- compatibility capability/profile schema;
- runtime-independent result and error types;
- bounded serialization primitives shared across trusted process boundaries.

It must not depend on Win32 UI, Windows mutation code, network/community services, or game-store SDKs.

Persisted data must never contain HWNDs, PIDs, Raw Input handles, Job handles, enumeration-order indices, COM pointers, or other process-lifetime identities.

### `hydra_runtime`

Contains the state machine and transaction orchestration:

- `SessionController`;
- `SeatRuntime`;
- session and Seat lifecycle state;
- activation ordering;
- rollback ordering;
- fail-closed policy;
- ports required from the OS and compatibility layers.

The runtime decides *when* a resource is allowed to change, but does not contain platform-specific mutation code.

A risky activation follows one understandable transaction:

`capture -> apply -> verify`

Failure follows:

`reverse rollback -> verify safe state`

If safe-state verification cannot be proven, the state is `RecoveryRequired`; the runtime must not manufacture a successful result.

### `hydra_windows`

Contains Windows implementations of runtime ports:

- process creation, Job objects and exact process identity;
- top-level window observation and placement;
- Raw Input/device inventory;
- displays and topology;
- audio endpoints/sessions/routing;
- XInput/DirectInput/controller inventory;
- HidHide integration;
- privileged/recovery adapters where required.

Windows APIs and documented platform contracts are the default implementation foundation. OS-specific code should not leak into `hydra_core` or UI models.

### `hydra_launch`

Contains everything whose reason to change is "how a target is found and prepared for launch":

- optional game discovery;
- Steam or other store/provider metadata readers;
- manual executable/custom launcher definitions;
- `LaunchTarget` construction;
- immutable launch planning and preflight;
- TwoPlayerSetup generation/selection.

Discovery is optional. A provider/store identity is metadata, not a prerequisite for runtime ownership.

A Game is a user-facing/catalog concept. A LaunchTarget is the executable contract the host can actually start and own. One Game may have several LaunchTargets.

### `hydra_compatibility`

Contains optional game/process compatibility capabilities at the runtime edge:

- Raw Input redirection/virtualization;
- polled keyboard-state virtualization;
- cursor/focus/clip virtualization;
- XInput/DirectInput adaptation;
- narrowly scoped per-title compatibility recipes;
- trusted materialization that is reversible and validated.

Normal launch must be understandable without reading this component. Compatibility capabilities are selected explicitly by validated requirements. Missing required capability means unsupported; it must never silently degrade to global input or synthetic success.

Third-party compatibility work enters only through this boundary after provenance and license review.

### `hydra_ui`

Contains presentation state and native control surfaces only:

- localization/accessibility;
- game/target selection state;
- UI-only persisted choices;
- minimal native views.

The UI may request commands through bounded IPC. It may not launch game processes directly, mutate global Windows state, own Seat lifecycle, reinterpret a failed backend result as success, or expose implementation-phase jargon as the normal product workflow.

## 4. LaunchTarget, not game detection, is the runtime contract

Game detection is a convenience. The runtime only needs a target it can start, identify and own.

The target model should support at least:

- direct executable + argument vector + working directory;
- launcher executable + arguments;
- provider URI where the provider itself is the intended launch boundary;
- optional expected handoff identities when a launcher legitimately starts the real game process.

Discovery may enrich a target with title, icon, store ID, version and compatibility evidence, but absence of those fields does not invalidate a manually configured executable.

This separation follows a useful pattern visible in Vortex: `requiredFiles` acts as a discovery fingerprint while `executable` is the launch/process target. Lutris likewise supports explicit executables, arguments, working directories and multiple launch configurations. HydraSeat should use the idea, not copy implementation code.

## 5. Custom launchers and unknown installations

A custom launcher is not a special case. It is a LaunchTarget whose root process may hand off to a game process.

The safe ownership order is:

1. HydraSeat creates the root process itself.
2. Job/process-tree evidence owns normal descendants.
3. A handoff outside the tree is accepted only by a bounded, explicit handoff rule using exact observed identity and a short activation epoch.
4. The target window must belong to the accepted process identity.
5. If ownership cannot be proven, activation fails closed.

Never recover ownership by scanning for a familiar process name and adopting the first match.

For installations that are not known to a store/provider, HydraSeat can offer:

- choose executable;
- choose launcher;
- choose installation folder and validate a user-selected executable;
- "use this running window" as a discovery helper: resolve its image path/version, create a candidate LaunchTarget, then ask the user to relaunch it through HydraSeat so the host owns it from process creation.

This last path avoids invasive attachment to an arbitrary already-running application while making unusual launchers discoverable.

HydraSeat does not need to determine whether an installation is "official". It treats unknown provenance as unknown evidence. It must not bypass DRM, anti-cheat, authentication, or deliberate single-instance/security restrictions.

## 6. Dependency direction

Allowed direction, conceptually:

```text
HydraSeat.exe -> hydra_ui -> hydra_launch -> hydra_core
                         -> host IPC contract

hydra_host.exe -> hydra_runtime -> hydra_core
               -> hydra_windows  (implements runtime ports)
               -> hydra_compatibility (optional capabilities)

community/catalog sync -> data validation -> hydra_core schemas
```

Forbidden dependencies include:

- runtime -> UI;
- runtime -> community/network popularity data;
- core -> Win32/UI;
- compatibility -> UI authority;
- discovery provider -> runtime mutation;
- diagnostics/labs -> production success authority.

## 7. Executables

Keep production executables few and obvious:

- `HydraSeat.exe`: user-facing control surface/client;
- `hydra_host.exe`: runtime authority;
- `hydra_watchdog.exe`: independent bounded recovery watcher;
- `hydra_reset.exe`: independent emergency recovery entry point.

Diagnostic/probe/lab executables should be behind a diagnostics/developer build option rather than forming the conceptual production architecture.

## 8. Build-graph rule

Do not make one static library per feature packet or source file. Prefer a small number of responsibility-level targets while retaining separate `.cpp` files internally.

A target split is justified only when at least one is true:

- it crosses a process or ABI boundary;
- it has a materially different dependency/platform boundary;
- it has multiple independent consumers that should not pull the rest of the component;
- it is a true optional capability/plugin boundary;
- it needs independent security/recovery isolation.

Otherwise keep the source files separate but compile them into the same responsibility-level component.

## 9. Migration sequence

Do not rewrite the codebase around this document. Converge incrementally:

1. remove dead presentation/legacy wrappers and hidden fallbacks;
2. keep persisted and runtime identities separate;
3. consolidate build targets that share one reason to change;
4. make `SessionController -> SeatRuntime` ownership explicit without duplicating current proven state machines;
5. separate discovery metadata from LaunchTarget ownership;
6. move game-specific compatibility to the compatibility edge;
7. import third-party capabilities only after license/provenance review and dedicated regression evidence;
8. stop structural refactoring once the dependency graph expresses these boundaries; spend the next effort on physical and commercial-game evidence.

## 10. Evidence required before calling the architecture production-ready

Good structure is not proof of game compatibility. Production claims require physical evidence for:

- two independent physical keyboards/mice with no cross-input bleed;
- controller isolation across relevant APIs;
- independent audio routing;
- two different games;
- lawful same-title/two-instance scenarios where supported;
- custom executable and custom-launcher handoff;
- Seat 1 stop/change/restart without interrupting Seat 2;
- crash/watchdog/emergency reset and verified ordinary-Windows restoration;
- clean-machine install/uninstall;
- protected/anti-cheat titles correctly refusing unsupported activation.

Synthetic/controlled evidence remains labeled synthetic/controlled.

## References

- D. L. Parnas, "On the Criteria To Be Used in Decomposing Systems into Modules," Communications of the ACM 15(12), 1972. DOI: 10.1145/361598.361623.
- "An empirical investigation of the impact of architectural smells on software maintainability," Journal of Systems and Software 225 (2025), 112382. DOI: 10.1016/j.jss.2025.112382. The study reports negative relationships between testability and dense structure, God components and scattered functionality.
- Nexus Mods Vortex game extension / game-description-language documentation: separation of launch executable from discovery fingerprints and store-specific executable branches.
- Lutris installer/configuration documentation: explicit executable/arguments/working-directory and multiple launch configurations.
- ProtoInput (`Ilyaki/ProtoInput`): modular process-local Windows input redirection reference; current upstream repository carries the MIT License.
- Nucleus Co-op (`SplitScreen-Me/splitscreenme-nucleus`): orchestration/handler and real-game compatibility reference; current repository is GPL-3.0 and should be treated as behavior/research input unless the project makes an explicit compatible licensing decision.
