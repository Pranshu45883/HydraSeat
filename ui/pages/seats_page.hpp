#pragma once

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QWidget>
#include <cstdint>
#include <memory>

#include "ui/engine_poller.hpp"
#include "ui/host_control_client.hpp"

namespace hydra::ui {

class SeatsPage : public QWidget {
    Q_OBJECT
public:
    explicit SeatsPage(
        std::shared_ptr<HostControlClient> hostControl,
        QWidget* parent = nullptr);

public slots:
    void updateState(const EngineStatePayload& payload);

private slots:
    void onPairRequested(std::uint32_t seatId);
    void onActivateRequested(std::uint32_t seatId);

private:
    struct SeatWidgets {
        QLabel* stateBadge{nullptr};
        QPushButton* actionBtn{nullptr};
        QLabel* appVal{nullptr};
        QLabel* winVal{nullptr};
        QLabel* audioVal{nullptr};

        QLabel* ctrlStatus{nullptr};
        QComboBox* physCombo{nullptr};
        QComboBox* srcCombo{nullptr};
        QPushButton* pairBtn{nullptr};
        QLabel* feedbackLabel{nullptr};
    };

    std::shared_ptr<HostControlClient> m_hostControl;
    EngineStatePayload m_lastPayload;
    SeatWidgets m_seat1;
    SeatWidgets m_seat2;

    void populateCombos(SeatWidgets& widgets);
    void updateBindingState(
        std::uint32_t seatId,
        const hydra::hostipc::SeatSnapshot* snapshot,
        SeatWidgets& widgets);
    const hydra::hostipc::SeatSnapshot* seatSnapshot(
        std::uint32_t seatId) const noexcept;
};

} // namespace hydra::ui
