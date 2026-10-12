# HydraSeat

HydraSeat is a Windows local gaming multiseat project focused on running **two independent local gaming Seats in one interactive Windows session**.

> **Licensing note:** the upstream README has declared MIT since the initial project commit, but the repository currently contains no `LICENSE` file. Release packaging therefore keeps project licensing as an explicit unresolved blocker until the upstream maintainer confirms or restores the intended license text.
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](https://isocpp.org/)
[![Windows](https://img.shields.io/badge/Windows-10%20%2F%2011-0078D4?logo=windows)](https://www.microsoft.com/windows/)

> **Project status:** v1 release-candidate engineering branch, not a GA release. The current code builds and passes the automated suite, and the canonical host IPC has been smoke-tested on real Windows hardware. That does not replace two-Seat physical isolation, representative real-game, clean-machine, reboot/recovery, production-signing, or licensing evidence.

## Product model

A **Seat** is one independent local gaming station. A Seat may own a display, keyboard/mouse, controller, audio endpoint, active game process tree, target window, and other runtime resources.

HydraSeat keeps these concepts separate:

- **Player** — the person/profile using a Seat;
- **Seat** — the local hardware/runtime boundary;
- **Game** — a user-facing/catalog identity;
- **LaunchTarget** — the executable/launcher contract HydraSeat can actually start and own.

The v1 target is exactly two active Seats. Stopping, restarting, or changing Seat 1 must not tear down Seat 2, and vice versa.

## Architecture direction

HydraSeat is converging on one runtime-authority model:

```text
HydraSeat UI / control surface
            |
            v
      bounded host IPC
            |
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

`hydra_host.exe` is the sole runtime authority on the reviewed v1 path. Each `SeatRuntime` owns only its Seat-local mutable state. `HydraSeat.exe` is an IPC client: Pranshu's Qt UI commits stable hardware assignments, performs explicit controller pairing, and launches/stops each Seat through the canonical host instead of constructing process-local runtime authority.

Core rules:

- exact ownership instead of process-name/window-title guessing;
- stable hardware identity instead of enumeration order;
- runtime PIDs/HWNDs/handles are never persisted as durable identity;
- missing or ambiguous isolation fails closed;
- risky Windows mutations require targeted capture, verification, rollback, and safe-state verification;
- compatibility work stays at the runtime edge and must not become UI or global runtime authority.

See [Architecture](docs/ARCHITECTURE.md) and the stricter [Collaboration Contract](docs/COLLABORATION_CONTRACT.md).

## Current implementation snapshot

The current release-candidate branch contains the two-Seat configuration model, stable display/keyboard/mouse/controller identity, connection-scoped UI leases, generation-scoped `SessionController` / `SeatRuntime` ownership, exact process/window claims, host-owned launch/stop, Windows audio routing, and the x64 Gate-C external session/bridge path.

Pranshu's Qt Widgets UI is the canonical HydraSeat UI. The build therefore requires exact Qt 6.8.3 instead of silently replacing that interface with a fallback UI when Qt is absent. Release tooling now stages the provenance-bound Qt runtime/plugins beside `HydraSeat.exe` and packages the Microsoft-signed x64 Visual C++ Redistributable as an installer prerequisite. This packaging path remains unverified until the current hardening pass completes its Windows-native validation.

The dated breakdown is maintained in [Current Status](docs/STATUS.md). Future sequencing is in [Roadmap](docs/ROADMAP.md).

## Safety and non-goals

HydraSeat is for local PC gaming. It is not intended to become:

- remote desktop or cloud gaming software;
- an enterprise VM or office multiseat manager;
- a DRM, anti-cheat, authentication, protected-process, or deliberate single-instance bypass;
- a system that silently changes global Windows state when Seat-local isolation cannot be proven.

Unsupported or ambiguous scenarios should be reported as unsupported rather than hidden behind a global fallback.

## Build prerequisites

- **OS:** Windows 11 x64 is the v1 release target; Windows 10 remains experimental in the release-scope contract;
- **Target process architecture:** x64 only for the reviewed v1 Gate-C path;
- **Compiler:** a C++20-capable Windows toolchain. The current local RC evidence was produced with MinGW-w64; an MSVC release build should still be qualified before GA;
- **Build system:** CMake 3.20+;
- **Windows SDK/APIs:** Win32 Raw Input, SetupAPI, DXGI, HID/XInput and related APIs;
- **Qt 6.8+:** required for the canonical Pranshu UI. Source builds must provide Qt Core/Widgets; release packaging must also deploy the required Qt runtime/plugins for clean-machine startup.

Typical MSVC configuration:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Documentation

- [Current Status](docs/STATUS.md) — merged, validated, research, and pending evidence;
- [Architecture](docs/ARCHITECTURE.md) — current ownership model and subsystem boundaries;
- [Maintainable Architecture](docs/MAINTAINABLE_ARCHITECTURE.md) — long-term responsibility-level component model;
- [Collaboration Contract](docs/COLLABORATION_CONTRACT.md) — engineering invariants and contributor boundaries;
- [Compatibility Strategy](docs/COMPATIBILITY_STRATEGY.md) — game/process compatibility policy;
- [Roadmap](docs/ROADMAP.md) — next integration and acceptance milestones.
