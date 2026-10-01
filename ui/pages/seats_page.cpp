#include "ui/pages/seats_page.hpp"

#include <QByteArray>
#include <QFrame>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QVariant>

namespace hydra::ui {
namespace {

QString errorText(const std::string& error, const QString& fallback) {
    return error.empty() ? fallback : QString::fromStdString(error);
}

const char* primaryButtonStyle() {
    return
        "QPushButton { background-color: #E10600; color: #F5F5F5; border: none; "
        "border-radius: 6px; padding: 0 16px; font-weight: bold; height: 34px; }"
        "QPushButton:hover { background-color: #FF1A1A; }"
        "QPushButton:disabled { background-color: #202020; color: #777777; }";
}

const char* secondaryButtonStyle() {
    return
        "QPushButton { background-color: #202020; color: #F5F5F5; "
        "border: 1px solid #333333; border-radius: 6px; padding: 0 16px; "
        "font-weight: bold; height: 34px; }"
        "QPushButton:hover { background-color: #2A2A2A; }"
        "QPushButton:disabled { background-color: #151515; color: #777777; }";
}

} // namespace

SeatsPage::SeatsPage(
    std::shared_ptr<HostControlClient> hostControl,
    QWidget* parent)
    : QWidget(parent),
      m_hostControl(std::move(hostControl)) {
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet(
        "QScrollArea { border: none; background-color: transparent; }");

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
    title->setStyleSheet(
        "font-size: 28px; font-weight: bold; color: #F5F5F5; "
        "font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);

    auto* subtitle = new QLabel(
        "Configure UI leases and controller bindings without taking game authority",
        this);
    subtitle->setStyleSheet(
        "font-size: 14px; color: #B5B5B5; font-family: 'Segoe UI', sans-serif; "
        "margin-bottom: 8px;");
    layout->addWidget(subtitle);

    auto* seatsLayout = new QHBoxLayout();
    seatsLayout->setSpacing(16);

    auto buildSeat = [this](int seatId, SeatWidgets& w) -> QFrame* {
        auto* frame = new QFrame(this);
        frame->setMinimumWidth(400);
        frame->setMaximumWidth(500);
        frame->setStyleSheet(
            "background-color: #151515; border-radius: 8px; "
            "border: 1px solid #292929; padding: 20px;");

        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(12);

        auto* headerLayout = new QHBoxLayout();
        auto* seatTitle =
            new QLabel(QString("Seat %1").arg(seatId), frame);
        seatTitle->setStyleSheet(
            "font-size: 18px; font-weight: bold; color: #F5F5F5; border: none;");
        headerLayout->addWidget(seatTitle);

        w.stateBadge = new QLabel("○ Host unavailable", frame);
        w.stateBadge->setStyleSheet(
            "font-size: 13px; font-weight: bold; color: #777777; border: none; "
            "margin-left: 12px;");
        headerLayout->addWidget(w.stateBadge);
        headerLayout->addStretch();

        w.actionBtn = new QPushButton("Acquire UI Lease", frame);
        w.actionBtn->setStyleSheet(primaryButtonStyle());
        headerLayout->addWidget(w.actionBtn);
        connect(
            w.actionBtn,
            &QPushButton::clicked,
            [this, seatId]() {
                onActivateRequested(static_cast<std::uint32_t>(seatId));
            });

        fl->addLayout(headerLayout);

        auto addField = [&](const QString& labelText, QLabel*& val) {
            auto* h = new QHBoxLayout();
            auto* lab = new QLabel(labelText);
            lab->setStyleSheet(
                "font-size: 13px; color: #777777; border: none;");
            lab->setFixedWidth(100);
            val = new QLabel("Not assigned");
            val->setStyleSheet(
                "font-size: 13px; color: #F5F5F5; border: none;");
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
        divider->setStyleSheet(
            "border: none; background-color: #292929; max-height: 1px; "
            "margin-top: 12px; margin-bottom: 12px;");
        fl->addWidget(divider);

        auto* ctrlTitle = new QLabel("CONTROLLER", frame);
        ctrlTitle->setStyleSheet(
            "font-size: 12px; font-weight: bold; color: #777777; border: none; "
            "margin-bottom: 4px;");
        fl->addWidget(ctrlTitle);

        w.ctrlStatus = new QLabel("Not Assigned", frame);
        w.ctrlStatus->setStyleSheet(
            "font-size: 13px; color: #F5F5F5; border: none; margin-bottom: 8px;");
        fl->addWidget(w.ctrlStatus);

        auto* physLabel = new QLabel("Physical Controller", frame);
        physLabel->setStyleSheet(
            "font-size: 12px; color: #B5B5B5; border: none;");
        fl->addWidget(physLabel);

        w.physCombo = new QComboBox(frame);
        w.physCombo->setStyleSheet(
            "QComboBox { padding: 4px 8px; background-color: #151515; "
            "color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; "
            "height: 34px; }"
            "QComboBox:focus { border: 1px solid #E10600; }");
        fl->addWidget(w.physCombo);

        auto* srcLabel = new QLabel("XInput Runtime Source", frame);
        srcLabel->setStyleSheet(
            "font-size: 12px; color: #B5B5B5; border: none; margin-top: 8px;");
        fl->addWidget(srcLabel);

        w.srcCombo = new QComboBox(frame);
        w.srcCombo->setStyleSheet(
            "QComboBox { padding: 4px 8px; background-color: #151515; "
            "color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; "
            "height: 34px; }"
            "QComboBox:focus { border: 1px solid #E10600; }");
        fl->addWidget(w.srcCombo);

        w.pairBtn = new QPushButton("Pair Controller", frame);
        w.pairBtn->setStyleSheet(
            "QPushButton { background-color: #202020; color: #F5F5F5; "
            "border: 1px solid #333333; border-radius: 6px; height: 34px; "
            "font-weight: bold; margin-top: 12px; }"
            "QPushButton:hover { background-color: #2A2A2A; }"
            "QPushButton:disabled { background-color: #151515; color: #777777; "
            "border: 1px solid #202020; }");
        fl->addWidget(w.pairBtn);
        connect(
            w.pairBtn,
            &QPushButton::clicked,
            [this, seatId]() {
                onPairRequested(static_cast<std::uint32_t>(seatId));
            });

        w.feedbackLabel = new QLabel("", frame);
        w.feedbackLabel->setWordWrap(true);
        w.feedbackLabel->setStyleSheet(
            "font-size: 12px; color: #E10600; border: none;");
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

const hydra::hostipc::SeatSnapshot* SeatsPage::seatSnapshot(
    std::uint32_t seatId) const noexcept {
    if (!m_lastPayload.hostSnapshot ||
        seatId == 0 ||
        seatId > m_lastPayload.hostSnapshot->seats.size()) {
        return nullptr;
    }
    return &m_lastPayload.hostSnapshot->seats[seatId - 1u];
}

void SeatsPage::updateState(const EngineStatePayload& payload) {
    m_lastPayload = payload;

    auto updateSeatData = [&](std::uint32_t seatId, SeatWidgets& w) {
        const auto* snapshot = seatSnapshot(seatId);
        if (!snapshot) {
            w.stateBadge->setText("○ Host unavailable");
            w.stateBadge->setStyleSheet(
                "font-size: 13px; font-weight: bold; color: #777777; "
                "border: none; margin-left: 12px;");
            w.actionBtn->setText("Acquire UI Lease");
            w.actionBtn->setDisabled(true);
            w.appVal->setText("Unavailable");
            w.winVal->setText("Unavailable");
            w.audioVal->setText("Unavailable");
            updateBindingState(seatId, nullptr, w);
            return;
        }

        if (snapshot->gameLeaseActive && snapshot->uiLeaseActive) {
            w.stateBadge->setText("● UI + Game Active");
        } else if (snapshot->gameLeaseActive) {
            w.stateBadge->setText("● Game Active");
        } else if (snapshot->uiLeaseActive) {
            w.stateBadge->setText("● UI Configuring");
        } else {
            w.stateBadge->setText("● Authority Idle");
        }
        w.stateBadge->setStyleSheet(
            snapshot->active
                ? "font-size: 13px; font-weight: bold; color: #E10600; "
                  "border: none; margin-left: 12px;"
                : "font-size: 13px; font-weight: bold; color: #777777; "
                  "border: none; margin-left: 12px;");

        const bool owned =
            m_hostControl && m_hostControl->ownsUiLease(seatId);
        if (owned) {
            w.actionBtn->setText("Release UI Lease");
            w.actionBtn->setStyleSheet(secondaryButtonStyle());
            w.actionBtn->setDisabled(false);
        } else if (snapshot->uiLeaseActive) {
            w.actionBtn->setText("UI Lease Busy");
            w.actionBtn->setStyleSheet(secondaryButtonStyle());
            w.actionBtn->setDisabled(true);
        } else {
            w.actionBtn->setText("Acquire UI Lease");
            w.actionBtn->setStyleSheet(primaryButtonStyle());
            w.actionBtn->setDisabled(false);
        }

        w.appVal->setText(
            snapshot->processOwned
                ? QString("PID %1").arg(snapshot->processId)
                : "Not assigned");
        w.winVal->setText(
            snapshot->windowOwned
                ? QString("HWND: 0x%1").arg(
                      static_cast<qulonglong>(snapshot->targetHwnd),
                      0,
                      16)
                : "Not assigned");
        w.audioVal->setText(
            snapshot->processOwned ? "See Audio page" : "Not assigned");

        updateBindingState(seatId, snapshot, w);
    };

    updateSeatData(1, m_seat1);
    updateSeatData(2, m_seat2);

    populateCombos(m_seat1);
    populateCombos(m_seat2);
}

void SeatsPage::populateCombos(SeatWidgets& w) {
    if (w.physCombo->hasFocus() || w.srcCombo->hasFocus()) return;

    const QString prevPhys = w.physCombo->currentData().toString();
    const QVariant prevSrc = w.srcCombo->currentData();

    w.physCombo->blockSignals(true);
    w.srcCombo->blockSignals(true);
    w.physCombo->clear();
    w.srcCombo->clear();

    w.physCombo->addItem("-- None --", QString());
    for (const auto& phys :
         m_lastPayload.controllerInventory.physicalControllers) {
        w.physCombo->addItem(
            QString::fromStdWString(phys.displayName),
            QString::fromStdWString(phys.persistentId));
    }

    w.srcCombo->addItem("-- None --", QVariant());
    for (const auto& src : m_lastPayload.controllerInventory.sources) {
        if (!src.connected || !src.runtimeXInputSlot) continue;
        w.srcCombo->addItem(
            QString("%1 (XInput %2)")
                .arg(QString::fromStdWString(src.displayName))
                .arg(static_cast<int>(*src.runtimeXInputSlot)),
            QVariant::fromValue(static_cast<int>(*src.runtimeXInputSlot)));
    }

    const int pIdx = w.physCombo->findData(prevPhys);
    if (pIdx > 0) w.physCombo->setCurrentIndex(pIdx);

    const int sIdx = w.srcCombo->findData(prevSrc);
    if (sIdx > 0) w.srcCombo->setCurrentIndex(sIdx);

    w.physCombo->blockSignals(false);
    w.srcCombo->blockSignals(false);
}

void SeatsPage::updateBindingState(
    std::uint32_t seatId,
    const hydra::hostipc::SeatSnapshot* snapshot,
    SeatWidgets& w) {
    if (!snapshot) {
        w.ctrlStatus->setText("Host unavailable");
        w.ctrlStatus->setStyleSheet(
            "font-size: 13px; color: #777777; border: none; margin-bottom: 8px;");
        w.pairBtn->setDisabled(true);
        return;
    }

    if (snapshot->controllerBound) {
        w.ctrlStatus->setText("● Bound");
        w.ctrlStatus->setStyleSheet(
            "font-size: 13px; color: #E10600; font-weight: bold; "
            "border: none; margin-bottom: 8px;");
    } else {
        w.ctrlStatus->setText("Not Assigned");
        w.ctrlStatus->setStyleSheet(
            "font-size: 13px; color: #777777; border: none; margin-bottom: 8px;");
    }

    const bool owned =
        m_hostControl && m_hostControl->ownsUiLease(seatId);
    const bool leaseAvailable = owned || !snapshot->uiLeaseActive;
    w.pairBtn->setDisabled(
        !leaseAvailable ||
        !m_lastPayload.controllerInventory.authoritative);
}

void SeatsPage::onActivateRequested(std::uint32_t seatId) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    w.feedbackLabel->setVisible(false);

    if (!m_hostControl) {
        w.feedbackLabel->setText("Canonical host control is unavailable.");
        w.feedbackLabel->setVisible(true);
        return;
    }

    std::string error;
    std::optional<hydra::hostipc::HostSnapshot> result;
    if (m_hostControl->ownsUiLease(seatId)) {
        result = m_hostControl->releaseUiLease(seatId, &error);
        if (result) {
            w.feedbackLabel->setText("UI lease released. Game authority was unchanged.");
        }
    } else {
        const auto* snapshot = seatSnapshot(seatId);
        if (snapshot && snapshot->uiLeaseActive) {
            w.feedbackLabel->setText(
                "This Seat's UI lease is owned by another control connection.");
            w.feedbackLabel->setVisible(true);
            return;
        }
        result = m_hostControl->acquireUiLease(seatId, &error);
        if (result) {
            w.feedbackLabel->setText("UI lease acquired. Game authority was unchanged.");
        }
    }

    if (!result) {
        w.feedbackLabel->setText(
            errorText(error, "The host rejected the UI lease operation."));
        w.feedbackLabel->setStyleSheet(
            "font-size: 12px; color: #E10600; border: none;");
    } else {
        w.feedbackLabel->setStyleSheet(
            "font-size: 12px; color: #B5B5B5; border: none;");
    }
    w.feedbackLabel->setVisible(true);
}

void SeatsPage::onPairRequested(std::uint32_t seatId) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    w.feedbackLabel->setVisible(false);

    const QString physId = w.physCombo->currentData().toString();
    bool slotOk = false;
    const int slot = w.srcCombo->currentData().toInt(&slotOk);

    if (physId.isEmpty() || !slotOk || slot < 0 ||
        slot >= static_cast<int>(hydra::controller::kXInputSlotCount)) {
        w.feedbackLabel->setText(
            "Select both a physical controller and a connected XInput source.");
        w.feedbackLabel->setVisible(true);
        return;
    }
    if (!m_lastPayload.controllerInventory.authoritative) {
        w.feedbackLabel->setText(
            "Controller inventory is not authoritative; refresh before pairing.");
        w.feedbackLabel->setVisible(true);
        return;
    }
    if (!m_hostControl) {
        w.feedbackLabel->setText("Canonical host control is unavailable.");
        w.feedbackLabel->setVisible(true);
        return;
    }

    const QByteArray latinId = physId.toLatin1();
    if (QString::fromLatin1(latinId) != physId) {
        w.feedbackLabel->setText(
            "This controller identifier cannot be represented by host protocol v2.");
        w.feedbackLabel->setVisible(true);
        return;
    }

    std::string error;
    const auto result = m_hostControl->pairController(
        seatId,
        latinId.toStdString(),
        static_cast<std::uint8_t>(slot),
        &error);
    if (!result) {
        w.feedbackLabel->setText(
            errorText(
                error,
                "Pairing failed because the Seat or controller state became stale."));
        w.feedbackLabel->setStyleSheet(
            "font-size: 12px; color: #E10600; border: none;");
    } else {
        w.feedbackLabel->setText("Controller pairing accepted by the host.");
        w.feedbackLabel->setStyleSheet(
            "font-size: 12px; color: #B5B5B5; border: none;");
    }
    w.feedbackLabel->setVisible(true);
}

} // namespace hydra::ui
