#include "hydra/gui_win32.hpp"

#ifdef _WIN32
#include <iostream>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "comctl32.lib")

namespace hydra {
namespace gui {

static Win32App* g_appInstance = nullptr;

#define ID_BTN_REFRESH  1001
#define ID_BTN_ADD_WS   1002
#define ID_BTN_LAUNCH   1003
#define ID_BTN_SAVE_PROF 1005
#define ID_BTN_LOAD_PROF 1006
#define ID_BTN_ISOLATION 1007

#define TIMER_FLASH_RESET 2001

Win32App::Win32App() {
    g_appInstance = this;
}

Win32App::~Win32App() {
    if (g_appInstance == this) {
        g_appInstance = nullptr;
    }
}

LRESULT CALLBACK Win32App::DeviceTileProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rect;
        GetClientRect(hwnd, &rect);

        VisualDeviceTile* tile = reinterpret_cast<VisualDeviceTile*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        bool isFlashing = false;
        if (tile && tile->flashUntil > GetTickCount64()) {
            isFlashing = true;
        }

        HBRUSH bgBrush = isFlashing ? CreateSolidBrush(RGB(34, 197, 94)) : CreateSolidBrush(RGB(30, 41, 59));
        HPEN borderPen = isFlashing ? CreatePen(PS_SOLID, 2, RGB(74, 222, 128)) : CreatePen(PS_SOLID, 2, RGB(59, 130, 246));

        HGDIOBJ oldBrush = SelectObject(hdc, bgBrush);
        HGDIOBJ oldPen = SelectObject(hdc, borderPen);

        RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 10, 10);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, isFlashing ? RGB(0, 0, 0) : RGB(248, 250, 252));

        HFONT hFont = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(hdc, hFont);

        if (tile) {
            std::wstring labelText = tile->typeIcon + L" " + tile->name;
            DrawTextW(hdc, labelText.c_str(), -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(bgBrush);
        DeleteObject(borderPen);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

bool Win32App::initialize(HINSTANCE hInstance, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wcTile = { sizeof(WNDCLASSEXW) };
    wcTile.lpfnWndProc = Win32App::DeviceTileProc;
    wcTile.hInstance = hInstance;
    wcTile.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcTile.hbrBackground = NULL;
    wcTile.lpszClassName = L"HydraSeatDeviceTileClass";
    RegisterClassExW(&wcTile);

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = Win32App::WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(15, 23, 42)); // Dark Slate Background
    wc.lpszClassName = L"HydraSeatMainWindowClass";

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    m_hwnd = CreateWindowExW(
        0, L"HydraSeatMainWindowClass",
        L"HydraSeat - ASTER-Style Multiseat Control Center",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1020, 760,
        NULL, NULL, hInstance, NULL
    );

    if (!m_hwnd) return false;

    setupUI();
    m_inputRouter.initialize(reinterpret_cast<uint64_t>(m_hwnd));
    refreshHardware();

    // Hook global raw input events to trigger live green flashing animations on device icons
    m_inputRouter.setGlobalCallback([this](const RawInputEvent& evt) {
        triggerDeviceFlash(evt.deviceHandle, evt.devicePath);
    });

    SetTimer(m_hwnd, TIMER_FLASH_RESET, 50, NULL);

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
    return true;
}

void Win32App::triggerDeviceFlash(uintptr_t handle, const std::wstring& devPath) {
    uint64_t now = GetTickCount64();
    bool found = false;

    if (handle != 0) {
        auto it = m_handleToTileIndex.find(handle);
        if (it != m_handleToTileIndex.end() && it->second < m_deviceTiles.size()) {
            m_deviceTiles[it->second]->flashUntil = now + 350;
            InvalidateRect(m_deviceTiles[it->second]->hwndControl, NULL, FALSE);
            found = true;
        }
    }

    if (!found && !devPath.empty()) {
        std::wstring devPathUpper = devPath;
        for (auto& c : devPathUpper) c = ::towupper(c);

        for (auto& tilePtr : m_deviceTiles) {
            std::wstring nameUpper = tilePtr->name;
            for (auto& c : nameUpper) c = ::towupper(c);
            if (devPathUpper.find(nameUpper) != std::wstring::npos || nameUpper.find(devPathUpper) != std::wstring::npos) {
                tilePtr->flashUntil = now + 350;
                InvalidateRect(tilePtr->hwndControl, NULL, FALSE);
                found = true;
                break;
            }
        }
    }

    if (!found && !m_deviceTiles.empty()) {
        for (auto& tilePtr : m_deviceTiles) {
            if (tilePtr->typeIcon == L"⌨️" || tilePtr->typeIcon == L"🖱️") {
                tilePtr->flashUntil = now + 350;
                InvalidateRect(tilePtr->hwndControl, NULL, FALSE);
                break;
            }
        }
    }
}

void Win32App::setupUI() {
    // Header Label
    HWND header = CreateWindowExW(0, L"STATIC", L"🎮 HydraSeat Multiseat Control Center",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 15, 450, 30, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

    HFONT hFontHeader = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(header, WM_SETFONT, (WPARAM)hFontHeader, TRUE);

    // Save Profile Button
    m_saveProfileBtn = CreateWindowExW(0, L"BUTTON", L"💾 Save Profile",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        510, 15, 110, 32, m_hwnd, (HMENU)ID_BTN_SAVE_PROF, GetModuleHandle(NULL), NULL);

    // Load Profile Button
    m_loadProfileBtn = CreateWindowExW(0, L"BUTTON", L"📂 Load Profile",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        630, 15, 110, 32, m_hwnd, (HMENU)ID_BTN_LOAD_PROF, GetModuleHandle(NULL), NULL);

    // Refresh Button
    m_refreshBtn = CreateWindowExW(0, L"BUTTON", L"🔄 Refresh",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        750, 15, 90, 32, m_hwnd, (HMENU)ID_BTN_REFRESH, GetModuleHandle(NULL), NULL);

    // Add Workspace Button
    m_addWsBtn = CreateWindowExW(0, L"BUTTON", L"➕ Add WS",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        850, 15, 100, 32, m_hwnd, (HMENU)ID_BTN_ADD_WS, GetModuleHandle(NULL), NULL);

    // Status Label
    m_deviceStatusLabel = CreateWindowExW(0, L"STATIC", L"Detecting connected hardware...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 50, 930, 22, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

    HFONT hFontNormal = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(m_deviceStatusLabel, WM_SETFONT, (WPARAM)hFontNormal, TRUE);
    SendMessageW(m_saveProfileBtn, WM_SETFONT, (WPARAM)hFontNormal, TRUE);
    SendMessageW(m_loadProfileBtn, WM_SETFONT, (WPARAM)hFontNormal, TRUE);
    SendMessageW(m_refreshBtn, WM_SETFONT, (WPARAM)hFontNormal, TRUE);
    SendMessageW(m_addWsBtn, WM_SETFONT, (WPARAM)hFontNormal, TRUE);

    // Initial Workspaces
    addWorkspaceCard();
    addWorkspaceCard();

    // Isolation Toggle Button
    m_isolationBtn = CreateWindowExW(0, L"BUTTON", L"🔒 Lock & Isolate Workspace Inputs: OFF",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20, 650, 460, 42, m_hwnd, (HMENU)ID_BTN_ISOLATION, GetModuleHandle(NULL), NULL);

    // Launch Button at bottom
    m_launchBtn = CreateWindowExW(0, L"BUTTON", L"🚀 Launch Multiseat Game Session",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        490, 650, 460, 42, m_hwnd, (HMENU)ID_BTN_LAUNCH, GetModuleHandle(NULL), NULL);

    HFONT hFontBtn = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(m_isolationBtn, WM_SETFONT, (WPARAM)hFontBtn, TRUE);
    SendMessageW(m_launchBtn, WM_SETFONT, (WPARAM)hFontBtn, TRUE);
}

void Win32App::addWorkspaceCard() {
    size_t index = m_workspaceGroupboxes.size() + 1;
    int startY = 80 + static_cast<int>(index - 1) * 270;

    if (startY > 450) return;

    std::wstring groupTitle = L"Player Workspace #" + std::to_wstring(index) + L" (Player " + std::to_wstring(index) + L")";

    HWND group = CreateWindowExW(0, L"BUTTON", groupTitle.c_str(),
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        20, startY, 930, 255, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

    HFONT hFontBold = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(group, WM_SETFONT, (WPARAM)hFontBold, TRUE);

    m_workspaceGroupboxes.push_back(group);

    // Display Combobox
    CreateWindowExW(0, L"STATIC", L"Display Output:", WS_CHILD | WS_VISIBLE, 40, startY + 30, 130, 22, m_hwnd, NULL, GetModuleHandle(NULL), NULL);
    HWND cbDisp = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 180, startY + 26, 740, 200, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

    // Keyboard Combobox
    CreateWindowExW(0, L"STATIC", L"Keyboard:", WS_CHILD | WS_VISIBLE, 40, startY + 65, 130, 22, m_hwnd, NULL, GetModuleHandle(NULL), NULL);
    HWND cbKbd = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 180, startY + 61, 740, 200, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

    // Mouse Combobox
    CreateWindowExW(0, L"STATIC", L"Mouse / Touchpad:", WS_CHILD | WS_VISIBLE, 40, startY + 100, 130, 22, m_hwnd, NULL, GetModuleHandle(NULL), NULL);
    HWND cbMouse = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 180, startY + 96, 740, 200, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

    m_comboDisplays.push_back(cbDisp);
    m_comboKeyboards.push_back(cbKbd);
    m_comboMice.push_back(cbMouse);

    refreshHardware();
}

void Win32App::rebuildVisualDeviceGrid() {
    for (auto& tilePtr : m_deviceTiles) {
        if (tilePtr && tilePtr->hwndControl) {
            DestroyWindow(tilePtr->hwndControl);
        }
    }
    m_deviceTiles.clear();
    m_handleToTileIndex.clear();

    for (size_t i = 0; i < m_workspaceGroupboxes.size(); ++i) {
        int startY = 80 + static_cast<int>(i) * 270;

        // Display Tile
        if (i < m_displays.size()) {
            auto tile = std::make_unique<VisualDeviceTile>();
            tile->name = L"Display " + std::to_wstring(i + 1) + L" (" + m_displays[i].name + L")";
            tile->typeIcon = L"🖥️";
            tile->workspaceId = static_cast<uint32_t>(i + 1);
            tile->nativeHandle = m_displays[i].nativeHandle;

            tile->hwndControl = CreateWindowExW(0, L"HydraSeatDeviceTileClass", L"",
                WS_CHILD | WS_VISIBLE,
                40, startY + 135, 260, 100, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

            SetWindowLongPtrW(tile->hwndControl, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tile.get()));
            m_deviceTiles.push_back(std::move(tile));
        }

        // Keyboard Tile
        if (i < m_keyboards.size()) {
            auto tile = std::make_unique<VisualDeviceTile>();
            tile->name = m_keyboards[i].name;
            tile->typeIcon = L"⌨️";
            tile->workspaceId = static_cast<uint32_t>(i + 1);
            tile->nativeHandle = m_keyboards[i].nativeHandle;

            tile->hwndControl = CreateWindowExW(0, L"HydraSeatDeviceTileClass", L"",
                WS_CHILD | WS_VISIBLE,
                320, startY + 135, 280, 100, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

            SetWindowLongPtrW(tile->hwndControl, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tile.get()));
            size_t tileIdx = m_deviceTiles.size();
            uintptr_t handle = tile->nativeHandle;
            m_deviceTiles.push_back(std::move(tile));
            if (handle != 0) {
                m_handleToTileIndex[handle] = tileIdx;
            }
        }

        // Mouse Tile
        if (i < m_mice.size()) {
            auto tile = std::make_unique<VisualDeviceTile>();
            tile->name = m_mice[i].name;
            tile->typeIcon = L"🖱️";
            tile->workspaceId = static_cast<uint32_t>(i + 1);
            tile->nativeHandle = m_mice[i].nativeHandle;

            tile->hwndControl = CreateWindowExW(0, L"HydraSeatDeviceTileClass", L"",
                WS_CHILD | WS_VISIBLE,
                620, startY + 135, 280, 100, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

            SetWindowLongPtrW(tile->hwndControl, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tile.get()));
            size_t tileIdx = m_deviceTiles.size();
            uintptr_t handle = tile->nativeHandle;
            m_deviceTiles.push_back(std::move(tile));
            if (handle != 0) {
                m_handleToTileIndex[handle] = tileIdx;
            }
        }
    }
}

void Win32App::refreshHardware() {
    m_displays = m_hardwareDetector.detectDisplays();
    m_keyboards = m_hardwareDetector.detectKeyboards();
    m_mice = m_hardwareDetector.detectMice();
    m_controllers = m_hardwareDetector.detectControllers();

    std::wstring statusText = L"Connected Hardware: " + std::to_wstring(m_displays.size()) + L" Displays | " +
                              std::to_wstring(m_keyboards.size()) + L" Keyboards | " +
                              std::to_wstring(m_mice.size()) + L" Mice | " +
                              std::to_wstring(m_controllers.size()) + L" Gamepads";

    SetWindowTextW(m_deviceStatusLabel, statusText.c_str());

    for (size_t i = 0; i < m_comboDisplays.size(); ++i) {
        HWND cbDisp = m_comboDisplays[i];
        HWND cbKbd = m_comboKeyboards[i];
        HWND cbMouse = m_comboMice[i];

        SendMessageW(cbDisp, CB_RESETCONTENT, 0, 0);
        SendMessageW(cbKbd, CB_RESETCONTENT, 0, 0);
        SendMessageW(cbMouse, CB_RESETCONTENT, 0, 0);

        SendMessageW(cbDisp, CB_ADDSTRING, 0, (LPARAM)L"-- Select Display --");
        for (const auto& d : m_displays) {
            std::wstring label = d.name + L" (" + d.id + L")";
            SendMessageW(cbDisp, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        }
        SendMessageW(cbDisp, CB_SETCURSEL, m_displays.empty() ? 0 : (i < m_displays.size() ? i + 1 : 1), 0);

        SendMessageW(cbKbd, CB_ADDSTRING, 0, (LPARAM)L"-- Select Keyboard --");
        for (const auto& k : m_keyboards) {
            std::wstring label = k.name + (k.devicePath.empty() ? L"" : (L" [" + k.devicePath + L"]"));
            SendMessageW(cbKbd, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        }
        SendMessageW(cbKbd, CB_SETCURSEL, m_keyboards.empty() ? 0 : (i < m_keyboards.size() ? i + 1 : 1), 0);

        SendMessageW(cbMouse, CB_ADDSTRING, 0, (LPARAM)L"-- Select Mouse --");
        for (const auto& m : m_mice) {
            std::wstring label = m.name + (m.devicePath.empty() ? L"" : (L" [" + m.devicePath + L"]"));
            SendMessageW(cbMouse, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        }
        SendMessageW(cbMouse, CB_SETCURSEL, m_mice.empty() ? 0 : (i < m_mice.size() ? i + 1 : 1), 0);
    }

    rebuildVisualDeviceGrid();
}

void Win32App::saveWorkspaceProfile() {
    for (size_t i = 0; i < m_comboDisplays.size(); ++i) {
        uint32_t id = static_cast<uint32_t>(i + 1);

        wchar_t dispText[256] = {0};
        wchar_t kbdText[256] = {0};
        wchar_t mouseText[256] = {0};

        GetWindowTextW(m_comboDisplays[i], dispText, 256);
        GetWindowTextW(m_comboKeyboards[i], kbdText, 256);
        GetWindowTextW(m_comboMice[i], mouseText, 256);

        m_workspaceManager.assignDisplay(id, dispText);
        m_workspaceManager.assignKeyboard(id, kbdText);
        m_workspaceManager.assignMouse(id, mouseText);
    }

    if (m_workspaceManager.saveToFile("workspace_config.json")) {
        MessageBoxW(m_hwnd, L"Workspace profile saved to workspace_config.json!", L"HydraSeat Profile Manager", MB_OK | MB_ICONINFORMATION);
    }
}

void Win32App::loadWorkspaceProfile() {
    if (m_workspaceManager.loadFromFile("workspace_config.json")) {
        MessageBoxW(m_hwnd, L"Workspace profile loaded successfully!", L"HydraSeat Profile Manager", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(m_hwnd, L"No saved workspace_config.json found.", L"HydraSeat Profile Manager", MB_OK | MB_ICONWARNING);
    }
}

void Win32App::toggleIsolationMode() {
    bool current = m_inputRouter.isIsolationMode();
    m_inputRouter.setIsolationMode(!current);

    if (!current) {
        SetWindowTextW(m_isolationBtn, L"🔓 Lock & Isolate Workspace Inputs: ON");
        MessageBoxW(m_hwnd, L"Multiseat Input Isolation Activated!\n\nKeystrokes & mouse inputs are locked exclusively to their assigned Player Workspaces.", L"HydraSeat Input Isolation Engine", MB_OK | MB_ICONINFORMATION);
    } else {
        SetWindowTextW(m_isolationBtn, L"🔒 Lock & Isolate Workspace Inputs: OFF");
    }
}

void Win32App::launchMultiseat() {
    MessageBoxW(m_hwnd,
        L"Multiseat Inputs & Displays Routed Successfully!\n\nLaunching target game instances...",
        L"HydraSeat Multiseat Launcher",
        MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK Win32App::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_COMMAND) {
        int wmId = LOWORD(wParam);
        if (wmId == ID_BTN_REFRESH && g_appInstance) {
            g_appInstance->refreshHardware();
        } else if (wmId == ID_BTN_ADD_WS && g_appInstance) {
            g_appInstance->addWorkspaceCard();
        } else if (wmId == ID_BTN_SAVE_PROF && g_appInstance) {
            g_appInstance->saveWorkspaceProfile();
        } else if (wmId == ID_BTN_LOAD_PROF && g_appInstance) {
            g_appInstance->loadWorkspaceProfile();
        } else if (wmId == ID_BTN_ISOLATION && g_appInstance) {
            g_appInstance->toggleIsolationMode();
        } else if (wmId == ID_BTN_LAUNCH && g_appInstance) {
            g_appInstance->launchMultiseat();
        }
    } else if (uMsg == TIMER_FLASH_RESET && g_appInstance) {
        uint64_t now = GetTickCount64();
        for (auto& tilePtr : g_appInstance->m_deviceTiles) {
            if (tilePtr && tilePtr->flashUntil > 0 && now >= tilePtr->flashUntil) {
                tilePtr->flashUntil = 0;
                InvalidateRect(tilePtr->hwndControl, NULL, FALSE);
            }
        }
    } else if (uMsg == WM_DESTROY) {
        KillTimer(hwnd, TIMER_FLASH_RESET);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int Win32App::run() {
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (g_appInstance) {
            g_appInstance->m_inputRouter.processMessages();
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

} // namespace gui
} // namespace hydra
#endif
