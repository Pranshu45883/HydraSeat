#pragma once
#include <QWidget>
#include <memory>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include "hydra/runtime_authority.hpp"
#include "hydra/authority_bridge.hpp"
#include "ui/engine_poller.hpp"

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
private:
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
    std::shared_ptr<hydra::runtime::AuthorityBridge> m_bridge;
    EngineStatePayload m_lastPayload;

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

    SeatWidgets m_seat1;
    SeatWidgets m_seat2;
    
    void populateCombos(SeatWidgets& widgets);
    void updateBindingState(uint32_t seatId, SeatWidgets& widgets);
};
} // namespace hydra::ui
