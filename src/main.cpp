#include "hydra/hardware_detector.hpp"
#include "hydra/input_router.hpp"
#include "hydra/display_manager.hpp"
#include "hydra/workspace_manager.hpp"
#include "hydra/game_launcher.hpp"

#ifdef HYDRA_HAS_QT
#include <QApplication>
#include "ui/main_window.hpp"
#endif

#include <iostream>

int main(int argc, char* argv[]) {
#ifdef HYDRA_HAS_QT
    QApplication app(argc, argv);
    hydra::ui::MainWindow window;
    window.show();
    return app.exec();
#else
    (void)argc;
    (void)argv;

    std::wcout << L"===============================================\n";
    std::wcout << L"       HydraSeat Core Engine v0.1.0            \n";
    std::wcout << L"   Windows Multiseat Local Gaming Framework    \n";
    std::wcout << L"===============================================\n\n";

    hydra::HardwareDetector detector;
    detector.printReport();

    hydra::DisplayManager displayMgr;
    auto displays = displayMgr.enumerateDisplays();
    std::wcout << L"\n[DisplayManager] Total Desktop Displays: " << displays.size() << L"\n";

    hydra::WorkspaceManager workspaceMgr;
    uint32_t p1Workspace = workspaceMgr.createWorkspace(L"Player 1 - Laptop");
    uint32_t p2Workspace = workspaceMgr.createWorkspace(L"Player 2 - Second Screen");

    std::wcout << L"\n[WorkspaceManager] Initialized Workspaces:\n";
    std::wcout << L"  - Workspace #" << p1Workspace << L": Player 1\n";
    std::wcout << L"  - Workspace #" << p2Workspace << L": Player 2\n";

    hydra::InputRouter router;
    if (router.initialize()) {
        std::wcout << L"\n[InputRouter] Win32 Raw Input Sink initialized successfully.\n";
    }

    std::wcout << L"\n[HydraSeat] CLI Mode active.\n";
    return 0;
#endif
}
