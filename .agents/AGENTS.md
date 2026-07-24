# HydraSeat Agent Rules & Workflow Instructions

## Overview
HydraSeat is an open-source Windows gaming multiseat framework written in C++20 and Qt 6. It enables multiple local players to play games on a single Windows PC with independent displays, keyboards, mice, and controllers.

---

## Autonomous Agent Execution Guidelines

### 1. Continuous Iteration & Self-Correction Loop
- When assigned a task, write or update code, run build/verification commands, inspect logs, and diagnose errors autonomously.
- If a build, test, or hardware API call fails:
  1. Inspect the complete error output / stack trace immediately.
  2. Identify the root cause without making superficial patches.
  3. Modify the code to address the underlying issue.
  4. Re-run build and verification commands.
  5. Repeat this cycle until all requirements for the step pass successfully.

### 2. Code Quality & Technical Standards
- **Language Standard**: C++20 (MSVC `/std:c++20`).
- **GUI & Application Framework**: Qt 6 (Widgets / QML, Signals/Slots, `QThread`, `QObject`).
- **Input Routing**: Win32 Raw Input API (`RegisterRawInputDevices`, `GetRawInputData`), Windows HID API (`HidD_*`), SetupAPI (`SetupDiGetClassDevs`), and DirectInput/XInput.
- **Display Management**: DXGI API (`CreateDXGIFactory`, `EnumAdapters`, `EnumOutputs`), Windows Display Configuration API (`QueryDisplayConfig`), and Virtual Display Driver (IDD) integrations.
- **Resource Management**: Strict RAII (`std::unique_ptr`, `std::shared_ptr`, Win32 `HANDLE` wrappers). Never leak Win32 handles or memory.
- **Error Handling**: Use explicit status return types (`std::expected` / `std::optional` or custom `Result<T>`) for Win32 API calls (`GetLastError()`).

### 3. Automatic .gitignore Maintenance
- **Auto-Update .gitignore**: Whenever creating new build output paths, log files, binary dependencies, or temporary files, immediately update [.gitignore](file:///c:/Users/prans/Downloads/HydraSeat/.gitignore) to ensure all intermediate and generated artifacts are strictly ignored.

### 4. Git Push Policy
- **Do NOT execute `git push`**: Only perform local commits (`git commit`) when requested. Do NOT push code to remote GitHub repository (`git push`) unless the user explicitly tells you to do so.

---

## Project Structure & Module Ownership

- `include/hydra/`: Public C++ headers
  - `hardware_detector.hpp`: Display, keyboard, mouse, and controller detection
  - `input_router.hpp`: Raw input hook & event router
  - `display_manager.hpp`: Virtual and physical display management
  - `workspace_manager.hpp`: Workspace allocation matrix
  - `game_launcher.hpp`: Game profile and process launcher
- `src/`: Implementation files
- `ui/`: Qt 6 GUI code (Qt Creator / QML / Widgets)
- `tests/`: Automated unit tests & hardware mock tests
- `docs/`: Technical specifications & architecture reference

---

## Verification Protocols
- **Build Verification**: Run `cmake --build build --config Release` or equivalent MSVC command. Ensure 0 compiler warnings (`/W4` or `/W3`) and 0 errors.
- **Runtime Verification**: Run test executables (`hydra_tests.exe` or CLI detection tools) to confirm correct device enumeration and event interception without crashing.
