#pragma once
#include <QWidget>
#include <memory>
#include <QLabel>
#include "hydra/runtime_authority.hpp"
#include "ui/engine_poller.hpp"

namespace hydra::ui {
class SeatsPage : public QWidget {
    Q_OBJECT
public:
    explicit SeatsPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent = nullptr);
public slots:
    void updateState(const EngineStatePayload& payload);
private:
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
    
    QLabel* m_seat1Content{nullptr};
    QLabel* m_seat2Content{nullptr};
    
    QString formatSeatDetails(uint32_t seatId);
};
} // namespace hydra::ui
