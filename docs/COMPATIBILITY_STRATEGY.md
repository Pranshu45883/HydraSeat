# HydraSeat Compatibility Strategy

Status: implementation strategy for growing real-game coverage without turning the runtime into a game-specific script engine.

## 1. Compatibility starts with a launch target, not a store database

HydraSeat does not need to prove that an application came from Steam, Epic, GOG, a standard installer, or any particular storefront before it can be launched.

Automatic game discovery is a user convenience. The production runtime needs a bounded `LaunchTarget` that it can start, identify, own, observe and stop safely.

Keep these concepts separate:

- `DiscoveryCandidate`: optional observation from a store manifest, filesystem fingerprint, shortcut or running window.
- `GameRecord`: user-facing metadata such as title, icon, version and provider references.
- `LaunchTarget`: exact launch configuration used by the host.
- `CompatibilityProfile`: capabilities and narrowly scoped compatibility behavior required by a target/environment.
- `HandoffRule`: optional bounded rule for a launcher that legitimately hands execution to another process.

A discovery failure is not a launch failure. A successful discovery is not proof that the target can be isolated.

## 2. First-class launch target forms

A `LaunchTarget` should support multiple configurations for one Game:

1. **Direct executable**
   - absolute executable path;
   - argument vector;
   - optional working directory;
   - optional expected architecture/version/hash metadata.

2. **Custom launcher**
   - launcher executable and argument vector;
   - working directory;
   - optional expected downstream executable identities;
   - bounded handoff timeout/epoch.

3. **Provider URI**
   - provider identity;
   - provider-owned URI/app identifier;
   - expected launch/handoff evidence;
   - no credentials stored by HydraSeat.

4. **Imported running application**
   - discovery helper only;
   - inspect the selected window owner process read-only;
   - resolve image path, architecture and useful version metadata;
   - save a candidate direct/custom LaunchTarget;
   - relaunch through HydraSeat for production ownership.

The fourth form must not turn HydraSeat into a general arbitrary-process injector. Production isolation starts from a process the host creates or from a validated handoff originating in that activation.

## 3. Discovery sources are replaceable conveniences

HydraSeat may discover candidates from any number of read-only sources without making them runtime dependencies:

- Steam manifests/libraries;
- Epic/GOG/other documented local metadata;
- Windows shortcuts and Start-menu entries;
- executable/file fingerprints inside a user-selected folder;
- a manually selected `.exe`;
- a manually selected launcher;
- a currently visible window selected by the user;
- future community catalog metadata after local validation.

Store/provider adapters return candidate metadata. They do not launch processes, mutate the system, or authorize compatibility by themselves.

A useful precedent is Vortex's separation between a discovery fingerprint (`requiredFiles`) and the executable used for launch/process monitoring (`executable`). Lutris similarly treats executable, arguments, working directory and alternate launch configurations as explicit configuration. HydraSeat should keep the idea and implement its own bounded contract.

## 4. Unknown, non-standard and custom installations

HydraSeat should not encode an "official installation" test into the core runtime.

A user-selected executable may come from:

- a portable build;
- a development build;
- a modded build;
- a custom launcher;
- a relocated installation;
- an unknown storefront;
- an otherwise unrecognized local installation.

The correct state is **unknown provenance / unknown compatibility evidence**, not automatically trusted and not automatically rejected.

The runtime still enforces the same ownership, isolation, compatibility and recovery rules.

HydraSeat must not implement or advertise circumvention of DRM, authentication, anti-cheat, licensing checks, or deliberate security/single-instance restrictions. If a target cannot be launched or isolated without such circumvention, HydraSeat reports it unsupported.

## 5. Custom launcher handoff

Launchers are a process-ownership problem, not a game-detection problem.

The production handoff algorithm should remain narrow:

1. The host creates the configured launcher/root process.
2. Normal descendants are owned through exact Job/process-tree evidence.
3. If the launcher intentionally exits and another process becomes the game, an explicit HandoffRule may nominate acceptable downstream executable identities.
4. Candidate handoff processes must be observed during the current short activation epoch and satisfy exact image/path/version/hash rules selected by the profile.
5. The target window must belong to the accepted process identity.
6. Ambiguous candidates or ownership gaps fail closed.

Never adopt the first process named `game.exe`, the first matching window title, or a process that was already running before the activation epoch merely because it looks plausible.

## 6. "Use this running window" fallback

This is the preferred UX fallback when automatic discovery fails.

Flow:

1. user chooses **Use running window**;
2. HydraSeat enumerates visible top-level windows only for the picker;
3. after the user explicitly selects one, HydraSeat reads the owning process image path and safe metadata;
4. it constructs a LaunchTarget candidate;
5. the user confirms arguments/working directory if necessary;
6. the current process is left untouched;
7. for an actual two-Seat session, HydraSeat starts a new instance from that saved LaunchTarget so runtime ownership begins at creation.

This provides the convenience users expect from game discovery without weakening exact ownership or requiring HydraSeat to know every storefront/launcher.

## 7. Multiple targets per Game

One `GameRecord` should be able to reference several launch configurations, for example:

- `Steam`;
- `Direct EXE`;
- `Modded`;
- `Custom launcher`;
- `DX11` / `DX12` variants;
- another locally configured version.

Compatibility evidence belongs to the relevant target/environment fingerprint, not merely the display title or Steam AppID.

At minimum the compatibility key should be able to distinguish:

- game/target identity;
- executable version/hash bucket when relevant;
- Windows build/architecture;
- HydraSeat version;
- compatibility profile/capabilities;
- single-Seat vs two-Seat/same-title scenario;
- relevant protection/anti-cheat state.

## 8. Compatibility capabilities, not giant handlers

Third-party compatibility knowledge should be translated into a small typed capability vocabulary rather than copied as a giant game handler script.

Candidate capabilities include:

- Raw Input registration/data virtualization;
- polled keyboard state virtualization;
- cursor position/clip/show state virtualization;
- foreground/focus virtualization;
- XInput slot virtualization;
- DirectInput visibility/order policy;
- optional physical-device cloaking;
- writable per-instance file/config materialization;
- narrowly scoped environment/argument preparation;
- exact launcher handoff rules.

A profile says which capabilities a target needs and with what bounded parameters. The runtime activates only those capabilities for the exact owned process/Seat.

## 9. What to reuse from open source

### ProtoInput: primary code-adaptation candidate

ProtoInput is the strongest current candidate for selective process-local input compatibility work because it already decomposes multiple Windows input/focus/cursor APIs into hookable modules and its upstream repository is MIT licensed.

Do not import ProtoInput as a second orchestrator. Prefer capability-by-capability adaptation behind HydraSeat's existing compatibility boundary:

- preserve upstream copyright/license notices for copied or substantial adapted code;
- record exact upstream revision and source files;
- wrap/adapt the capability to HydraSeat's typed Seat/activation context;
- keep HydraSeat ownership, activation ordering and rollback authoritative;
- add controlled and physical evidence for every capability actually enabled in production.

### Nucleus Co-op: compatibility behavior/reference database

Nucleus has much broader real-game operational knowledge and is valuable for learning which combinations of launch preparation and input/window tricks are required by games.

Its current upstream repository is GPL-3.0. Do not casually paste GPL implementation into a differently licensed HydraSeat core. Unless HydraSeat deliberately makes a compatible licensing decision, use Nucleus to identify behaviors and test cases, then implement the required typed capability independently or source that capability from a compatible permissive project.

### Vortex and Lutris: discovery/configuration model references

Use their product ideas for:

- store/folder discovery as optional convenience;
- executable separate from discovery fingerprint;
- multiple launch configurations;
- manual configuration when automatic detection fails.

Do not import their complete plugin/extension frameworks merely to solve executable discovery.

## 10. Third-party absorption gate

Every external compatibility contribution must pass this gate before production use:

1. **Provenance**: repository, revision, file/module and original author/license recorded.
2. **License**: compatible with the way HydraSeat will distribute the adapted code; required notices included.
3. **Capability mapping**: external behavior mapped to one or more typed HydraSeat capabilities; no second global orchestrator.
4. **Ownership**: operation can affect only the exact current Seat/owned process or explicitly declared machine-wide resource.
5. **Activation**: preconditions are explicit; no hidden default/fallback.
6. **Verification**: applied state has an observable postcondition.
7. **Rollback**: reversible mutation has rollback and postcondition, or process-local state is proven to disappear with exact process teardown.
8. **Evidence**: controlled negative tests plus the appropriate physical/real-game evidence before a support claim is made.

If an external project solves a case only through broad process scanning, arbitrary scripts, catch-and-continue, global state, unbounded plugin execution or protection bypass, copy the lesson rather than the implementation.

## 11. Compatibility growth order

### Stage A — universal target entry

Make direct executable/custom launcher/multiple LaunchTargets fully usable. Add the running-window import helper. This removes storefront coverage as a blocker.

### Stage B — launcher handoff

Strengthen exact owned process handoff for launchers that spawn/replace the real target. Build controlled fixtures for descendant, launcher-exits, delayed-child and ambiguous-candidate cases.

### Stage C — capability absorption

Compare HydraSeat's current Gate-C/input implementation with permissively licensed ProtoInput modules. Adapt only capabilities that close measured gaps. Avoid duplicate implementations of the same hook.

### Stage D — compatibility profiles/data

Turn repeated per-game requirements into bounded data profiles referencing capabilities. Keep arbitrary scripts out of the default production path.

### Stage E — field evidence

Build a real-game matrix from cheap/simple targets outward:

- open-source/raw-input reference games;
- common non-protected commercial games;
- custom launchers/modded configurations;
- lawful same-title multi-instance games;
- protected games only where documented behavior can be tested without bypass.

## 12. Evidence is the stopping condition

The backend should not keep growing because another hypothetical compatibility abstraction could be useful.

For each unsupported game, first identify the observed missing capability. Add or adapt code only when the missing capability is concrete and reusable.

A compatibility implementation is complete only when its evidence matches the claim:

- pure/unit tests prove algorithms;
- controlled processes prove exact process/window ownership and API behavior;
- physical hardware proves zero-cross input/controller/audio behavior;
- real games prove game compatibility;
- community reports increase evidence breadth but never replace local authority or required protection review.

Current controlled evidence does not yet justify claiming universal physical two-keyboard/two-mouse or commercial-game compatibility. Closing that evidence gap is more valuable than adding another speculative abstraction layer.
