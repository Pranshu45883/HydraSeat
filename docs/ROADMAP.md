# HydraSeat Roadmap

Snapshot alignment: **2026-10-01**. This roadmap separates implemented software work from physical/manual production acceptance.

## 1. Canonical backend integration

- [x] v1 limited to Seat 1 and Seat 2.
- [x] stable controller identity separated from runtime XInput identity.
- [x] generation-scoped Seat runtime ownership and stale-token rejection.
- [x] exact process identity and cross-Seat process/window/controller checks.
- [x] reconnect-safe controller inventory and pairing foundations.
- [x] Seat-local virtual/process-local XInput foundations and controlled isolation tooling.
- [x] process/group/window/display ownership foundations.
- [x] crash journal, reset, watchdog, startup/recovery foundations.
- [x] installer/update/privilege/support transaction foundations.
- [x] portable two-player setup model.
- [x] diagnostics and provider/profile/compatibility pipeline foundations.
- [x] release acceptance and validation tooling.
- [x] host/client authority split with host IPC v2.
- [x] host-owned Windows audio mutation path.

These check marks mean the software path is integrated and has automated evidence where applicable. They do not mean physical two-player compatibility is proven.

## 2. Host/control authority

- [x] hydra_host.exe is the canonical mutation authority.
- [x] read-only and Control client roles are separated.
- [x] UiConfiguration and GameProcess leases are independent and may coexist.
- [x] stale lease/generation/process identity fails closed.
- [x] UI lease ownership is bound to named-pipe connection lifetime.
- [x] client disconnect automatically releases its UI leases.
- [x] controller pairing is a host command.
- [x] audio route/reset is a host command.
- [x] UI does not receive SessionController/RuntimeHost pointers.
- [x] UI configuration does not terminate an active game lease.

## 3. UI/UX convergence

- [x] UI polling uses read-only host snapshots.
- [x] UI mutations use one persistent Control connection.
- [x] Seats UI distinguishes locally owned, free, and busy UI leases.
- [x] game-active Seats can enter UI configuration without game deactivation.
- [x] controller pairing uses current persistent physical identity plus XInput slot.
- [x] audio actions use exact process identity through the host.
- [ ] run final physical-device usability pass for device naming and reconnect feedback.
- [ ] refine user-facing failure explanations from real-game/manual observations.
- [ ] validate settings flow on a clean installed build.

## 4. Windows audio acceptance

- [x] endpoint inventory.
- [x] session observation with process identity.
- [x] canonical host route/reset command path.
- [x] fail-closed process/Seat/lease validation.
- [ ] prove independent routing on two physical render endpoints.
- [ ] verify target session movement and rollback across representative applications.
- [ ] verify one Seat audio mutation never changes the unrelated Seat/global default.

## 5. Keyboard, mouse, display, and launch acceptance

- [ ] prove two physical keyboard/mouse streams without cross-input bleed.
- [ ] verify cursor/focus/clip behavior where required.
- [ ] verify exact owned-window placement on a real multi-display setup.
- [ ] verify Seat 1 display/input changes do not disturb Seat 2.
- [ ] validate direct executable launch on representative targets.
- [ ] validate bounded custom-launcher handoff without process-name scanning.
- [ ] preserve explicit fail-closed behavior when required compatibility is unavailable.

## 6. Recovery and packaging evidence

The implementation/tooling exists, but these environment-dependent gates remain manual:

- [ ] crash recovery on a real Windows installation restores verified safe state.
- [ ] reboot recovery clears stale state without touching unrelated state.
- [ ] watchdog/emergency reset is exercised end-to-end on physical hardware.
- [ ] clean-machine install/update/uninstall is exercised.
- [ ] privilege boundaries are reviewed on the installed product.
- [ ] production signing trust and artifact verification are exercised with release credentials.

## 7. Production acceptance gates

HydraSeat is not production-ready until evidence covers at least:

- [ ] two independent physical keyboard/mouse assignments;
- [ ] two controller assignments with reconnect/replug behavior;
- [ ] independent audio on two physical render endpoints;
- [ ] Seat 1 stop/restart/reconfigure without Seat 2 interruption;
- [ ] two different real games;
- [ ] lawful same-title/two-instance scenarios where supported;
- [ ] custom executable and custom-launcher handoff;
- [ ] crash/recovery returning Windows to a verified safe state;
- [ ] clean-machine setup and uninstall;
- [ ] unsupported protected/anti-cheat scenarios refusing activation safely.

Controlled and automated results remain engineering evidence; they are not promoted to physical or commercial-game evidence.
