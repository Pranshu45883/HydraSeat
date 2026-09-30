#pragma once
#include <QWidget>
#include <memory>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QLabel>
#include "hydra/runtime_authority.hpp"
#include "ui/engine_poller.hpp"

namespace hydra::ui {
class DiagnosticsPage : public QWidget {
    Q_OBJECT
public:
    explicit DiagnosticsPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent = nullptr);
public slots:
    void updateState(const EngineStatePayload& payload);
private:
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
    QVBoxLayout* m_listLayout{nullptr};
    
    QFrame* buildSeatDiagnostics(uint32_t seatId);
    void updateSeatDiagnostics(uint32_t seatId, QFrame* frame);
    
    QList<QFrame*> m_seatFrames;
};
} // namespace hydra::ui
