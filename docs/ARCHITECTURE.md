# HydraSeat Architecture Specification

## System Overview

```
                      +-------------------+
                      |   Qt 6 GUI (App)  |
                      +---------+---------+
                                |
        +-----------------------+-----------------------+
        |                       |                       |
+-------v-------+       +-------v-------+       +-------v-------+
|    Display    |       |   Workspace   |       | Game Profiles |
|    Manager    |       |    Manager    |       |   & Launcher  |
+-------+-------+       +-------+-------+       +-------+-------+
        |                       |                       |
        +-----------------------+-----------------------+
                                |
                      +---------v---------+
                      |   Input Router    |
                      +---------+---------+
                                |
        +-----------------------+-----------------------+
        |                       |                       |
+-------v-------+       +-------v-------+       +-------v-------+
|   Keyboard    |       |     Mouse     |       |  Controller   |
|    Router     |       |    Router     |       |    Router     |
+---------------+       +---------------+       +---------------+
```

---

## Component Breakdown

### 1. Hardware Detector (`HardwareDetector`)
- Uses `EnumDisplayMonitors`, `EnumDisplayDevices`, and DXGI (`IDXGIFactory::EnumAdapters`, `IDXGIAdapter::EnumOutputs`) for displays.
- Uses `GetRawInputDeviceList`, `GetRawInputDeviceInfoW`, and `SetupDiGetClassDevsW` for keyboard/mouse device enumeration (`\Device\HID#...` device interface paths).
- Uses `XInputGetState` and DirectInput/SetupAPI for controllers (Xbox, DualSense, Switch Pro, Generic HID).

### 2. Input Router (`InputRouter`)
- Registers Raw Input devices via `RegisterRawInputDevices()` with `RIDEV_INPUTSINK` or custom window hook procedures (`WNDPROC`).
- Distinguishes device handles (`RAWINPUTHEADER.hDevice`) to differentiate distinct physical keyboards, mice, and touchpads.
- Intercepts and routes events to assigned workspace virtual inputs or hooked target processes.

### 3. Display Manager (`DisplayManager`)
- Manages physical monitors and virtual displays (IDD Virtual Display Driver, SpaceDesk, Sunshine, SuperDisplay, Apollo).
- Configures workspace resolutions, refresh rates, and screen bounds via `ChangeDisplaySettingsExW` and Windows Display Configuration APIs.

### 4. Workspace Manager (`WorkspaceManager`)
- Maintains workspace data structures:
  ```json
  {
    "workspace_id": 1,
    "display_id": "\\\\.\\DISPLAY1",
    "keyboard_device": "\\\\?\\HID#VID_046D...",
    "mouse_device": "\\\\?\\HID#VID_046D...",
    "controller_index": 0
  }
  ```

### 5. Game Launcher (`GameLauncher`)
- Launches target executable with workspace configurations, environment overrides, and window hooks.
