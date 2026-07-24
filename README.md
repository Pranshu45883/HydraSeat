# HydraSeat 🎮

An open-source Windows local gaming multiseat framework.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](https://isocpp.org/)
[![Qt 6](https://img.shields.io/badge/Qt-6.8-41CD52?logo=qt)](https://www.qt.io/)

---

## 🎯 Project Goal

Windows natively merges all physical keyboards and mice into a single input stream. **HydraSeat** breaks this limitation by routing Raw Input events and display outputs to independent local player workspaces on a single PC — with **minimal latency**, **no virtual machines**, and **no Remote Desktop**.

```
                           🎮 HydraSeat Core
                                   │
              ┌────────────────────┴────────────────────┐
              ▼                                         ▼
   🖥️ Display 1 (Laptop Screen)             📱 Display 2 (Virtual/Phone)
   👤 Player 1                               👤 Player 2
   ⌨️ Laptop Keyboard                        ⌨️ USB Keyboard
   🖱️ Touchpad                               🖱️ USB Mouse
```

---

## 🚫 Non-Goals

HydraSeat is focused **strictly on local PC gaming**. It will **NOT** be:
- ❌ School computer lab software
- ❌ Office multiseat enterprise tool
- ❌ Remote desktop software
- ❌ Cloud gaming service
- ❌ Enterprise VM manager

---

## 🏗️ Architecture

- **GUI**: Qt 6 (Modern UI with drag-and-drop workspace assignment)
- **Input Routing**: Win32 Raw Input API, Windows HID API, SetupAPI, XInput / DirectInput
- **Display Routing**: DXGI API, Windows Display Configuration API (`QueryDisplayConfig`), IDD Virtual Display Driver
- **Session & Game Launcher**: Steam, Epic, EA, GOG, and generic executable profile launcher

---

## 🛠️ Build Prerequisites

- **OS**: Windows 10 / Windows 11 (64-bit)
- **Compiler**: Visual Studio 2022 (MSVC with C++20 support)
- **Build System**: CMake 3.20+
- **Framework**: Qt 6.x (Widgets / Core)
- **Windows SDK**: Windows 10/11 SDK (Win32 Raw Input, DXGI, SetupAPI)

---

## 🚀 Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md) for detailed Phase 0 to Phase 7 deliverables.
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for internal component designs.
