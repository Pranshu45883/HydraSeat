# HydraSeat Development Roadmap

---

## Phase 0: Research & Foundation (Current)
- [x] Establish C++20 / Qt 6 project workspace architecture
- [x] Configure `.agents/AGENTS.md` autonomous iteration rules
- [x] Document Windows input/display architectural design
- [ ] Evaluate open-source libraries: Interception, HidHide, ViGEmBus, Virtual Display Driver (IDD)

---

## Phase 1: Hardware Detection
- [ ] Implement `HardwareDetector` module
- [ ] Detect and list physical display monitors & virtual displays
- [ ] Differentiate distinct physical keyboards by HID device path (`RAWINPUTHEADER.hDevice`)
- [ ] Differentiate distinct mice and touchpads
- [ ] Differentiate controllers (XInput, DualSense, HID)
- [ ] Build CLI hardware detection tool for verification

---

## Phase 2: Device Assignment UI & Workspace Matrix
- [ ] Build Qt 6 drag-and-drop workspace assignment UI
- [ ] Implement `WorkspaceManager` state machine
- [ ] Save and load workspace profiles to JSON

---

## Phase 3: Raw Input Router Core
- [ ] Implement Win32 Raw Input sink window & hook loop
- [ ] Intercept individual physical keyboard events independently
- [ ] Intercept individual physical mouse events independently
- [ ] Route input events to target window handles without OS merging

---

## Phase 4: Display Routing & Virtual Display Management
- [ ] Integrate Virtual Display Driver (IDD) / SpaceDesk / Sunshine adapters
- [ ] Auto-create virtual display outputs for secondary devices (e.g. Android phone)

---

## Phase 5: Prototype Game Support (MVP)
- [ ] Launch single local co-op target game with 2 independent displays & input devices
- [ ] Verify low latency, zero input bleeding between workspaces

---

## Phase 6: Game Launcher & Profile Manager
- [ ] Game profiles (Steam, Epic, EA, GOG, Custom Executables)
- [ ] Automatic workspace restoration on game launch

---

## Phase 7: Plugin SDK & Extensions
- [ ] Community plugin SDK for custom controllers, display drivers, and game hooks
