#pragma once
#include <QWidget>
#include <memory>
#include <QVBoxLayout>
#include <QScrollArea>
#include "hydra/runtime_authority.hpp"
#include "ui/engine_poller.hpp"

namespace hydra::ui {
class ApplicationsPage : public QWidget {
    Q_OBJECT
public:
    explicit ApplicationsPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent = nullptr);
public slots:
    void updateState(const EngineStatePayload& payload);
private:
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
    QVBoxLayout* m_listLayout{nullptr};
    
    // Check if an app is bound to any seat
    QString getAssignedSeat(uint32_t pid) const;
};
} // namespace hydra::ui
