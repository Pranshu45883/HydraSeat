#pragma once
#include <QWidget>
#include <memory>
#include <QLabel>
#include <QGridLayout>
#include "hydra/runtime_authority.hpp"
#include "ui/engine_poller.hpp"

namespace hydra::ui {
class DashboardPage : public QWidget {
    Q_OBJECT
public:
    explicit DashboardPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent = nullptr);
public slots:
    void updateState(const EngineStatePayload& payload);
private:
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
    
    QLabel* m_valSeats;
    QLabel* m_valActive;
    QLabel* m_valDisplays;
    QLabel* m_valInputs;
    QLabel* m_valAudioEndpoints;
    QLabel* m_valAudioSessions;

    QLabel* m_seat1State;
    QLabel* m_seat1App;
    QLabel* m_seat1Audio;
    QLabel* m_seat1Controller;

    QLabel* m_seat2State;
    QLabel* m_seat2App;
    QLabel* m_seat2Audio;
    QLabel* m_seat2Controller;

    void setupMetrics(QGridLayout* layout);
    void setupSeats(QGridLayout* layout);
    void updateSeat(uint32_t seatId, QLabel* stateLbl, QLabel* appLbl, QLabel* audioLbl, QLabel* ctrlLbl);
};
} // namespace hydra::ui
