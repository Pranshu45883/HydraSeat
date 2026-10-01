#pragma once

#include <QGridLayout>
#include <QLabel>
#include <QWidget>
#include <cstdint>

#include "ui/engine_poller.hpp"

namespace hydra::ui {

class DashboardPage : public QWidget {
    Q_OBJECT
public:
    explicit DashboardPage(QWidget* parent = nullptr);

public slots:
    void updateState(const EngineStatePayload& payload);

private:
    QLabel* m_valSeats{nullptr};
    QLabel* m_valActive{nullptr};
    QLabel* m_valDisplays{nullptr};
    QLabel* m_valInputs{nullptr};
    QLabel* m_valAudioEndpoints{nullptr};
    QLabel* m_valAudioSessions{nullptr};

    QLabel* m_seat1State{nullptr};
    QLabel* m_seat1App{nullptr};
    QLabel* m_seat1Audio{nullptr};
    QLabel* m_seat1Controller{nullptr};

    QLabel* m_seat2State{nullptr};
    QLabel* m_seat2App{nullptr};
    QLabel* m_seat2Audio{nullptr};
    QLabel* m_seat2Controller{nullptr};

    void setupMetrics(QGridLayout* layout);
    void setupSeats(QGridLayout* layout);
    void updateSeat(
        const hydra::hostipc::SeatSnapshot* snapshot,
        QLabel* stateLbl,
        QLabel* appLbl,
        QLabel* audioLbl,
        QLabel* ctrlLbl);
};

} // namespace hydra::ui
