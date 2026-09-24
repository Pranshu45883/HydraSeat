#pragma once

#include <QMainWindow>
#include <QStackedWidget>
#include <QListWidget>
#include <QLabel>
#include <memory>

#include "hydra/runtime_authority.hpp"
#include "hydra/hardware_detector.hpp"
#include "ui/engine_poller.hpp"
#include "ui/routing_controller.hpp"

namespace hydra::ui {

class EnginePoller; // Forward declaration

class AppWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit AppWindow(QWidget* parent = nullptr);
    ~AppWindow() override;

private slots:
    void onNavigationChanged(int index);

private:
    void setupUi();
    void setupSidebar();
    void setupStatusbar();
    
    // Core engine domain objects
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
    std::shared_ptr<hydra::HardwareDetector> m_hardwareDetector;
    std::unique_ptr<EnginePoller> m_enginePoller;
    std::unique_ptr<RoutingController> m_routingController;
    
    // UI Elements
    QListWidget* m_sidebar{nullptr};
    QStackedWidget* m_workspaceStack{nullptr};
    QLabel* m_statusLabel{nullptr};
};

} // namespace hydra::ui
