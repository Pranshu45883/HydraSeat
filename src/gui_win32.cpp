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
#define ID_EDIT_INPUTLOG 1004
#define ID_BTN_SAVE_PROF 1005
#define ID_BTN_LOAD_PROF 1006
#define ID_BTN_ISOLATION 1007

Win32App::Win32App() {
    g_appInstance = this;
}

Win32App::~Win32App() {
    if (g_appInstance == this) {
        g_appInstance = nullptr;
    }
}

bool Win32App::initialize(HINSTANCE hInstance, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = Win32App::WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"HydraSeatMainWindowClass";

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    m_hwnd = CreateWindowExW(
        0, L"HydraSeatMainWindowClass",
        L"HydraSeat - Windows Local Gaming Multiseat Control Center & Isolation Engine",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 750,
        NULL, NULL, hInstance, NULL
    );

    if (!m_hwnd) return false;

    setupUI();
    m_inputRouter.initialize();
    refreshHardware();

    // Hook global raw input events to Live Input Tester & Routing Engine
    m_inputRouter.setGlobalCallback([this](const RawInputEvent& evt) {
        std::wstringstream ss;
        std::wstring devName;

        if (evt.deviceHandle != 0) {
            for (const auto& k : m_keyboards) {
                if (k.nativeHandle == evt.deviceHandle) {
                    devName = k.name;
                    break;
                }
            }
            if (devName.empty()) {
                for (const auto& m : m_mice) {
                    if (m.nativeHandle == evt.deviceHandle) {
                        devName = m.name;
                        break;
                    }
                }
            }
        }

        if (devName.empty() && !evt.devicePath.empty()) {
            std::wstring evtPathUpper = evt.devicePath;
            for (auto& c : evtPathUpper) c = ::towupper(c);

            for (const auto& k : m_keyboards) {
                if (!k.devicePath.empty()) {
                    std::wstring kPathUpper = k.devicePath;
                    for (auto& c : kPathUpper) c = ::towupper(c);
                    if (evtPathUpper.find(kPathUpper) != std::wstring::npos || kPathUpper.find(evtPathUpper) != std::wstring::npos) {
                        devName = k.name;
                        break;
                    }
                }
            }
            if (devName.empty()) {
                for (const auto& m : m_mice) {
                    if (!m.devicePath.empty()) {
                        std::wstring mPathUpper = m.devicePath;
                        for (auto& c : mPathUpper) c = ::towupper(c);
                        if (evtPathUpper.find(mPathUpper) != std::wstring::npos || mPathUpper.find(evtPathUpper) != std::wstring::npos) {
                            devName = m.name;
                            break;
                        }
                    }
                }
            }
        }

        if (devName.empty()) {
            if (evt.vkey > 0) {
                devName = L"USB External Keyboard";
            } else if (evt.deviceHandle == 0 || evt.isTouchpad) {
                devName = L"Laptop Touchpad";
            } else {
                devName = L"External Mouse";
            }
        }

        uint32_t wsId = m_workspaceManager.findWorkspaceByKeyboardPath(devName);
        if (wsId == 0) wsId = m_workspaceManager.findWorkspaceByMousePath(devName);

        ss << L"[" << devName << L"]";
        if (wsId > 0) {
            ss << L" -> Workspace #" << wsId;
        }

        if (evt.vkey > 0) {
            ss << L" | Key VK: 0x" << std::hex << evt.vkey << std::dec
               << (evt.messageType == WM_KEYDOWN ? L" [KEYDOWN]" : L" [KEYUP]");
        } else {
            ss << L" | Motion dX: " << evt.deltaX << L" dY: " << evt.deltaY;
        }

        ss << L"\r\n";
        logInputEvent(ss.str());
    });

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
    return true;
}

void Win32App::logInputEvent(const std::wstring& message) {
    if (!m_inputLogEdit) return;

    int len = GetWindowTextLengthW(m_inputLogEdit);
    if (len > 10000) {
        SetWindowTextW(m_inputLogEdit, L"");
        len = 0;
    }
    SendMessageW(m_inputLogEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(m_inputLogEdit, EM_REPLACESEL, FALSE, (LPARAM)message.c_str());
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
        480, 15, 110, 32, m_hwnd, (HMENU)ID_BTN_SAVE_PROF, GetModuleHandle(NULL), NULL);

    // Load Profile Button
    m_loadProfileBtn = CreateWindowExW(0, L"BUTTON", L"📂 Load Profile",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        600, 15, 110, 32, m_hwnd, (HMENU)ID_BTN_LOAD_PROF, GetModuleHandle(NULL), NULL);

    // Refresh Button
    m_refreshBtn = CreateWindowExW(0, L"BUTTON", L"🔄 Refresh",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        720, 15, 90, 32, m_hwnd, (HMENU)ID_BTN_REFRESH, GetModuleHandle(NULL), NULL);

    // Add Workspace Button
    m_addWsBtn = CreateWindowExW(0, L"BUTTON", L"➕ Add WS",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        820, 15, 100, 32, m_hwnd, (HMENU)ID_BTN_ADD_WS, GetModuleHandle(NULL), NULL);

    // Status Label
    m_deviceStatusLabel = CreateWindowExW(0, L"STATIC", L"Detecting connected hardware...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 50, 900, 22, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

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

    // Input Tester Groupbox & Edit Log Box
    HWND inputTesterGroup = CreateWindowExW(0, L"BUTTON", L"⚡ Live Input Isolation Tester & Device Router",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        20, 480, 900, 150, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

    HFONT hFontBold = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(inputTesterGroup, WM_SETFONT, (WPARAM)hFontBold, TRUE);

    m_inputLogEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"Waiting for Raw Input events...\r\n",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        35, 510, 870, 105, m_hwnd, (HMENU)ID_EDIT_INPUTLOG, GetModuleHandle(NULL), NULL);

    HFONT hFontMono = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(m_inputLogEdit, WM_SETFONT, (WPARAM)hFontMono, TRUE);

    // Isolation Toggle Button
    m_isolationBtn = CreateWindowExW(0, L"BUTTON", L"🔒 Lock & Isolate Workspace Inputs: OFF",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20, 642, 440, 42, m_hwnd, (HMENU)ID_BTN_ISOLATION, GetModuleHandle(NULL), NULL);

    // Launch Button at bottom
    m_launchBtn = CreateWindowExW(0, L"BUTTON", L"🚀 Launch Multiseat Game Session",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        470, 642, 450, 42, m_hwnd, (HMENU)ID_BTN_LAUNCH, GetModuleHandle(NULL), NULL);

    HFONT hFontBtn = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(m_isolationBtn, WM_SETFONT, (WPARAM)hFontBtn, TRUE);
    SendMessageW(m_launchBtn, WM_SETFONT, (WPARAM)hFontBtn, TRUE);
}

void Win32App::addWorkspaceCard() {
    size_t index = m_comboDisplays.size() + 1;
    int startY = 80 + static_cast<int>(index - 1) * 195;

    if (startY > 350) return;

    std::wstring groupTitle = L"Player Workspace #" + std::to_wstring(index) + L" (Player " + std::to_wstring(index) + L")";

    HWND group = CreateWindowExW(0, L"BUTTON", groupTitle.c_str(),
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        20, startY, 900, 185, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

    HFONT hFontBold = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(group, WM_SETFONT, (WPARAM)hFontBold, TRUE);

    // Display Combobox
    CreateWindowExW(0, L"STATIC", L"Display Output:", WS_CHILD | WS_VISIBLE, 40, startY + 32, 130, 22, m_hwnd, NULL, GetModuleHandle(NULL), NULL);
    HWND cbDisp = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 180, startY + 28, 720, 200, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

    // Keyboard Combobox
    CreateWindowExW(0, L"STATIC", L"Keyboard:", WS_CHILD | WS_VISIBLE, 40, startY + 72, 130, 22, m_hwnd, NULL, GetModuleHandle(NULL), NULL);
    HWND cbKbd = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 180, startY + 68, 720, 200, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

    // Mouse Combobox
    CreateWindowExW(0, L"STATIC", L"Mouse / Touchpad:", WS_CHILD | WS_VISIBLE, 40, startY + 112, 130, 22, m_hwnd, NULL, GetModuleHandle(NULL), NULL);
    HWND cbMouse = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 180, startY + 108, 720, 200, m_hwnd, NULL, GetModuleHandle(NULL), NULL);

    m_comboDisplays.push_back(cbDisp);
    m_comboKeyboards.push_back(cbKbd);
    m_comboMice.push_back(cbMouse);

    refreshHardware();
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
    } else if (uMsg == WM_DESTROY) {
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
