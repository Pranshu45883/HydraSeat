#pragma once
#include <QWidget>
#include <memory>
#include <QLabel>
#include "hydra/runtime_authority.hpp"
#include "hydra/authority_bridge.hpp"
#include "ui/engine_poller.hpp"
#include <QComboBox>
#include <QPushButton>

namespace hydra::ui {
class SeatsPage : public QWidget {
    Q_OBJECT
public:
    explicit SeatsPage(
        std::shared_ptr<hydra::runtime::SessionController> sessionController,
        std::shared_ptr<hydra::runtime::AuthorityBridge> bridge,
        QWidget* parent = nullptr);
public slots:
    void updateState(const EngineStatePayload& payload);
private slots:
    void onPairRequested(uint32_t seatId);
    void onActivateRequested(uint32_t seatId);
    void onDeactivateRequested(uint32_t seatId);
private:
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
    std::shared_ptr<hydra::runtime::AuthorityBridge> m_bridge;
    EngineStatePayload m_lastPayload;

    struct SeatWidgets {
        QLabel* authorityLabel{nullptr};
        QPushButton* activateBtn{nullptr};
        QPushButton* deactivateBtn{nullptr};
        QLabel* contentLabel{nullptr};
        QComboBox* physCombo{nullptr};
        QComboBox* srcCombo{nullptr};
        QPushButton* pairBtn{nullptr};
        QLabel* feedbackLabel{nullptr};
        QLabel* bindingStateLabel{nullptr};
    };

    SeatWidgets m_seat1;
    SeatWidgets m_seat2;
    
    QString formatSeatDetails(uint32_t seatId);
    void populateCombos(SeatWidgets& widgets);
    void updateBindingState(uint32_t seatId, SeatWidgets& widgets);
};
} // namespace hydra::ui
