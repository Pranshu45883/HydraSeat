#include <QScrollArea>
#include "ui/pages/seats_page.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>

namespace hydra::ui {

SeatsPage::SeatsPage(
    std::shared_ptr<hydra::runtime::SessionController> sessionController,
    std::shared_ptr<hydra::runtime::AuthorityBridge> bridge,
    QWidget* parent)
    : QWidget(parent), m_sessionController(std::move(sessionController)), m_bridge(std::move(bridge)) {
        auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");
    auto* container = new QWidget();
    container->setStyleSheet("background-color: transparent;");
    auto* layout = new QVBoxLayout(container);
    
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(scrollArea);
    scrollArea->setWidget(container);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(24);

    auto* title = new QLabel("Seats Management", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5; font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);

    auto* subtitle = new QLabel("Manage runtime authority, applications, and controller bindings per seat", this);
    subtitle->setStyleSheet("font-size: 14px; color: #B5B5B5; font-family: 'Segoe UI', sans-serif; margin-bottom: 8px;");
    layout->addWidget(subtitle);

    auto* seatsLayout = new QHBoxLayout();
    seatsLayout->setSpacing(16);
    
    auto buildSeat = [this](int seatId, SeatWidgets& w) -> QFrame* {
        auto* frame = new QFrame(this);
        frame->setMinimumWidth(400);
        frame->setMaximumWidth(500);
        frame->setStyleSheet("background-color: #151515; border-radius: 8px; border: 1px solid #292929; padding: 20px;");
        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(0,0,0,0);
        fl->setSpacing(12);
        
        auto* headerLayout = new QHBoxLayout();
        auto* seatTitle = new QLabel(QString("Seat %1").arg(seatId), frame);
        seatTitle->setStyleSheet("font-size: 18px; font-weight: bold; color: #F5F5F5; border: none;");
        headerLayout->addWidget(seatTitle);
        
        w.stateBadge = new QLabel("● Authority Idle", frame);
        w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-left: 12px;");
        headerLayout->addWidget(w.stateBadge);
        
        headerLayout->addStretch();
        
        w.actionBtn = new QPushButton("Activate", frame);
        w.actionBtn->setStyleSheet(
            "QPushButton { background-color: #E10600; color: #F5F5F5; border: none; border-radius: 6px; padding: 0 16px; font-weight: bold; height: 34px; }"
            "QPushButton:hover { background-color: #FF1A1A; }"
            "QPushButton:disabled { background-color: #202020; color: #777777; }"
        );
        headerLayout->addWidget(w.actionBtn);
        connect(w.actionBtn, &QPushButton::clicked, [this, seatId]() { onActivateRequested(seatId); });
        
        fl->addLayout(headerLayout);
        
        auto addField = [&](const QString& labelText, QLabel*& val) {
            auto* h = new QHBoxLayout();
            auto* lab = new QLabel(labelText);
            lab->setStyleSheet("font-size: 13px; color: #777777; border: none;");
            lab->setFixedWidth(100);
            val = new QLabel("Not assigned");
            val->setStyleSheet("font-size: 13px; color: #F5F5F5; border: none;");
            h->addWidget(lab);
            h->addWidget(val);
            h->addStretch();
            fl->addLayout(h);
        };
        
        addField("Application", w.appVal);
        addField("Window", w.winVal);
        addField("Audio", w.audioVal);
        
        auto* divider = new QFrame();
        divider->setFrameShape(QFrame::HLine);
        divider->setStyleSheet("border: none; background-color: #292929; max-height: 1px; margin-top: 12px; margin-bottom: 12px;");
        fl->addWidget(divider);
        
        auto* ctrlTitle = new QLabel("CONTROLLER", frame);
        ctrlTitle->setStyleSheet("font-size: 12px; font-weight: bold; color: #777777; border: none; margin-bottom: 4px;");
        fl->addWidget(ctrlTitle);
        
        w.ctrlStatus = new QLabel("Not Assigned", frame);
        w.ctrlStatus->setStyleSheet("font-size: 13px; color: #F5F5F5; border: none; margin-bottom: 8px;");
        fl->addWidget(w.ctrlStatus);
        
        auto* physLabel = new QLabel("Physical Controller", frame);
        physLabel->setStyleSheet("font-size: 12px; color: #B5B5B5; border: none;");
        fl->addWidget(physLabel);
        
        w.physCombo = new QComboBox(frame);
        w.physCombo->setStyleSheet("QComboBox { padding: 4px 8px; background-color: #151515; color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; height: 34px; }"
                                   "QComboBox:focus { border: 1px solid #E10600; }");
        fl->addWidget(w.physCombo);
        
        auto* srcLabel = new QLabel("XInput Runtime Source", frame);
        srcLabel->setStyleSheet("font-size: 12px; color: #B5B5B5; border: none; margin-top: 8px;");
        fl->addWidget(srcLabel);
        
        w.srcCombo = new QComboBox(frame);
        w.srcCombo->setStyleSheet("QComboBox { padding: 4px 8px; background-color: #151515; color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; height: 34px; }"
                                  "QComboBox:focus { border: 1px solid #E10600; }");
        fl->addWidget(w.srcCombo);
        
        w.pairBtn = new QPushButton("Pair Controller", frame);
        w.pairBtn->setStyleSheet(
            "QPushButton { background-color: #202020; color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; height: 34px; font-weight: bold; margin-top: 12px; }"
            "QPushButton:hover { background-color: #2A2A2A; }"
            "QPushButton:disabled { background-color: #151515; color: #777777; border: 1px solid #202020; }"
        );
        fl->addWidget(w.pairBtn);
        connect(w.pairBtn, &QPushButton::clicked, [this, seatId]() { onPairRequested(seatId); });
        
        w.feedbackLabel = new QLabel("", frame);
        w.feedbackLabel->setStyleSheet("font-size: 12px; color: #E10600; border: none;");
        w.feedbackLabel->setVisible(false);
        fl->addWidget(w.feedbackLabel);

        fl->addStretch();
        return frame;
    };

    seatsLayout->addWidget(buildSeat(1, m_seat1));
    seatsLayout->addWidget(buildSeat(2, m_seat2));

        seatsLayout->addStretch();
    layout->addLayout(seatsLayout);
    layout->addStretch();
}

void SeatsPage::updateState(const EngineStatePayload& payload) {
    m_lastPayload = payload;
    
    auto updateSeatData = [&](uint32_t seatId, SeatWidgets& w) {
        auto snapshot = m_sessionController->snapshot(seatId);
        if (!snapshot) {
            w.stateBadge->setText("○ Unavailable");
            w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-left: 12px;");
            w.actionBtn->setDisabled(true);
            return;
        }

        if (snapshot->gameLeaseActive && snapshot->uiLeaseActive) {
            w.stateBadge->setText("● UI + Game Active");
            w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; border: none; margin-left: 12px;");
            w.actionBtn->setText("Deactivate");
            w.actionBtn->setStyleSheet(
                "QPushButton { background-color: #202020; color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; padding: 0 16px; font-weight: bold; height: 34px; }"
                "QPushButton:hover { background-color: #2A2A2A; }"
            );
        } else if (snapshot->gameLeaseActive) {
            w.stateBadge->setText("● Game Active");
            w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; border: none; margin-left: 12px;");
            w.actionBtn->setText("Deactivate");
            w.actionBtn->setStyleSheet(
                "QPushButton { background-color: #202020; color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; padding: 0 16px; font-weight: bold; height: 34px; }"
                "QPushButton:hover { background-color: #2A2A2A; }"
            );
        } else if (snapshot->uiLeaseActive) {
            w.stateBadge->setText("● UI Configuring");
            w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; border: none; margin-left: 12px;");
            w.actionBtn->setText("Deactivate");
            w.actionBtn->setStyleSheet(
                "QPushButton { background-color: #202020; color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; padding: 0 16px; font-weight: bold; height: 34px; }"
                "QPushButton:hover { background-color: #2A2A2A; }"
            );
        } else {
            w.stateBadge->setText("● Authority Idle");
            w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-left: 12px;");
            w.actionBtn->setText("Activate");
            w.actionBtn->setStyleSheet(
                "QPushButton { background-color: #E10600; color: #F5F5F5; border: none; border-radius: 6px; padding: 0 16px; font-weight: bold; height: 34px; }"
                "QPushButton:hover { background-color: #FF1A1A; }"
                "QPushButton:disabled { background-color: #202020; color: #777777; }"
            );
        }
        
        w.actionBtn->setDisabled(false);

        if (snapshot->process) {
            w.appVal->setText(QString("PID %1").arg(snapshot->process->pid));
        } else {
            w.appVal->setText("Not assigned");
        }
        
        if (snapshot->targetHwnd) {
            w.winVal->setText(QString("HWND: 0x%1").arg(snapshot->targetHwnd, 0, 16));
        } else {
            w.winVal->setText("Not assigned");
        }

        if (snapshot->audioEndpoint) {
            w.audioVal->setText(QString::fromStdWString(snapshot->audioEndpoint->endpointId));
        } else {
            w.audioVal->setText("Not assigned");
        }
        
        updateBindingState(seatId, w);
    };

    updateSeatData(1, m_seat1);
    updateSeatData(2, m_seat2);
    
    populateCombos(m_seat1);
    populateCombos(m_seat2);
}

void SeatsPage::populateCombos(SeatWidgets& w) {
    if (w.physCombo->hasFocus() || w.srcCombo->hasFocus()) return;

    QString prevPhys = w.physCombo->currentData().toString();
    QString prevSrc = w.srcCombo->currentData().toString();

    w.physCombo->blockSignals(true);
    w.srcCombo->blockSignals(true);

    w.physCombo->clear();
    w.srcCombo->clear();

    w.physCombo->addItem("-- None --", QString());
    for (const auto& phys : m_lastPayload.controllerInventory.physicalControllers) {
        w.physCombo->addItem(QString::fromStdWString(phys.displayName), QString::fromStdWString(phys.persistentId));
    }
    
    w.srcCombo->addItem("-- None --", QString());
    for (const auto& src : m_lastPayload.controllerInventory.sources) {
        w.srcCombo->addItem(QString::fromStdWString(src.displayName), QString::fromStdString(src.runtimeKey));
    }

    int pIdx = w.physCombo->findData(prevPhys);
    if (pIdx > 0) w.physCombo->setCurrentIndex(pIdx);

    int sIdx = w.srcCombo->findData(prevSrc);
    if (sIdx > 0) w.srcCombo->setCurrentIndex(sIdx);

    w.physCombo->blockSignals(false);
    w.srcCombo->blockSignals(false);
}

void SeatsPage::updateBindingState(uint32_t seatId, SeatWidgets& w) {
    auto snapshot = m_sessionController->snapshot(seatId);
    if (!snapshot || !snapshot->controllerBinding) {
        w.ctrlStatus->setText("Not Assigned");
        w.ctrlStatus->setStyleSheet("font-size: 13px; color: #777777; border: none; margin-bottom: 8px;");
        if (snapshot && snapshot->uiLeaseActive) w.pairBtn->setDisabled(false);
        else w.pairBtn->setDisabled(true);
        return;
    }
    
    w.ctrlStatus->setText("● Bound");
    w.ctrlStatus->setStyleSheet("font-size: 13px; color: #E10600; font-weight: bold; border: none; margin-bottom: 8px;");
    
    if (snapshot->uiLeaseActive) w.pairBtn->setDisabled(false);
    else w.pairBtn->setDisabled(true);
}

void SeatsPage::onActivateRequested(uint32_t seatId) {
    auto snapshot = m_sessionController->snapshot(seatId);
    if (!snapshot) return;
    
    SeatWidgets& w = (seatId == 1) ? m_seat1 : m_seat2;
    w.feedbackLabel->setVisible(false);

    if (snapshot->uiLeaseActive || snapshot->gameLeaseActive) {
        if (!m_bridge->releaseUiLease(seatId)) {
            w.feedbackLabel->setText("Failed to release UI lease");
            w.feedbackLabel->setVisible(true);
        }
    } else {
        if (!m_bridge->requestUiLease(seatId)) {
            w.feedbackLabel->setText("Failed to request UI lease. Another authority might own it.");
            w.feedbackLabel->setVisible(true);
        }
    }
}

void SeatsPage::onPairRequested(uint32_t seatId) {
    SeatWidgets& w = (seatId == 1) ? m_seat1 : m_seat2;
    w.feedbackLabel->setVisible(false);

    QString physId = w.physCombo->currentData().toString();
    QString srcId = w.srcCombo->currentData().toString();

    if (physId.isEmpty() || srcId.isEmpty()) {
        w.feedbackLabel->setText("Select both a physical controller and a runtime source.");
        w.feedbackLabel->setVisible(true);
        return;
    }

    hydra::controller::SeatBinding binding;
    binding.seatId = seatId;
    binding.runtimeKey = srcId.toStdString();
    binding.persistentControllerId = physId.toStdWString();
    
    bool result = m_bridge->pairController(seatId, binding, m_lastPayload.controllerInventory);
    
    if (!result) {
        w.feedbackLabel->setText("Pairing failed (stale binding or unowned seat).");
        w.feedbackLabel->setVisible(true);
    }
}

} // namespace hydra::ui
