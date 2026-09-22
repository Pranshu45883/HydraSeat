#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

#ifdef _WIN32

std::atomic<int> liveWindows{0};
std::atomic<bool> fightPosition{false};
std::atomic<bool> fightGuard{false};
std::atomic<bool> transitionPending{false};
constexpr UINT_PTR kSafetyTimer = 1;
constexpr UINT_PTR kTransitionDestroyTimer = 2;
constexpr UINT_PTR kTransitionCreateTimer = 3;
constexpr UINT_PTR kSecondaryInputCreateTimer = 4;
constexpr UINT kFightPositionMessage = WM_APP + 42u;
constexpr wchar_t kFixtureClassName[] = L"HydraSeatP4WindowFixture";

enum class TransitionKind : std::uint8_t {
    None = 0,
    RecreateGame = 1,
    SplashToGame = 2,
    InputTarget = 3,
    VisualTarget = 4,
    SplashVisualInput = 5,
};

TransitionKind transitionKind{TransitionKind::None};
HWND transitionControl{nullptr};
HWND transitionWindow{nullptr};

HWND createWindow(const wchar_t* className, const wchar_t* title,
                  DWORD style, DWORD exStyle, int x, int y, int width, int height,
                  HWND owner = nullptr, bool show = true);

void createTransitionReplacement() {
    const wchar_t* title = nullptr;
    int x = 160;
    int y = 120;
    int width = 640;
    int height = 400;
    bool show = true;
    bool createInputLater = false;
    switch (transitionKind) {
        case TransitionKind::RecreateGame:
            title = L"Hydra Game Replacement";
            x = 180;
            y = 140;
            break;
        case TransitionKind::SplashToGame:
            title = L"Hydra Game";
            x = 190;
            y = 150;
            break;
        case TransitionKind::InputTarget:
            title = L"Hydra Input Sink Replacement";
            x = 230;
            y = 180;
            width = 360;
            height = 240;
            show = false;
            break;
        case TransitionKind::VisualTarget:
            title = L"Hydra Visual Replacement";
            x = 180;
            y = 140;
            break;
        case TransitionKind::SplashVisualInput:
            title = L"Hydra Visual";
            x = 190;
            y = 150;
            createInputLater = true;
            break;
        case TransitionKind::None:
            break;
    }
    if (title == nullptr) {
        transitionPending.store(false, std::memory_order_release);
        return;
    }
    transitionWindow = createWindow(kFixtureClassName, title, WS_OVERLAPPEDWINDOW,
                                    0, x, y, width, height, nullptr, show);
    transitionPending.store(false, std::memory_order_release);
    if (transitionWindow == nullptr && liveWindows.load(std::memory_order_acquire) == 0) {
        PostQuitMessage(7);
        return;
    }
    if (createInputLater && transitionControl != nullptr &&
        SetTimer(transitionControl, kSecondaryInputCreateTimer, 500u, nullptr) == 0) {
        PostQuitMessage(8);
    }
}

LRESULT CALLBACK fixtureWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SETTEXT: {
            const LRESULT result = DefWindowProcW(window, message, wParam, lParam);
            NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, window, OBJID_WINDOW, CHILDID_SELF);
            return result;
        }
        case WM_WINDOWPOSCHANGED: {
            const LRESULT result = DefWindowProcW(window, message, wParam, lParam);
            if (fightPosition.load(std::memory_order_acquire) &&
                !fightGuard.load(std::memory_order_acquire)) {
                wchar_t title[64]{};
                const int copied = GetWindowTextW(window, title, 64);
                if (copied > 0 && std::wstring_view(title, static_cast<std::size_t>(copied)) ==
                                      L"Hydra Game") {
                    (void)PostMessageW(window, kFightPositionMessage, 0, 0);
                }
            }
            return result;
        }
        case kFightPositionMessage:
            fightGuard.store(true, std::memory_order_release);
            (void)SetWindowPos(window, nullptr, 160, 120, 640, 400,
                               SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
            fightGuard.store(false, std::memory_order_release);
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (window == transitionControl) return 0;
            if (liveWindows.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
                !transitionPending.load(std::memory_order_acquire)) {
                PostQuitMessage(0);
            }
            return 0;
        case WM_TIMER:
            if (wParam == kSafetyTimer && window != transitionControl) {
                DestroyWindow(window);
                return 0;
            }
            if (window == transitionControl && wParam == kTransitionDestroyTimer) {
                (void)KillTimer(window, kTransitionDestroyTimer);
                transitionPending.store(true, std::memory_order_release);
                const HWND stale = transitionWindow;
                transitionWindow = nullptr;
                if (stale != nullptr && IsWindow(stale) != FALSE) DestroyWindow(stale);
                (void)SetTimer(window, kTransitionCreateTimer, 500u, nullptr);
                return 0;
            }
            if (window == transitionControl && wParam == kTransitionCreateTimer) {
                (void)KillTimer(window, kTransitionCreateTimer);
                createTransitionReplacement();
                return 0;
            }
            if (window == transitionControl && wParam == kSecondaryInputCreateTimer) {
                (void)KillTimer(window, kSecondaryInputCreateTimer);
                if (createWindow(kFixtureClassName, L"Hydra Input Sink", WS_OVERLAPPEDWINDOW,
                                 0, 230, 180, 360, 240, nullptr, false) == nullptr) {
                    PostQuitMessage(9);
                }
                return 0;
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::filesystem::path selfPath() {
    std::wstring buffer(32768u, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

std::wstring quote(std::wstring_view argument) {
    if (argument.empty()) return L"\"\"";
    if (argument.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'\"') {
            result.append(slashes * 2u + 1u, L'\\');
            result.push_back(L'\"');
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(ch);
        }
    }
    result.append(slashes * 2u, L'\\');
    result.push_back(L'\"');
    return result;
}

bool spawnGameChild() {
    const auto executable = selfPath();
    std::wstring command = quote(executable.wstring()) + L" --mode game";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(executable.c_str(), mutableCommand.data(),
                                        nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, executable.parent_path().c_str(),
                                        &startup, &process);
    if (created == FALSE) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

HWND createWindow(const wchar_t* className, const wchar_t* title,
                  DWORD style, DWORD exStyle, int x, int y, int width, int height,
                  HWND owner, bool show) {
    HWND window = CreateWindowExW(exStyle, className, title, style,
                                  x, y, width, height, owner, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr) return nullptr;
    liveWindows.fetch_add(1, std::memory_order_relaxed);
    (void)SetTimer(window, kSafetyTimer, 15000u, nullptr);
    if (show) {
        ShowWindow(window, SW_SHOWNA);
        UpdateWindow(window);
    }
    return window;
}

HWND createTransitionControl() {
    return CreateWindowExW(0, kFixtureClassName, L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
}

bool beginTransition(TransitionKind kind, HWND window) {
    transitionKind = kind;
    transitionWindow = window;
    transitionControl = createTransitionControl();
    if (transitionControl == nullptr) return false;
    return SetTimer(transitionControl, kTransitionDestroyTimer, 1500u, nullptr) != 0;
}

#endif

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    std::string mode = "launcher";
    bool spawnChild = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--mode" && index + 1 < argc) {
            mode = argv[++index];
        } else if (argument == "--spawn-child") {
            spawnChild = true;
        } else if (argument == "--fight-position") {
            fightPosition.store(true, std::memory_order_release);
        } else {
            return 2;
        }
    }

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = fixtureWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kFixtureClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 3;
    }

    HWND first = nullptr;
    HWND second = nullptr;
    HWND third = nullptr;
    bool transitionStarted = true;
    if (mode == "launcher") {
        first = createWindow(kFixtureClassName, L"Hydra Launcher", WS_OVERLAPPEDWINDOW,
                             0, 40, 40, 500, 320);
        second = createWindow(kFixtureClassName, L"Hydra Launcher Extra", WS_OVERLAPPEDWINDOW,
                              0, 100, 100, 360, 240);
        if (spawnChild && !spawnGameChild()) return 4;
    } else if (mode == "game") {
        first = createWindow(kFixtureClassName, L"Hydra Game", WS_OVERLAPPEDWINDOW,
                             0, 160, 120, 640, 400);
        second = createWindow(kFixtureClassName, L"Hydra Game Popup",
                              WS_POPUP | WS_CAPTION | WS_SYSMENU,
                              WS_EX_TOOLWINDOW, 220, 180, 280, 160, first);
    } else if (mode == "unowned") {
        first = createWindow(kFixtureClassName, L"Hydra Unowned", WS_OVERLAPPEDWINDOW,
                             0, 250, 200, 420, 260);
    } else if (mode == "same-title") {
        first = createWindow(kFixtureClassName, L"Hydra Game", WS_OVERLAPPEDWINDOW,
                             0, 260, 210, 420, 260);
    } else if (mode == "recreate") {
        first = createWindow(kFixtureClassName, L"Hydra Game", WS_OVERLAPPEDWINDOW,
                             0, 160, 120, 640, 400);
        transitionStarted = first != nullptr && beginTransition(TransitionKind::RecreateGame, first);
    } else if (mode == "transition") {
        first = createWindow(kFixtureClassName, L"Hydra Splash", WS_OVERLAPPEDWINDOW,
                             0, 80, 80, 480, 300);
        transitionStarted = first != nullptr && beginTransition(TransitionKind::SplashToGame, first);
    } else if (mode == "input-target") {
        first = createWindow(kFixtureClassName, L"Hydra Visual", WS_OVERLAPPEDWINDOW,
                             0, 160, 120, 640, 400);
        second = createWindow(kFixtureClassName, L"Hydra Input Sink", WS_OVERLAPPEDWINDOW,
                              0, 230, 180, 360, 240, nullptr, false);
        transitionStarted = second != nullptr && beginTransition(TransitionKind::InputTarget, second);
    } else if (mode == "input-stable") {
        first = createWindow(kFixtureClassName, L"Hydra Visual", WS_OVERLAPPEDWINDOW,
                             0, 160, 120, 640, 400);
        second = createWindow(kFixtureClassName, L"Hydra Input Sink", WS_OVERLAPPEDWINDOW,
                              0, 230, 180, 360, 240, nullptr, false);
    } else if (mode == "input-visual-recreate") {
        first = createWindow(kFixtureClassName, L"Hydra Visual", WS_OVERLAPPEDWINDOW,
                             0, 160, 120, 640, 400);
        second = createWindow(kFixtureClassName, L"Hydra Input Sink", WS_OVERLAPPEDWINDOW,
                              0, 230, 180, 360, 240, nullptr, false);
        transitionStarted = first != nullptr && second != nullptr &&
                            beginTransition(TransitionKind::VisualTarget, first);
    } else if (mode == "input-foreign") {
        first = createWindow(kFixtureClassName, L"Hydra Input Sink", WS_OVERLAPPEDWINDOW,
                             0, 230, 180, 360, 240, nullptr, false);
    } else if (mode == "input-ambiguous") {
        first = createWindow(kFixtureClassName, L"Hydra Visual", WS_OVERLAPPEDWINDOW,
                             0, 160, 120, 640, 400);
        second = createWindow(kFixtureClassName, L"Hydra Input Sink", WS_OVERLAPPEDWINDOW,
                              0, 230, 180, 360, 240, nullptr, false);
        third = createWindow(kFixtureClassName, L"Hydra Input Sink", WS_OVERLAPPEDWINDOW,
                             0, 230, 180, 360, 240, nullptr, false);
    } else if (mode == "input-progression") {
        first = createWindow(kFixtureClassName, L"Hydra Splash", WS_OVERLAPPEDWINDOW,
                             0, 80, 80, 480, 300);
        transitionStarted = first != nullptr &&
                            beginTransition(TransitionKind::SplashVisualInput, first);
    } else {
        return 5;
    }

    const bool requiresSecond = mode == "launcher" || mode == "game" ||
                                mode == "input-target" || mode == "input-stable" ||
                                mode == "input-visual-recreate" || mode == "input-ambiguous";
    const bool requiresThird = mode == "input-ambiguous";
    if (first == nullptr || (requiresSecond && second == nullptr) ||
        (requiresThird && third == nullptr) || !transitionStarted) {
        return 6;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
#else
    (void)argc;
    (void)argv;
    return 0;
#endif
}
