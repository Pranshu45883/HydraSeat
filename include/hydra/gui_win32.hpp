#pragma once

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

#include "hydra/hardware_detector.hpp"
#include "hydra/workspace_manager.hpp"
#include "hydra/input_router.hpp"

namespace hydra {
namespace gui {

enum class PartitionOwner {
    Pool = 0,
    Player1 = 1,
    Player2 = 2
};

struct VisualDeviceTile {
    HWND hwndControl{nullptr};
    std::wstring name;
    std::wstring typeIcon; // 🖥️, ⌨️, 🖱️, 🎮
    uintptr_t nativeHandle{0};
    std::wstring devicePath;
    PartitionOwner owner{PartitionOwner::Pool};
    uint64_t flashUntil{0};
};

class Win32App {
public:
    Win32App();
    ~Win32App();

    bool initialize(HINSTANCE hInstance, int nCmdShow);
    int run();

    void triggerDeviceFlash(uintptr_t handle, const std::wstring& devPath);
    void moveTileToNextPartition(VisualDeviceTile* tile);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK DeviceTileProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void setupUI();
    void refreshHardware();
    void layoutDeviceTiles();
    void saveWorkspaceProfile();
    void loadWorkspaceProfile();
    void toggleIsolationMode();
    void launchMultiseat();

    HWND m_hwnd{nullptr};
    HWND m_poolGroup{nullptr};
    HWND m_p1Group{nullptr};
    HWND m_p2Group{nullptr};

    HWND m_saveProfileBtn{nullptr};
    HWND m_loadProfileBtn{nullptr};
    HWND m_refreshBtn{nullptr};
    HWND m_isolationBtn{nullptr};
    HWND m_launchBtn{nullptr};
    HWND m_deviceStatusLabel{nullptr};

    std::vector<std::unique_ptr<VisualDeviceTile>> m_deviceTiles;
    std::unordered_map<uintptr_t, size_t> m_handleToTileIndex;

    HardwareDetector m_hardwareDetector;
    WorkspaceManager m_workspaceManager;
    InputRouter m_inputRouter;

    std::vector<DeviceInfo> m_displays;
    std::vector<DeviceInfo> m_keyboards;
    std::vector<DeviceInfo> m_mice;
    std::vector<DeviceInfo> m_controllers;
};

} // namespace gui
} // namespace hydra
#endif
