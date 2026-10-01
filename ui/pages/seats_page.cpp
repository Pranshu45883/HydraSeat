#include "ui/pages/seats_page.hpp"
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace hydra::ui {

SeatsPage::SeatsPage(
    std::shared_ptr<HostControlClient> hostControl,
    QWidget* parent)
    : QWidget(parent), m_hostControl(std::move(hostControl)) {

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");
    auto* container = new QWidget();
    container->setStyleSheet("background-color: transparent;");
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(24);

    auto* title = new QLabel("Seats Management", container);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5; font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);

    auto* subtitle = new QLabel("Configure hardware assignments and application states", container);
    subtitle->setStyleSheet("font-size: 14px; color: #B5B5B5; font-family: 'Segoe UI', sans-serif; margin-bottom: 8px;");
    layout->addWidget(subtitle);

    auto* seatsLayout = new QHBoxLayout();
    seatsLayout->setSpacing(16);

    seatsLayout->addWidget(buildSeat(1, m_seat1));
    seatsLayout->addWidget(buildSeat(2, m_seat2));
    seatsLayout->addStretch();

    layout->addLayout(seatsLayout);
    layout->addStretch();
    scrollArea->setWidget(container);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(scrollArea);
}

QWidget* SeatsPage::buildSeat(std::uint32_t seatId, SeatWidgets& w) {
    auto* frame = new QFrame(this);
    frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 20px; border: 1px solid #292929;");
    frame->setMinimumWidth(400);
    frame->setMaximumWidth(500);
    
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(16);

    auto* headerLayout = new QHBoxLayout();
    auto* title = new QLabel(QString("SEAT %1").arg(seatId), frame);
    title->setStyleSheet("font-size: 16px; font-weight: bold; color: #F5F5F5; border: none;");
    w.stateBadge = new QLabel("-? Available", frame);
    w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-left: 12px;");
    headerLayout->addWidget(title);
    headerLayout->addWidget(w.stateBadge);
    headerLayout->addStretch();
    layout->addLayout(headerLayout);

    auto createRow = [&](const QString& labelText, QComboBox*& combo) {
        auto* l = new QLabel(labelText, frame);
        l->setStyleSheet("font-size: 11px; font-weight: bold; color: #777777; margin-bottom: 2px; border: none; text-transform: uppercase;");
        combo = new QComboBox(frame);
        combo->setStyleSheet("QComboBox { background-color: #0A0A0A; color: #F5F5F5; border: 1px solid #333333; border-radius: 4px; padding: 4px 8px; font-size: 13px; } QComboBox::drop-down { border: none; } QComboBox:disabled { color: #777777; background-color: #151515; }");
        layout->addWidget(l);
        layout->addWidget(combo);
    };

    createRow("APPLICATION", w.appCombo);
    createRow("DISPLAY", w.displayCombo);
    createRow("KEYBOARD", w.keyboardCombo);
    createRow("MOUSE", w.mouseCombo);
    
    auto* ctrlLabel = new QLabel("CONTROLLER", frame);
    ctrlLabel->setStyleSheet("font-size: 11px; font-weight: bold; color: #777777; margin-bottom: 2px; border: none; text-transform: uppercase;");
    layout->addWidget(ctrlLabel);
    
    auto* ctrlLayout = new QHBoxLayout();
    w.ctrlPhysCombo = new QComboBox(frame);
    w.ctrlSrcCombo = new QComboBox(frame);
    w.ctrlPhysCombo->setStyleSheet("QComboBox { background-color: #0A0A0A; color: #F5F5F5; border: 1px solid #333333; border-radius: 4px; padding: 4px 8px; font-size: 13px; } QComboBox::drop-down { border: none; } QComboBox:disabled { color: #777777; background-color: #151515; }");
    w.ctrlSrcCombo->setStyleSheet("QComboBox { background-color: #0A0A0A; color: #F5F5F5; border: 1px solid #333333; border-radius: 4px; padding: 4px 8px; font-size: 13px; } QComboBox::drop-down { border: none; } QComboBox:disabled { color: #777777; background-color: #151515; }");
    ctrlLayout->addWidget(w.ctrlPhysCombo);
    ctrlLayout->addWidget(w.ctrlSrcCombo);
    layout->addLayout(ctrlLayout);

    createRow("AUDIO", w.audioCombo);

    auto* btnLayout = new QHBoxLayout();
    
    auto styleBtnPrimary = "QPushButton { background-color: #E10600; color: #F5F5F5; font-size: 14px; font-weight: bold; padding: 8px 16px; border-radius: 4px; border: none; } QPushButton:hover { background-color: #FF0A04; } QPushButton:disabled { background-color: #333333; color: #777777; }";
    auto styleBtnSecondary = "QPushButton { background-color: #292929; color: #F5F5F5; font-size: 14px; font-weight: bold; padding: 8px 16px; border-radius: 4px; border: 1px solid #333333; } QPushButton:hover { background-color: #333333; } QPushButton:disabled { background-color: #151515; color: #777777; border: 1px solid #222222; }";

    w.configureBtn = new QPushButton("Configure Seat", frame);
    w.configureBtn->setStyleSheet(styleBtnSecondary);
    w.launchBtn = new QPushButton("Launch", frame);
    w.launchBtn->setStyleSheet(styleBtnPrimary);
    w.stopBtn = new QPushButton("Stop", frame);
    w.stopBtn->setStyleSheet(styleBtnSecondary);
    w.reconfigureBtn = new QPushButton("Reconfigure", frame);
    w.reconfigureBtn->setStyleSheet(styleBtnSecondary);

    btnLayout->addWidget(w.configureBtn);
    btnLayout->addWidget(w.launchBtn);
    btnLayout->addWidget(w.stopBtn);
    btnLayout->addWidget(w.reconfigureBtn);
    layout->addLayout(btnLayout);

    w.feedbackLabel = new QLabel(frame);
    w.feedbackLabel->setStyleSheet("font-size: 12px; color: #B5B5B5; border: none; margin-top: 8px;");
    w.feedbackLabel->setWordWrap(true);
    w.feedbackLabel->setVisible(false);
    layout->addWidget(w.feedbackLabel);

    connect(w.configureBtn, &QPushButton::clicked, this, [this, seatId]() { onConfigureRequested(seatId); });
    connect(w.launchBtn, &QPushButton::clicked, this, [this, seatId]() { onLaunchRequested(seatId); });
    connect(w.stopBtn, &QPushButton::clicked, this, [this, seatId]() { onStopRequested(seatId); });
    connect(w.reconfigureBtn, &QPushButton::clicked, this, [this, seatId]() { onConfigureRequested(seatId); });

    w.appCombo->addItem("Antigravity IDE");
    w.appCombo->addItem("Minecraft");

    return frame;
}

const hydra::hostipc::SeatSnapshot* SeatsPage::seatSnapshot(
    std::uint32_t seatId) const noexcept {
    if (!m_lastPayload.hostSnapshot) return nullptr;
    if (seatId > 0 && seatId <= m_lastPayload.hostSnapshot->seats.size()) {
        return &m_lastPayload.hostSnapshot->seats[seatId - 1u];
    }
    return nullptr;
}

void SeatsPage::updateState(const EngineStatePayload& payload) {
    m_lastPayload = payload;
    updateSeatData(1, m_seat1);
    updateSeatData(2, m_seat2);
    populateCombos(m_seat1);
    populateCombos(m_seat2);
}

void SeatsPage::updateSeatData(std::uint32_t seatId, SeatWidgets& w) {
    const auto* snapshot = seatSnapshot(seatId);
    if (!snapshot) {
        w.stateBadge->setText("-? Host unavailable");
        w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-left: 12px;");
        w.configureBtn->setDisabled(true);
        w.launchBtn->setDisabled(true);
        w.stopBtn->setDisabled(true);
        w.reconfigureBtn->setDisabled(true);
        return;
    }

    if (snapshot->active && snapshot->processOwned) {
        w.stateBadge->setText("-? Running");
        w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; border: none; margin-left: 12px;");
        
        w.configureBtn->setVisible(false);
        w.launchBtn->setVisible(false);
        w.stopBtn->setVisible(true);
        w.reconfigureBtn->setVisible(true);
        
        w.stopBtn->setDisabled(false);
        w.reconfigureBtn->setDisabled(false);
        
        w.appCombo->setDisabled(true);
    } else {
        w.stateBadge->setText("-? Ready");
        w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-left: 12px;");
        
        w.configureBtn->setVisible(true);
        w.launchBtn->setVisible(true);
        w.stopBtn->setVisible(false);
        w.reconfigureBtn->setVisible(false);
        
        w.configureBtn->setDisabled(false);
        w.launchBtn->setDisabled(false);
        
        w.appCombo->setDisabled(false);
    }
}

void SeatsPage::populateCombos(SeatWidgets& w) {
    if (w.displayCombo->count() == 0) w.displayCombo->addItem("Missing Host Capability");
    if (w.keyboardCombo->count() == 0) w.keyboardCombo->addItem("Missing Host Capability");
    if (w.mouseCombo->count() == 0) w.mouseCombo->addItem("Missing Host Capability");
    if (w.audioCombo->count() == 0) w.audioCombo->addItem("Missing Host Capability");

    if (w.ctrlPhysCombo->hasFocus() || w.ctrlSrcCombo->hasFocus()) return;

    const QString prevPhys = w.ctrlPhysCombo->currentData().toString();
    const QVariant prevSrc = w.ctrlSrcCombo->currentData();

    w.ctrlPhysCombo->blockSignals(true);
    w.ctrlSrcCombo->blockSignals(true);
    w.ctrlPhysCombo->clear();
    w.ctrlSrcCombo->clear();

    w.ctrlPhysCombo->addItem("-- None --", QString());
    for (const auto& phys : m_lastPayload.controllerInventory.physicalControllers) {
        w.ctrlPhysCombo->addItem(QString::fromStdWString(phys.displayName), QString::fromStdWString(phys.persistentId));
    }

    w.ctrlSrcCombo->addItem("-- None --", QVariant());
    for (const auto& src : m_lastPayload.controllerInventory.sources) {
        if (!src.connected || !src.runtimeXInputSlot) continue;
        w.ctrlSrcCombo->addItem(QString("%1 (XInput %2)").arg(QString::fromStdWString(src.displayName)).arg(static_cast<int>(*src.runtimeXInputSlot)), QVariant::fromValue(static_cast<int>(*src.runtimeXInputSlot)));
    }

    const int pIdx = w.ctrlPhysCombo->findData(prevPhys);
    if (pIdx > 0) w.ctrlPhysCombo->setCurrentIndex(pIdx);

    const int sIdx = w.ctrlSrcCombo->findData(prevSrc);
    if (sIdx > 0) w.ctrlSrcCombo->setCurrentIndex(sIdx);

    w.ctrlPhysCombo->blockSignals(false);
    w.ctrlSrcCombo->blockSignals(false);
}

void SeatsPage::onConfigureRequested(std::uint32_t seatId) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    w.feedbackLabel->setVisible(false);

    if (!m_hostControl) {
        w.feedbackLabel->setText("Canonical host control is unavailable.");
        w.feedbackLabel->setVisible(true);
        return;
    }

    const QString physId = w.ctrlPhysCombo->currentData().toString();
    bool slotOk = false;
    const int slot = w.ctrlSrcCombo->currentData().toInt(&slotOk);

    if (physId.isEmpty() || !slotOk || slot < 0 || slot >= 4) {
        w.feedbackLabel->setText("Select both a physical controller and a connected XInput source.");
        w.feedbackLabel->setStyleSheet("font-size: 12px; color: #B5B5B5; border: none;");
        w.feedbackLabel->setVisible(true);
        return;
    }

    const QByteArray latinId = physId.toLatin1();
    std::string error;
    const auto result = m_hostControl->pairController(seatId, latinId.toStdString(), static_cast<std::uint8_t>(slot), &error);
    
    if (!result) {
        w.feedbackLabel->setText(QString("Pairing failed: %1").arg(QString::fromStdString(error)));
        w.feedbackLabel->setStyleSheet("font-size: 12px; color: #E10600; border: none;");
    } else {
        w.feedbackLabel->setText("Controller paired. Other hardware assignments require backend capabilities not currently exposed.");
        w.feedbackLabel->setStyleSheet("font-size: 12px; color: #B5B5B5; border: none;");
    }
    w.feedbackLabel->setVisible(true);
}

void SeatsPage::onLaunchRequested(std::uint32_t seatId) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    w.feedbackLabel->setVisible(false);

    if (!m_hostControl) {
        w.feedbackLabel->setText("Canonical host control is unavailable.");
        w.feedbackLabel->setVisible(true);
        return;
    }
    
    w.stateBadge->setText("-? Starting...");
    w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #B5B5B5; border: none; margin-left: 12px;");

    std::string title = w.appCombo->currentText().toStdString();
    std::string error;
    const auto result = m_hostControl->launchGame(seatId, title, "C:\\Windows\\System32\\notepad.exe", "", "C:\\", &error);
    
    if (!result) {
        w.feedbackLabel->setText(QString("The application could not be launched. %1").arg(QString::fromStdString(error)));
        w.feedbackLabel->setStyleSheet("font-size: 12px; color: #E10600; border: none;");
        w.feedbackLabel->setVisible(true);
        
        w.stateBadge->setText("-? Ready");
        w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-left: 12px;");
    }
}

void SeatsPage::onStopRequested(std::uint32_t seatId) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    w.feedbackLabel->setVisible(false);

    if (!m_hostControl) {
        w.feedbackLabel->setText("Canonical host control is unavailable.");
        w.feedbackLabel->setVisible(true);
        return;
    }

    std::string error;
    const auto result = m_hostControl->stopGame(seatId, &error);
    
    if (!result) {
        w.feedbackLabel->setText(QString("The application could not be stopped. %1").arg(QString::fromStdString(error)));
        w.feedbackLabel->setStyleSheet("font-size: 12px; color: #E10600; border: none;");
        w.feedbackLabel->setVisible(true);
    }
}

} // namespace hydra::ui
