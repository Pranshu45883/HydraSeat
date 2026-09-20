# HydraSeat Roadmap

This roadmap follows the current ownership architecture rather than the old Phase 0–7 prototype sequence. It describes integration order, not a promise that controlled tests equal production readiness.

See [STATUS.md](STATUS.md) for the dated implementation snapshot.

## 1. Merged foundation

The current upstream baseline already establishes:

- [x] v1 limited to Seat 1 and Seat 2;
- [x] stable controller identity separated from runtime XInput slot identity;
- [x] stable controller IDs persisted in the two-Seat configuration;
- [x] UI/controller selection expressed in Seat terms;
- [x] activation-generation-scoped `SeatRuntime` state;
- [x] cross-Seat exact process/window/controller ownership checks;
- [x] Seat-owned transient controller bindings;
- [x] fail-closed runtime ownership rules.

These are foundation contracts. They do not by themselves prove complete multiseat gameplay.

## 2. Controller and process integration sequence

Move validated controller/process work into `main` in dependency order. Each upstream PR should be immediately mergeable on the current base; dependent work may remain on contributor branches until its prerequisite is merged.

1. **Reconnect-safe controller binding**
   - track source generation across disconnect/reconnect;
   - invalidate stale bindings rather than trusting an old runtime slot.

2. **Explicit controller pairing**
   - pair stable physical identity with the current runtime XInput source using explicit user evidence;
   - never infer physical-to-XInput mapping from enumeration order.

3. **Seat-local virtual XInput contract**
   - expose a Seat-private logical XInput namespace;
   - logical slot 0 maps to the Seat source; unrelated slots remain disconnected;
   - stale activation/source generations fail closed.

4. **Controlled process-isolation evidence**
   - run independent child processes against isolated Seat mappings;
   - verify state and vibration do not cross Seat boundaries;
   - preserve no-popup/noninteractive failure behavior for automated Windows tests.

5. **Process-local XInput ABI adapter**
   - expose the required XInput ABI without falling back to system-global controller state;
   - keep injection/interposition strategy separate from ABI correctness.

6. **Launch/process ownership**
   - create the Seat process suspended;
   - retain exact process identity and process handle;
   - publish ownership before resume;
   - make stop/failure rollback Seat-local.

7. **Process-tree ownership and launcher handoff**
   - own normal descendants with Job/process-tree evidence;
   - accept out-of-tree launcher handoff only through a bounded explicit contract;
   - never recover ownership by scanning for a familiar process name.

8. **Host/client authority split**
   - move production mutation authority into `hydra_host.exe`;
   - keep UI as a bounded client rather than a second state machine.

## 3. Windows audio track

Audio proceeds in parallel as a separate Windows subsystem until a safe Seat integration contract is proven.

1. **Endpoint inventory**
   - read-only render endpoint enumeration;
   - stable/opaque endpoint identity and availability state;
   - no global-default mutation.

2. **Session observation**
   - read-only observation across endpoints;
   - preserve endpoint context;
   - use exact process creation identity, not PID alone;
   - report whether an observation is complete/authoritative.

3. **Routing feasibility**
   - experiments remain outside the production path;
   - undocumented mechanisms are not considered supported merely because an API call returns success;
   - prove the target session actually moved to the intended endpoint;
   - prove another process/Seat was not affected;
   - rollback must restore only the exact owned prior state, never clear unrelated application/global audio policy.

4. **Seat integration**
   - only after a routing mechanism has target-scoped apply/verify/rollback evidence;
   - integrate in the direction `SessionController -> SeatRuntime -> audio contract -> Windows backend`.

## 4. Keyboard/mouse and display isolation

After authority and process ownership are reliable:

- [ ] prove two physical keyboard/mouse streams can be attributed without cross-input bleed;
- [ ] introduce only the minimum compatibility/interposition needed by a selected target;
- [ ] verify cursor/focus/clip behavior where required;
- [ ] bind target windows to exact owned processes;
- [ ] place each Seat window/display without disturbing the other Seat;
- [ ] keep display/input rollback targeted and reversible.

Synthetic Raw Input or window tests remain lower-layer evidence until repeated with physical devices and real targets.

## 5. LaunchTarget and compatibility

- [ ] keep Game/catalog identity separate from `LaunchTarget`;
- [ ] support direct executables and custom launchers as first-class targets;
- [ ] make discovery optional UX rather than launch authority;
- [ ] select compatibility capabilities explicitly from validated requirements;
- [ ] keep game-specific compatibility at the runtime edge;
- [ ] fail unsupported/ambiguous isolation closed rather than using global fallback;
- [ ] preserve third-party license/provenance records for adapted behavior or code.

## 6. Recovery and packaging

Before calling the product operationally safe:

- [ ] crash-safe Seat rollback;
- [ ] independent watchdog/emergency reset path;
- [ ] reboot recovery and stale-state cleanup;
- [ ] clean-machine install/uninstall evidence;
- [ ] privilege boundaries documented and minimized;
- [ ] release signing and artifact verification.

## 7. Production acceptance gates

HydraSeat is not production-ready until physical/manual evidence covers at least:

- [ ] two independent physical keyboard/mouse assignments;
- [ ] two controller assignments with reconnect/replug behavior;
- [ ] independent audio on two physical render endpoints;
- [ ] Seat 1 stop/restart/change without Seat 2 interruption;
- [ ] two different real games;
- [ ] lawful same-title/two-instance scenarios where the title supports them;
- [ ] custom executable and custom-launcher handoff;
- [ ] crash/recovery returning Windows to a verified safe state;
- [ ] clean-machine setup and uninstall;
- [ ] unsupported protected/anti-cheat scenarios refusing activation safely.

Controlled/synthetic results remain useful engineering evidence, but they are not promoted to physical or commercial-game evidence.
