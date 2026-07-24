#pragma once

#include <QWidget>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>

#include "hydra/workspace_manager.hpp"
#include "hydra/hardware_detector.hpp"

namespace hydra {
namespace ui {

class WorkspaceWidget : public QGroupBox {
    Q_OBJECT

public:
    explicit WorkspaceWidget(uint32_t workspaceId, QWidget* parent = nullptr);
    ~WorkspaceWidget() override = default;

    uint32_t workspaceId() const { return m_workspaceId; }

    void updateDeviceLists(const std::vector<DeviceInfo>& displays,
                           const std::vector<DeviceInfo>& keyboards,
                           const std::vector<DeviceInfo>& mice,
                           const std::vector<DeviceInfo>& controllers);

    WorkspaceConfig getCurrentConfig() const;

signals:
    void configChanged(uint32_t workspaceId);
    void removeRequested(uint32_t workspaceId);

private slots:
    void onSelectionChanged();

private:
    uint32_t m_workspaceId{1};

    QComboBox* m_displayCombo{nullptr};
    QComboBox* m_keyboardCombo{nullptr};
    QComboBox* m_mouseCombo{nullptr};
    QComboBox* m_controllerCombo{nullptr};
    QPushButton* m_removeBtn{nullptr};
};

} // namespace ui
} // namespace hydra
