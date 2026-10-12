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
#include <QColor>
#include <QPalette>
#include "ui/app_window.hpp"
#include "ui/ui_settings.hpp"
#endif

#include <iostream>

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)pCmdLine;

#ifdef HYDRA_HAS_QT
    QApplication app(__argc, __argv);
    QApplication::setOrganizationName("HydraSeat");
    QApplication::setApplicationName("HydraSeat");

    // AppWindow owns Pranshu's layout/style sheet, but native dialogs and
    // popup views can escape that widget subtree. Give Qt a matching dark
    // application palette so no platform/default control falls back to black
    // text on a dark surface.
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#0A0A0A"));
    palette.setColor(QPalette::WindowText, QColor("#F5F5F5"));
    palette.setColor(QPalette::Base, QColor("#101010"));
    palette.setColor(QPalette::AlternateBase, QColor("#151515"));
    palette.setColor(QPalette::Text, QColor("#F5F5F5"));
    palette.setColor(QPalette::Button, QColor("#202020"));
    palette.setColor(QPalette::ButtonText, QColor("#F5F5F5"));
    palette.setColor(QPalette::Highlight, QColor("#E10600"));
    palette.setColor(QPalette::HighlightedText, QColor("#F5F5F5"));
    palette.setColor(QPalette::ToolTipBase, QColor("#202020"));
    palette.setColor(QPalette::ToolTipText, QColor("#F5F5F5"));
    palette.setColor(QPalette::PlaceholderText, QColor("#777777"));
    // Explicit disabled colors prevent platform/native styling from falling
    // back to dark/black text on HydraSeat's dark surfaces.
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#777777"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#777777"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#777777"));
    palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, QColor("#666666"));
    palette.setColor(QPalette::Disabled, QPalette::Base, QColor("#101010"));
    palette.setColor(QPalette::Disabled, QPalette::Button, QColor("#151515"));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor("#B5B5B5"));
    palette.setColor(QPalette::Inactive, QPalette::Text, QColor("#F5F5F5"));
    palette.setColor(QPalette::Inactive, QPalette::ButtonText, QColor("#F5F5F5"));
    app.setPalette(palette);

    hydra::ui::AppWindow window;
    if (hydra::ui::UiSettings::load().startMinimized) {
        window.showMinimized();
    } else {
        window.show();
    }
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
