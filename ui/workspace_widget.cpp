#include "ui/workspace_widget.hpp"

namespace hydra {
namespace ui {

WorkspaceWidget::WorkspaceWidget(uint32_t workspaceId, QWidget* parent)
    : QGroupBox(parent), m_workspaceId(workspaceId) 
{
    setTitle(QString("Workspace #%1 (Player %2)").arg(workspaceId).arg(workspaceId));
    setStyleSheet(
        "QGroupBox { font-weight: bold; border: 2px solid #3B82F6; border-radius: 8px; margin-top: 10px; padding: 12px; background-color: #1E293B; color: #F8FAFC; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; color: #60A5FA; }"
        "QLabel { color: #94A3B8; font-size: 13px; font-weight: normal; }"
        "QComboBox { background-color: #0F172A; border: 1px solid #475569; border-radius: 4px; padding: 4px 8px; color: #F8FAFC; font-size: 13px; }"
        "QComboBox::drop-down { border: none; }"
        "QPushButton { background-color: #EF4444; color: white; border: none; border-radius: 4px; padding: 6px 12px; font-weight: bold; }"
        "QPushButton:hover { background-color: #DC2626; }"
    );

    auto* mainLayout = new QVBoxLayout(this);

    // Display
    auto* displayLayout = new QHBoxLayout();
    displayLayout->addWidget(new QLabel("Display Output:", this));
    m_displayCombo = new QComboBox(this);
    displayLayout->addWidget(m_displayCombo, 1);
    mainLayout->addLayout(displayLayout);

    // Keyboard
    auto* kbdLayout = new QHBoxLayout();
    kbdLayout->addWidget(new QLabel("Keyboard:", this));
    m_keyboardCombo = new QComboBox(this);
    kbdLayout->addWidget(m_keyboardCombo, 1);
    mainLayout->addLayout(kbdLayout);

    // Mouse
    auto* mouseLayout = new QHBoxLayout();
    mouseLayout->addWidget(new QLabel("Mouse / Touchpad:", this));
    m_mouseCombo = new QComboBox(this);
    mouseLayout->addWidget(m_mouseCombo, 1);
    mainLayout->addLayout(mouseLayout);

    // Controller
    auto* ctrlLayout = new QHBoxLayout();
    ctrlLayout->addWidget(new QLabel("Gamepad:", this));
    m_controllerCombo = new QComboBox(this);
    ctrlLayout->addWidget(m_controllerCombo, 1);
    mainLayout->addLayout(ctrlLayout);

    // Bottom controls
    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->addStretch();
    m_removeBtn = new QPushButton("Remove Workspace", this);
    bottomLayout->addWidget(m_removeBtn);
    mainLayout->addLayout(bottomLayout);

    connect(m_displayCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &WorkspaceWidget::onSelectionChanged);
    connect(m_keyboardCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &WorkspaceWidget::onSelectionChanged);
    connect(m_mouseCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &WorkspaceWidget::onSelectionChanged);
    connect(m_controllerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &WorkspaceWidget::onSelectionChanged);
    
    connect(m_removeBtn, &QPushButton::clicked, this, [this]() {
        emit removeRequested(m_workspaceId);
    });
}

void WorkspaceWidget::updateDeviceLists(const std::vector<DeviceInfo>& displays,
                                       const std::vector<DeviceInfo>& keyboards,
                                       const std::vector<DeviceInfo>& mice,
                                       const std::vector<DeviceInfo>& controllers) {
    m_displayCombo->blockSignals(true);
    m_keyboardCombo->blockSignals(true);
    m_mouseCombo->blockSignals(true);
    m_controllerCombo->blockSignals(true);

    m_displayCombo->clear();
    m_keyboardCombo->clear();
    m_mouseCombo->clear();
    m_controllerCombo->clear();

    m_displayCombo->addItem("-- None --", QString());
    for (const auto& d : displays) {
        m_displayCombo->addItem(QString::fromStdWString(d.name), QString::fromStdWString(d.id));
    }

    m_keyboardCombo->addItem("-- None --", QString());
    for (const auto& k : keyboards) {
        m_keyboardCombo->addItem(QString::fromStdWString(k.name), QString::fromStdWString(k.devicePath));
    }

    m_mouseCombo->addItem("-- None --", QString());
    for (const auto& m : mice) {
        m_mouseCombo->addItem(QString::fromStdWString(m.name), QString::fromStdWString(m.devicePath));
    }

    m_controllerCombo->addItem("-- None --", QString());
    for (const auto& c : controllers) {
        m_controllerCombo->addItem(QString::fromStdWString(c.name), static_cast<uint>(c.nativeHandle));
    }

    m_displayCombo->blockSignals(false);
    m_keyboardCombo->blockSignals(false);
    m_mouseCombo->blockSignals(false);
    m_controllerCombo->blockSignals(false);
}

WorkspaceConfig WorkspaceWidget::getCurrentConfig() const {
    WorkspaceConfig config{};
    config.workspaceId = m_workspaceId;
    config.displayDeviceName = m_displayCombo->currentData().toString().toStdWString();
    config.keyboardDevicePath = m_keyboardCombo->currentData().toString().toStdWString();
    config.mouseDevicePath = m_mouseCombo->currentData().toString().toStdWString();
    config.controllerIndex = m_controllerCombo->currentData().toUInt();
    return config;
}

void WorkspaceWidget::onSelectionChanged() {
    emit configChanged(m_workspaceId);
}

} // namespace ui
} // namespace hydra
