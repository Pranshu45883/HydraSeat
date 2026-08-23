# Phase 0 Research Decisions

This document closes the remaining Phase 0 research task from `docs/ROADMAP.md` by evaluating the input-isolation and virtual-display technologies originally listed by the project.

The goal is not to add dependencies during Phase 0. The goal is to record which technologies fit HydraSeat's documented direction so later phases can proceed without repeating the same investigation.

## Decision Summary

| Technology | Phase 0 decision | Intended HydraSeat role |
| --- | --- | --- |
| Interception | Do not adopt as a default dependency | Experimental fallback only for later input-isolation research |
| HidHide | Candidate, not selected | Phase 3 experiment for hiding selected physical HID devices; keyboard/mouse and multi-process behavior must be proven first |
| ViGEmBus | Do not adopt for new development | None; retired dependency, evaluate a maintained successor only if virtual gamepads become necessary |
| Microsoft IddCx / IDD | Supported reference path, not yet selected | Phase 4 reference for virtual-monitor research alongside the adapters already named in the architecture |

## Interception

Interception provides a low-level keyboard and mouse interception API through a Windows driver. Technically, that can help with per-device suppression and reinjection, which is relevant to the long-term goal of zero input bleeding.

However, it is not a good default HydraSeat dependency:

- The upstream project documents a WDK 7.1 build environment and testing through Windows 10 rather than a current Windows 11 driver toolchain.
- Driver installation requires administrator privileges.
- The public licensing model is dual-licensed: the non-commercial path uses LGPL for the library/API while commercial use requires separate licensing.
- Recent upstream issue reports include Windows 11 installation problems and anti-cheat incompatibility reports.

Decision: keep Interception out of the baseline architecture. It may be evaluated later as an experimental Phase 3 backend if Raw Input plus supported device-hiding mechanisms cannot provide the required isolation.

Sources:
- https://github.com/oblitum/Interception
- https://github.com/oblitum/Interception/issues

## HidHide

HidHide is a Windows HID device filter designed to hide selected gaming input devices from applications while allowing explicitly whitelisted applications to retain access.

This matches a problem HydraSeat is expected to face in Phase 3: observing a physical device with Raw Input does not by itself prevent another game or process from receiving the same device.

Reasons to keep HidHide as a candidate:

- It targets Windows 10/11 x64 and is built around a modern Windows driver toolchain.
- Its device-filter model is directly relevant to suppressing physical-device visibility without requiring HydraSeat to invent a new filter driver during early development.
- The repository exposes an MIT-licensed codebase, although its packaged distribution also contains additional licensing material that must be reviewed before redistribution decisions are made.

Constraints:

- HidHide should be optional, not required for hardware detection or the Phase 1 UI.
- HydraSeat should integrate through documented configuration/control interfaces instead of copying driver internals.
- Any Phase 3 design must test recovery behavior carefully so a bad configuration cannot leave the user's keyboard or mouse unusable.

Decision: retain HidHide as one Phase 3 experiment candidate, but do not select or add it as a Phase 0 dependency. Before adoption, test keyboard and mouse filtering, composite HID devices, Raw Input visibility, multiple target processes, recovery after misconfiguration, and any application classes that bypass its filtering model.

Sources:
- https://github.com/nefarius/HidHide
- https://github.com/nefarius/HidHide/releases

## ViGEmBus

ViGEmBus emulates common game controllers as virtual USB devices and historically provided a convenient target for controller virtualization.

It is not suitable for new HydraSeat work because the upstream repository was retired and archived in November 2023. The maintainer explicitly directs users to the project's end-of-life notice.

HydraSeat does not currently need virtual controller emulation to complete hardware detection or Raw Input routing. Pulling a retired kernel driver into the baseline would increase maintenance and signing risk without helping the current milestone.

Decision: do not adopt ViGEmBus. If Phase 3 or Phase 5 later requires virtual gamepads, evaluate a maintained successor at that time instead of binding the architecture to ViGEmBus now.

Source:
- https://github.com/nefarius/ViGEmBus

## Microsoft IddCx / Indirect Display Driver

Microsoft's Indirect Display Driver model is the supported Windows mechanism for creating displays that are not attached to a traditional GPU output. The IddCx class extension provides a user-mode driver model, and Microsoft maintains an official IddSample demonstrating monitor arrival, mode handling, GPU/DXGI setup, and swap-chain processing.

This fits HydraSeat's Phase 4 goals better than making a third-party virtual-display product part of the core architecture.

Important constraints from Microsoft's sample documentation:

- The sample is intentionally minimal and contains production TODOs; it should be treated as a reference, not copied unchanged into a release driver.
- Frame processing and thread scheduling directly affect system performance and therefore HydraSeat's low-latency goal.
- Driver packaging, signing, installation, and recovery must be designed separately before production distribution.

Decision: keep IddCx/IDD as the first-party Windows reference path for Phase 4, but do not commit HydraSeat to shipping a custom display driver during Phase 0. Phase 4 must compare a custom IddCx driver against the third-party adapters already named in the architecture, including WDK/toolchain, signing, packaging, deployment, recovery, latency, and maintenance costs. Any SpaceDesk, Sunshine, SuperDisplay, or Apollo integration must be limited to local display-output use and must not turn HydraSeat into a Remote Desktop or cloud-gaming product.

Sources:
- https://learn.microsoft.com/windows-hardware/drivers/display/indirect-display-driver-model-overview
- https://learn.microsoft.com/samples/microsoft/windows-driver-samples/indirect-display-driver-sample/

## Core Feasibility Risk

The evaluation does not establish a production-ready end-to-end keyboard/mouse isolation path yet, and Phase 0 should record that explicitly.

- Raw Input can identify which physical device produced an event, but observing an event does not by itself suppress the normal Windows input path to other applications.
- HidHide may help hide selected physical HID devices, but hiding alone does not create independent per-seat keyboard/mouse outputs for target games.
- Interception can suppress and reinject low-level input, but its current maintenance, Windows 11, anti-cheat, and licensing concerns make it unsuitable as HydraSeat's default foundation without a later prototype.
- Therefore zero cross-seat input bleeding remains a Phase 3 feasibility gate, not a capability that Phase 0 claims is already solved.

Phase 3 must prove an end-to-end path on Windows 10/11 with two physical keyboard/mouse pairs, two target windows/processes, one target in the foreground, cursor capture/ClipCursor behavior, clean recovery after failure, and no cross-seat input bleeding. If the evaluated user-mode/filter approaches cannot satisfy that gate, the architecture must be revisited before Phase 5.

## Phase Boundaries

These decisions deliberately do not implement input suppression, virtual gamepads, or a virtual display driver during Phase 0.

Phase 1 should continue with deterministic hardware identity and enumeration. Phase 3 owns the isolation feasibility gate described above. Phase 4 should compare the documented display paths before selecting an implementation.

With the listed technologies evaluated, their adoption status recorded, and the unresolved isolation risk made explicit, the research-and-foundation deliverables defined for Phase 0 are complete.
