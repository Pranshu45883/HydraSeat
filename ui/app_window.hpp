#pragma once

#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QStackedWidget>
#include <QPushButton>
#include <memory>

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
    void navigateToPage(int targetIndex);
    void maybeShowFirstRunWizard(const EngineStatePayload& payload);

    std::shared_ptr<HostControlClient> m_hostControl;
    std::unique_ptr<EnginePoller> m_enginePoller;
    std::unique_ptr<RoutingController> m_routingController;

    QListWidget* m_sidebar{nullptr};
    QStackedWidget* m_workspaceStack{nullptr};
    QLabel* m_connectionLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
    QDialog* m_firstRunWizard{nullptr};
    QLabel* m_firstRunSeat1Status{nullptr};
    QLabel* m_firstRunSeat2Status{nullptr};
    QLabel* m_firstRunGamesStatus{nullptr};
    QLabel* m_firstRunAudioStatus{nullptr};
    QPushButton* m_firstRunSeat1Button{nullptr};
    QPushButton* m_firstRunSeat2Button{nullptr};
    QPushButton* m_firstRunGamesButton{nullptr};
    bool m_firstRunWizardHandled{false};
};

} // namespace hydra::ui
