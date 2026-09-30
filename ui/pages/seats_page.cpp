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
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(24);

    auto* title = new QLabel("Seats Overview", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5;");
    layout->addWidget(title);

    auto* seatsLayout = new QHBoxLayout();
    
    auto buildSeat = [this](int seatId, SeatWidgets& w) -> QFrame* {
        auto* frame = new QFrame(this);
        frame->setStyleSheet("background-color: #151515; border-radius: 8px; padding: 16px; border: 1px solid #2A2A2A;");
        auto* fl = new QVBoxLayout(frame);
        
        auto* headerLayout = new QHBoxLayout();
        auto* seatTitle = new QLabel(QString("Seat %1").arg(seatId), frame);
        seatTitle->setStyleSheet("font-size: 20px; font-weight: bold; color: #F5F5F5; border: none;");
        headerLayout->addWidget(seatTitle);
        headerLayout->addStretch();
        
        w.authorityLabel = new QLabel("", frame);
        w.authorityLabel->setStyleSheet("font-size: 13px; font-weight: bold; margin-right: 8px; border: none;");
        headerLayout->addWidget(w.authorityLabel);
        
        w.activateBtn = new QPushButton("Activate", frame);
        w.activateBtn->setStyleSheet(
            "QPushButton { background-color: #E10600; color: #F5F5F5; border: none; border-radius: 4px; padding: 4px 12px; font-weight: bold; }"
            "QPushButton:hover { background-color: #FF1A1A; }"
            "QPushButton:disabled { background-color: #202020; color: #777777; }"
        );
        headerLayout->addWidget(w.activateBtn);
        
        w.deactivateBtn = new QPushButton("Deactivate", frame);
        w.deactivateBtn->setStyleSheet(
            "QPushButton { background-color: #202020; color: #F5F5F5; border: 1px solid #2A2A2A; border-radius: 4px; padding: 4px 12px; font-weight: bold; }"
            "QPushButton:hover { background-color: #2A2A2A; }"
            "QPushButton:disabled { background-color: #151515; color: #777777; }"
        );
        headerLayout->addWidget(w.deactivateBtn);
        
        connect(w.activateBtn, &QPushButton::clicked, [this, seatId]() { onActivateRequested(seatId); });
        connect(w.deactivateBtn, &QPushButton::clicked, [this, seatId]() { onDeactivateRequested(seatId); });

        fl->addLayout(headerLayout);

        w.contentLabel = new QLabel("Loading...", frame);
        w.contentLabel->setStyleSheet("font-size: 14px; line-height: 1.5; color: #B5B5B5; border: none;");
        fl->addWidget(w.contentLabel);
        
        // Controller Section
        auto* ctrlTitle = new QLabel("Controller Assignment", frame);
        ctrlTitle->setStyleSheet("font-size: 16px; font-weight: bold; color: #F5F5F5; margin-top: 16px; margin-bottom: 4px; border: none;");
        fl->addWidget(ctrlTitle);

        w.bindingStateLabel = new QLabel("Status:\n○ Not Assigned", frame);
        w.bindingStateLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-bottom: 12px;");
        fl->addWidget(w.bindingStateLabel);

        auto* physLabel = new QLabel("Physical Controller", frame);
        physLabel->setStyleSheet("font-size: 13px; color: #B5B5B5; margin-bottom: 2px; border: none;");
        fl->addWidget(physLabel);

        w.physCombo = new QComboBox(frame);
        w.physCombo->setStyleSheet("padding: 6px; background-color: #202020; color: #F5F5F5; border: 1px solid #2A2A2A; border-radius: 4px;");
        fl->addWidget(w.physCombo);

        auto* srcLabel = new QLabel("XInput Runtime Source", frame);
        srcLabel->setStyleSheet("font-size: 13px; color: #B5B5B5; margin-top: 8px; margin-bottom: 2px; border: none;");
        fl->addWidget(srcLabel);

        w.srcCombo = new QComboBox(frame);
        w.srcCombo->setStyleSheet("padding: 6px; background-color: #202020; color: #F5F5F5; border: 1px solid #2A2A2A; border-radius: 4px;");
        fl->addWidget(w.srcCombo);

        w.pairBtn = new QPushButton("Pair Controller", frame);
        w.pairBtn->setStyleSheet(
            "QPushButton { background-color: #E10600; color: #F5F5F5; border: none; border-radius: 4px; padding: 8px 16px; font-weight: bold; margin-top: 12px; }"
            "QPushButton:hover { background-color: #FF1A1A; }"
            "QPushButton:disabled { background-color: #202020; color: #777777; }"
        );
        w.pairBtn->setDisabled(true);
        fl->addWidget(w.pairBtn);

        w.feedbackLabel = new QLabel("", frame);
        w.feedbackLabel->setStyleSheet("font-size: 12px; color: #E10600; margin-top: 4px; border: none;");
        w.feedbackLabel->setWordWrap(true);
        w.feedbackLabel->setVisible(false);
        fl->addWidget(w.feedbackLabel);

        auto checkPairBtn = [&w]() {
            bool validPhys = w.physCombo->currentIndex() >= 0 && !w.physCombo->currentData().toString().isEmpty();
            bool validSrc = w.srcCombo->currentIndex() >= 0 && !w.srcCombo->currentData().toString().isEmpty();
            w.pairBtn->setEnabled(validPhys && validSrc);
        };

        connect(w.physCombo, &QComboBox::currentIndexChanged, checkPairBtn);
        connect(w.srcCombo, &QComboBox::currentIndexChanged, checkPairBtn);

        connect(w.pairBtn, &QPushButton::clicked, [this, seatId]() {
            onPairRequested(seatId);
        });

        fl->addStretch();
        return frame;
    };

    seatsLayout->addWidget(buildSeat(1, m_seat1));
    seatsLayout->addWidget(buildSeat(2, m_seat2));

    layout->addLayout(seatsLayout);
    layout->addStretch();
}

QString SeatsPage::formatSeatDetails(uint32_t seatId) {
    auto snapshot = m_sessionController->snapshot(seatId);
    if (!snapshot) return "○ UNAVAILABLE";

    QString text;
    if (snapshot->active()) {
        text += "● AVAILABLE\n\n";
    } else {
        text += "○ AVAILABLE\n\n";
    }

    text += "Application\n";
    if (snapshot->process) {
        text += QString("%1\nPID %2\n\n")
                    .arg(snapshot->process->creationIdentity ? "Attached" : "Unknown")
                    .arg(snapshot->process->pid);
    } else {
        text += "Not assigned\n\n";
    }
    
    text += "Window\n";
    if (snapshot->targetHwnd) {
        text += QString("HWND: 0x%1\n\n").arg(snapshot->targetHwnd, 0, 16);
    } else {
        text += "Not assigned\n\n";
    }

    text += "Audio\n";
    if (snapshot->audioEndpoint) {
        text += QString("%1\n● Routed\n\n").arg(QString::fromStdWString(snapshot->audioEndpoint->endpointId));
    } else {
        text += "Not assigned\n\n";
    }

    return text;
}

void SeatsPage::populateCombos(SeatWidgets& w) {
    w.physCombo->blockSignals(true);
    w.srcCombo->blockSignals(true);

    QString prevPhys = w.physCombo->currentData().toString();
    QString prevSrc = w.srcCombo->currentData().toString();

    w.physCombo->clear();
    if (m_lastPayload.controllerInventory.physicalControllers.empty()) {
        w.physCombo->addItem("[ No physical controllers detected ]", "");
    } else {
        w.physCombo->addItem("[ Select physical controller... ▼ ]", "");
        for (const auto& phys : m_lastPayload.controllerInventory.physicalControllers) {
            w.physCombo->addItem(QString::fromStdWString(phys.displayName), QString::fromStdWString(phys.persistentId));
        }
    }

    w.srcCombo->clear();
    bool hasXInput = false;
    for (const auto& src : m_lastPayload.controllerInventory.sources) {
        if (src.api == hydra::controller::ApiSurface::XInput && src.connected && src.runtimeXInputSlot) hasXInput = true;
    }

    if (!hasXInput) {
        w.srcCombo->addItem("[ No XInput runtime sources detected ]", "");
    } else {
        w.srcCombo->addItem("[ Select XInput runtime source... ▼ ]", "");
        for (const auto& src : m_lastPayload.controllerInventory.sources) {
            if (src.api == hydra::controller::ApiSurface::XInput && src.connected && src.runtimeXInputSlot) {
                w.srcCombo->addItem(QString("XInput Slot %1").arg(*src.runtimeXInputSlot), *src.runtimeXInputSlot);
            }
        }
    }

    int pIdx = w.physCombo->findData(prevPhys);
    if (pIdx > 0) w.physCombo->setCurrentIndex(pIdx);
    else w.physCombo->setCurrentIndex(0);

    int sIdx = w.srcCombo->findData(prevSrc);
    if (sIdx > 0) w.srcCombo->setCurrentIndex(sIdx);
    else w.srcCombo->setCurrentIndex(0);

    w.physCombo->blockSignals(false);
    w.srcCombo->blockSignals(false);
    
    // Trigger enablement check
    bool validPhys = w.physCombo->currentIndex() > 0 && !w.physCombo->currentData().toString().isEmpty();
    bool validSrc = w.srcCombo->currentIndex() > 0 && !w.srcCombo->currentData().toString().isEmpty();
    w.pairBtn->setEnabled(validPhys && validSrc);
}

void SeatsPage::updateBindingState(uint32_t seatId, SeatWidgets& w) {
    auto snapshot = m_sessionController->snapshot(seatId);
    if (!snapshot || !snapshot->controllerBinding) {
        w.bindingStateLabel->setText("Status:\n○ Not Assigned");
        w.bindingStateLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-bottom: 12px;");
        return;
    }

    const auto& binding = *snapshot->controllerBinding;
    
    // Check if stale
    bool stale = true;
    bool connected = false;
    for (const auto& src : m_lastPayload.controllerInventory.sources) {
        if (src.api == binding.api && src.runtimeXInputSlot == binding.runtimeXInputSlot) {
            connected = src.connected;
            if (src.sourceGeneration == binding.sourceGeneration) {
                stale = false;
            }
            break;
        }
    }

    QString physName = binding.persistentControllerId ? QString::fromStdWString(*binding.persistentControllerId) : "Unknown Physical Controller";
    for (const auto& p : m_lastPayload.controllerInventory.physicalControllers) {
        if (binding.persistentControllerId && p.persistentId == *binding.persistentControllerId) {
            physName = QString::fromStdWString(p.displayName);
            break;
        }
    }

    QString slotStr = binding.runtimeXInputSlot ? QString::number(*binding.runtimeXInputSlot) : "?";

    if (stale) {
        w.bindingStateLabel->setText(QString("Status:\n⚠ Stale (Slot %1)\n%2").arg(slotStr).arg(physName));
        w.bindingStateLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; border: none; margin-bottom: 12px;");
    } else if (connected) {
        w.bindingStateLabel->setText(QString("Status:\n● Paired (Slot %1)\n%2").arg(slotStr).arg(physName));
        w.bindingStateLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; border: none; margin-bottom: 12px;");
    } else {
        w.bindingStateLabel->setText(QString("Status:\n○ Disconnected (Slot %1)\n%2").arg(slotStr).arg(physName));
        w.bindingStateLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-bottom: 12px;");
    }
}

void SeatsPage::updateState(const EngineStatePayload& payload) {
    m_lastPayload = payload;
    
    auto updateAuthorityUI = [this](uint32_t seatId, SeatWidgets& w) {
        bool active = m_bridge->isUiLeaseActive(seatId);
        auto snapshot = m_sessionController->snapshot(seatId);
        if (active) {
            if (snapshot && snapshot->gameLeaseActive) {
                w.authorityLabel->setText("● UI + Game Active");
            } else {
                w.authorityLabel->setText("● UI Configuring");
            }
            w.authorityLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; margin-right: 8px; border: none;");
            w.activateBtn->hide();
            w.deactivateBtn->show();
        } else {
            if (snapshot && snapshot->gameLeaseActive) {
                w.authorityLabel->setText("● Game Active");
                w.authorityLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; margin-right: 8px; border: none;");
            } else {
                w.authorityLabel->setText("○ Authority Idle");
                w.authorityLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; margin-right: 8px; border: none;");
            }
            w.activateBtn->show();
            w.deactivateBtn->hide();
        }
    };

    updateAuthorityUI(1, m_seat1);
    updateAuthorityUI(2, m_seat2);

    m_seat1.contentLabel->setText(formatSeatDetails(1));
    m_seat2.contentLabel->setText(formatSeatDetails(2));

    populateCombos(m_seat1);
    populateCombos(m_seat2);
    
    updateBindingState(1, m_seat1);
    updateBindingState(2, m_seat2);
}

void SeatsPage::onPairRequested(uint32_t seatId) {
    SeatWidgets& w = (seatId == 1) ? m_seat1 : m_seat2;
    
    if (w.physCombo->currentIndex() <= 0 || w.srcCombo->currentIndex() <= 0) {
        return;
    }

    w.bindingStateLabel->setText("Status:\n◌ Pairing...");
    w.bindingStateLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; border: none; margin-bottom: 12px;");
    w.physCombo->setDisabled(true);
    w.srcCombo->setDisabled(true);
    w.pairBtn->setDisabled(true);
    w.feedbackLabel->setVisible(false);

    std::wstring physId = w.physCombo->currentData().toString().toStdWString();
    uint8_t slot = static_cast<uint8_t>(w.srcCombo->currentData().toUInt());

    auto result = hydra::controller::pairPhysicalControllerToXInput(
        seatId, physId, slot, m_lastPayload.controllerInventory);

    if (result.status != hydra::controller::PairingStatus::Ok || !result.binding) {
        switch (result.status) {
            case hydra::controller::PairingStatus::InvalidSeat:
                w.feedbackLabel->setText("✕ Invalid Seat error.");
                break;
            case hydra::controller::PairingStatus::InvalidPersistentId:
                w.feedbackLabel->setText("✕ Invalid controller error.");
                break;
            case hydra::controller::PairingStatus::PhysicalControllerNotFound:
                w.feedbackLabel->setText("✕ Controller disappeared.");
                break;
            case hydra::controller::PairingStatus::AmbiguousPhysicalController:
                w.feedbackLabel->setText("✕ Ambiguous identity error.");
                break;
            case hydra::controller::PairingStatus::RuntimeSlotOutOfRange:
                w.feedbackLabel->setText("✕ Invalid XInput source.");
                break;
            case hydra::controller::PairingStatus::RuntimeSourceNotFound:
                w.feedbackLabel->setText("✕ XInput source unavailable.");
                break;
            case hydra::controller::PairingStatus::RuntimeSourceDisconnected:
                w.feedbackLabel->setText("✕ Controller disconnected.");
                break;
            default:
                w.feedbackLabel->setText("✕ Pairing failed.");
                break;
        }
        w.feedbackLabel->setVisible(true);
        w.physCombo->setDisabled(false);
        w.srcCombo->setDisabled(false);
        w.pairBtn->setDisabled(false);
        return;
    }

    bool success = m_bridge->pairController(seatId, *result.binding, m_lastPayload.controllerInventory);

    if (!success) {
        w.feedbackLabel->setText("✕ Pairing failed or authority unavailable.");
        w.feedbackLabel->setVisible(true);
        w.physCombo->setDisabled(false);
        w.srcCombo->setDisabled(false);
        w.pairBtn->setDisabled(false);
        return;
    }

    w.feedbackLabel->setVisible(false);
    w.physCombo->setDisabled(false);
    w.srcCombo->setDisabled(false);
    w.pairBtn->setDisabled(false);
}

void SeatsPage::onActivateRequested(uint32_t seatId) {
    if (!m_bridge->requestUiLease(seatId)) {
        SeatWidgets& w = (seatId == 1) ? m_seat1 : m_seat2;
        w.feedbackLabel->setText("✕ Failed to activate Seat.");
        w.feedbackLabel->setVisible(true);
    }
}

void SeatsPage::onDeactivateRequested(uint32_t seatId) {
    m_bridge->releaseUiLease(seatId);
}

} // namespace hydra::ui
