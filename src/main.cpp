#include "hydra/hardware_detector.hpp"
#include "hydra/input_router.hpp"
#include "hydra/display_manager.hpp"
#include "hydra/workspace_manager.hpp"
#include "hydra/game_launcher.hpp"

#ifdef _WIN32
#include "hydra/gui_win32.hpp"
#endif

#ifdef HYDRA_HAS_QT
#include <QApplication>
#include "ui/main_window.hpp"
#endif

#include <iostream>

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)pCmdLine;

#ifdef HYDRA_HAS_QT
    int argc = 0;
    char** argv = nullptr;
    QApplication app(argc, argv);
    hydra::ui::MainWindow window;
    window.show();
    return app.exec();
#else
    hydra::gui::Win32App guiApp;
    if (guiApp.initialize(hInstance, nCmdShow)) {
        return guiApp.run();
    }
    return 0;
#endif
}
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    return wWinMain(GetModuleHandle(NULL), NULL, GetCommandLineW(), SW_SHOW);
#else
    (void)argc;
    (void)argv;
    std::cout << "HydraSeat GUI requires Windows." << std::endl;
    return 0;
#endif
}
