#pragma once

#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QStackedWidget>
#include <memory>

#include "hydra/hardware_detector.hpp"
#include "ui/engine_poller.hpp"
#include "ui/host_control_client.hpp"
#include "ui/routing_controller.hpp"

namespace hydra::ui {

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

    std::shared_ptr<HostControlClient> m_hostControl;
    std::shared_ptr<hydra::HardwareDetector> m_hardwareDetector;
    std::unique_ptr<EnginePoller> m_enginePoller;
    std::unique_ptr<RoutingController> m_routingController;

    QListWidget* m_sidebar{nullptr};
    QStackedWidget* m_workspaceStack{nullptr};
    QLabel* m_connectionLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
};

} // namespace hydra::ui
