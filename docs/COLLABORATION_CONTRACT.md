# HydraSeat Collaboration Contract

Status: required engineering contract for changes that affect production behavior.

The purpose of this document is to keep HydraSeat easy to modify without turning safety, compatibility or recovery into hidden conventions. It is intentionally shorter and stricter than a roadmap.

## 1. Runtime authority

- `hydra_host.exe` MUST be the only runtime authority.
- UI processes MUST be clients. They MUST NOT directly launch production game processes or mutate global Windows state.
- Machine-wide session state MUST have one owner.
- Each Seat's game/process/window/input/controller/audio state MUST have one Seat-local owner.
- A new owner, manager, registry, coordinator or service MUST NOT be introduced unless the existing owner cannot represent the responsibility without violating another boundary.

## 2. Persisted identity vs runtime identity

Persisted configuration MAY contain stable identities such as device interface IDs, endpoint IDs, provider metadata IDs and user-selected executable paths.

Persisted configuration MUST NOT contain:

- HWND values;
- PIDs as durable identity;
- Raw Input device handles;
- Job/process handles;
- COM/interface pointers;
- enumeration-order device indices;
- transient target window or process state.

Runtime handles live only in runtime-owned state and are reconstructed from current evidence.

## 3. Exact ownership

- A process MUST be owned from exact creation/process-tree evidence or an explicitly validated handoff contract.
- Code MUST NOT adopt a process merely because its image name or window title looks familiar.
- A Seat MUST NOT be allowed to steal another Seat's process or window.
- Window placement MUST verify that the window belongs to the accepted process identity.
- A launcher handoff MUST be bounded by the active launch epoch and expected executable identity.

## 4. Fail closed

Production code MUST NOT convert missing evidence into success.

Forbidden examples:

- missing compatibility backend -> global input fallback;
- null production dependency -> silently construct a different default implementation;
- failed mutation verification -> continue as if applied;
- failed rollback verification -> return Idle/Success;
- unknown provider revision -> trust stale cached data;
- controlled/synthetic evidence -> label as physical/game evidence.

When required isolation or recovery cannot be proven, the result is unsupported/failure/recovery-required as appropriate.

## 5. Reversible Windows mutation

Every risky Windows mutation MUST define:

1. what state is captured before mutation;
2. the exact owned resource that may change;
3. how success is verified;
4. how rollback is performed;
5. how restored/safe state is verified.

Rollback SHOULD run in reverse activation order.

Broad cleanup such as process-name killing, arbitrary registry sweeps, global device resets or deleting unrelated user files is forbidden.

## 6. IPC and cross-process contracts

Cross-process messages MUST be:

- explicitly versioned;
- bounded in size/count;
- fixed-width where ABI width matters;
- pointer-free;
- strictly decoded;
- explicit about generation/revision/ownership epochs where stale commands are dangerous.

A client MUST NOT be able to manufacture runtime authority by supplying an unverified PID, HWND, pointer, provider result, compatibility badge or arbitrary command string.

## 7. Launch and discovery

- Game discovery is optional UX.
- Manual executable/custom launcher targets are first-class.
- Store/provider identity MUST NOT be required solely to launch a user-selected local target.
- A discovered Game and the LaunchTarget used to run it MUST remain distinct concepts.
- One Game MAY expose multiple LaunchTargets/configurations.
- Discovery failure MUST NOT be interpreted as launch incompatibility.
- Launch ownership failure MUST NOT be hidden by discovery metadata.

Unknown or custom installations are treated as unknown provenance/evidence, not automatically rejected and not automatically trusted.

HydraSeat MUST NOT add DRM, anti-cheat, authentication or deliberate single-instance/security bypasses.

## 8. Compatibility boundary

Game-specific compatibility belongs at the runtime edge.

- Normal launch orchestration SHOULD be understandable without reading game-specific hook code.
- Required compatibility capabilities MUST be selected explicitly by typed/profiled requirements.
- Compatibility code MUST NOT become UI authority or global runtime state.
- Compatibility profiles SHOULD be data-first and narrowly scoped.
- Arbitrary downloaded scripts/plugins MUST NOT execute in the normal production path without a separate explicit trust model.

## 9. Third-party code and research

Before copying or adapting third-party implementation code, a contributor MUST record:

- upstream repository/project;
- exact source revision or release;
- source file(s) or subsystem studied;
- license and required notices;
- whether code is copied/adapted or only behavior was studied;
- tests proving the integration does not weaken HydraSeat ownership/recovery contracts.

Permissively licensed components such as MIT-licensed ProtoInput may be candidates for selective adaptation with required attribution and notices.

GPL code from projects such as the current SplitScreen-Me Nucleus repository MUST NOT be copied into a differently licensed core by accident. A deliberate compatible project licensing decision is required before such code is incorporated. Until then, use behavior/protocol observations and clean independent implementation only.

The same provenance discipline applies to Vortex/Lutris or any future compatibility source. This is a project-governance safeguard, not legal advice.

## 10. UI contract

The normal UI exists to express user intent, not backend topology.

Normal users should see concepts such as:

- Game/target;
- Player;
- Seat 1 / Seat 2;
- hardware assignment;
- readiness/blocking reason;
- Play / Stop / Reconfigure / recovery.

Implementation terms such as Gate identifiers, provider registry revisions, hook IDs, backend packet names or roadmap milestones belong in diagnostics only.

Custom drawing, design systems, animation or card-style abstraction require a demonstrated product need. Native controls are the default while the product flow is still being validated.

## 11. Component/build rule

A source file does not automatically deserve a library.

A new build target SHOULD exist only for a true process/ABI/platform/security/optional-capability boundary or a component with an independently changing dependency set.

Do not create:

- interfaces with one implementation solely "for flexibility";
- factories that only call one constructor;
- wrappers that only forward a call without enforcing a boundary;
- duplicated validation at adjacent layers;
- compatibility aliases with no supported consumer;
- target names based on roadmap/phase packet numbers.

Prefer several readable source files inside one responsibility-level component.

## 12. Pull request contract

A normal PR SHOULD have one primary reason to change.

Every production-affecting PR MUST state:

- owner/boundary changed;
- invariant preserved or introduced;
- rollback/recovery impact;
- compatibility/evidence impact;
- focused tests run.

A PR that changes runtime authority, persistent schema, IPC, process ownership, input isolation or recovery MUST include dedicated negative/failure-path tests.

Structural PRs SHOULD reduce concepts, owners or dependencies. A refactor that only renames or adds an abstraction without reducing ambiguity does not count as de-slop.

## 13. Evidence language

Use these evidence categories literally:

- unit/pure;
- controlled/synthetic;
- controlled real process/open-source target;
- physical hardware;
- commercial/real game;
- community report.

Passing a lower layer MUST NOT be presented as passing a higher layer.

A compatibility percentage or support claim MUST identify enough environment information to be meaningful: game/target identity, relevant version/build, Windows/architecture, compatibility profile/backend, and scenario.

## 14. Review questions

Before merging, reviewers should be able to answer:

1. Who owns this state?
2. Is the identity stable or runtime-only?
3. What happens when the dependency is missing?
4. What is captured before mutation?
5. How is success verified?
6. How is rollback verified?
7. Can another Seat/user/process be affected accidentally?
8. Is game discovery being confused with launch authority?
9. Is this abstraction hiding a real change decision?
10. Is the evidence label stronger than the evidence actually collected?

If these answers are unclear, the change is not ready for the production path.
