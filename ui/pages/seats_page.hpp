#pragma once
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QWidget>
#include <QScrollArea>
#include <cstdint>
#include <memory>
#include <optional>

#include "hydra/controller_pairing.hpp"
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
    void refreshApplications();

private slots:
    void onApplicationSelectionChanged(std::uint32_t seatId);
    void onLaunchRequested(std::uint32_t seatId);
    void onStopRequested(std::uint32_t seatId);
    void onConfigureRequested(std::uint32_t seatId);
    void onControllerSelectionChanged(std::uint32_t seatId);

private:
    struct SeatWidgets {
        QLabel* stateBadge{nullptr};
        QLabel* feedbackLabel{nullptr};

        QComboBox* appCombo{nullptr};
        QComboBox* displayCombo{nullptr};
        QComboBox* keyboardCombo{nullptr};
        QComboBox* mouseCombo{nullptr};
        QComboBox* ctrlPhysCombo{nullptr};
        QComboBox* ctrlSrcCombo{nullptr};
        QComboBox* audioCombo{nullptr};

        QPushButton* configureBtn{nullptr};
        QPushButton* launchBtn{nullptr};
        QPushButton* stopBtn{nullptr};
        QPushButton* reconfigureBtn{nullptr};

        std::optional<hydra::controller::XInputPairingSnapshot>
            controllerPairingBaseline;
        QString controllerPairingId;
        bool operationInFlight{false};
    };

    std::shared_ptr<HostControlClient> m_hostControl;
    EngineStatePayload m_lastPayload;
    SeatWidgets m_seat1;
    SeatWidgets m_seat2;

    QWidget* buildSeat(std::uint32_t seatId, SeatWidgets& widgets);
    void updateSeatData(
        std::uint32_t seatId,
        SeatWidgets& widgets);
    void populateCombos(std::uint32_t seatId, SeatWidgets& widgets);
    const hydra::hostipc::SeatSnapshot* seatSnapshot(
        std::uint32_t seatId) const noexcept;
};

} // namespace hydra::ui
