#pragma once

#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/engine_poller.hpp"

namespace hydra::ui {

class HardwarePage : public QWidget {
    Q_OBJECT
public:
    explicit HardwarePage(QWidget* parent = nullptr);

public slots:
    void updateState(const EngineStatePayload& payload);

private:
    QVBoxLayout* m_listLayout{nullptr};

    void addSection(
        const QString& title,
        const std::vector<hydra::DeviceInfo>& devices);
    void addControllerSection(
        const hydra::controller::InventorySnapshot& inventory);
};

} // namespace hydra::ui
