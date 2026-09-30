#pragma once
#include <QWidget>
#include <memory>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QLabel>
#include "hydra/runtime_authority.hpp"
#include "ui/engine_poller.hpp"

namespace hydra::ui {
class HardwarePage : public QWidget {
    Q_OBJECT
public:
    explicit HardwarePage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent = nullptr);
public slots:
    void updateState(const EngineStatePayload& payload);
private:
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
    QVBoxLayout* m_listLayout{nullptr};
    
    void addSection(const QString& title, const std::vector<hydra::DeviceInfo>& devices);
    void addControllerSection(const hydra::controller::InventorySnapshot& inventory);
};
} // namespace hydra::ui
