#pragma once

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <memory>

#include "hydra/hardware_detector.hpp"
#include "hydra/workspace_manager.hpp"
#include "hydra/input_router.hpp"

namespace hydra {
namespace gui {

class Win32App {
public:
    Win32App();
    ~Win32App();

    bool initialize(HINSTANCE hInstance, int nCmdShow);
    int run();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void setupUI();
    void refreshHardware();
    void addWorkspaceCard();
    void launchMultiseat();

    HWND m_hwnd{nullptr};
    HWND m_statusBtn{nullptr};
    HWND m_addWsBtn{nullptr};
    HWND m_refreshBtn{nullptr};
    HWND m_launchBtn{nullptr};
    HWND m_deviceStatusLabel{nullptr};

    std::vector<HWND> m_comboDisplays;
    std::vector<HWND> m_comboKeyboards;
    std::vector<HWND> m_comboMice;

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
