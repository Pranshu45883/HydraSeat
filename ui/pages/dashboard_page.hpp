#pragma once
#include <QWidget>
#include <memory>
#include "hydra/runtime_authority.hpp"
#include "ui/engine_poller.hpp"
#include <QLabel>
namespace hydra::ui {
class DashboardPage : public QWidget {
    Q_OBJECT
public:
    explicit DashboardPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent = nullptr);
public slots:
    void updateState(const EngineStatePayload& payload);

private:
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
    
    // UI Elements
    QLabel* m_statsLabel{nullptr};
    QLabel* m_seat1Label{nullptr};
    QLabel* m_seat2Label{nullptr};
    
    QString formatSeatInfo(uint32_t seatId);
};
} // namespace hydra::ui
