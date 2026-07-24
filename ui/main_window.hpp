#pragma once

#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QStatusBar>
#include <QLabel>
#include <QMap>

#include "hydra/hardware_detector.hpp"
#include "hydra/workspace_manager.hpp"
#include "hydra/input_router.hpp"
#include "hydra/game_launcher.hpp"
#include "ui/workspace_widget.hpp"

namespace hydra {
namespace ui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

private slots:
    void onAddWorkspaceClicked();
    void onRemoveWorkspaceRequested(uint32_t workspaceId);
    void onRefreshHardwareClicked();
    void onLaunchGameClicked();

private:
    void setupUi();
    void refreshHardware();

    HardwareDetector m_hardwareDetector;
    WorkspaceManager m_workspaceManager;
    InputRouter m_inputRouter;
    GameLauncher m_gameLauncher;

    QVBoxLayout* m_workspaceContainerLayout{nullptr};
    QMap<uint32_t, WorkspaceWidget*> m_workspaceWidgets;
    QLabel* m_statusLabel{nullptr};

    std::vector<DeviceInfo> m_cachedDisplays;
    std::vector<DeviceInfo> m_cachedKeyboards;
    std::vector<DeviceInfo> m_cachedMice;
    std::vector<DeviceInfo> m_cachedControllers;
};

} // namespace ui
} // namespace hydra
