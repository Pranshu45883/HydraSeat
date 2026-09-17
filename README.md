# HydraSeat

HydraSeat is an open-source Windows local gaming multiseat project focused on running **two independent local gaming Seats in one interactive Windows session**.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](https://isocpp.org/)
[![Windows](https://img.shields.io/badge/Windows-10%20%2F%2011-0078D4?logo=windows)](https://www.microsoft.com/windows/)

> **Project status:** active prototype / architecture convergence. HydraSeat is not yet production-ready. Controlled tests do not replace physical two-Seat hardware, real-game, clean-machine, reboot, recovery, or signing evidence.

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

`hydra_host.exe` is the intended sole runtime authority. Each `SeatRuntime` owns only its Seat-local mutable state. The current repository is migrating incrementally toward this host/client split; current `main` still builds the existing `HydraSeat` application while the authority contracts are introduced and verified in smaller changes.

Core rules:

- exact ownership instead of process-name/window-title guessing;
- stable hardware identity instead of enumeration order;
- runtime PIDs/HWNDs/handles are never persisted as durable identity;
- missing or ambiguous isolation fails closed;
- risky Windows mutations require targeted capture, verification, rollback, and safe-state verification;
- compatibility work stays at the runtime edge and must not become UI or global runtime authority.

See [Architecture](docs/ARCHITECTURE.md) and the stricter [Collaboration Contract](docs/COLLABORATION_CONTRACT.md).

## Current implementation snapshot

Merged `main` already contains the two-Seat configuration model, stable controller identity/inventory, UI selection by stable controller ID, activation-token-scoped `SessionController` / `SeatRuntime` ownership, exact process/window claims, and Seat-owned controller bindings.

Additional controller virtualization, process-isolation, XInput compatibility, launch/process ownership, and Windows audio work is being validated incrementally before it enters the production path.

The dated breakdown is maintained in [Current Status](docs/STATUS.md). Future sequencing is in [Roadmap](docs/ROADMAP.md).

## Safety and non-goals

HydraSeat is for local PC gaming. It is not intended to become:

- remote desktop or cloud gaming software;
- an enterprise VM or office multiseat manager;
- a DRM, anti-cheat, authentication, protected-process, or deliberate single-instance bypass;
- a system that silently changes global Windows state when Seat-local isolation cannot be proven.

Unsupported or ambiguous scenarios should be reported as unsupported rather than hidden behind a global fallback.

## Build prerequisites

- **OS:** Windows 10 / Windows 11, x64 development target;
- **Compiler:** Visual Studio 2022 with C++20 support;
- **Build system:** CMake 3.20+;
- **Windows SDK:** Win32 Raw Input, SetupAPI, DXGI, HID/XInput and related APIs;
- **Qt 6:** optional in the current build. When Qt Widgets/Core is unavailable, the Win32 UI path is used.

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
