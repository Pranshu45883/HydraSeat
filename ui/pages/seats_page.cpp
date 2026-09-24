#include "ui/pages/seats_page.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>

namespace hydra::ui {

SeatsPage::SeatsPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent)
    : QWidget(parent), m_sessionController(std::move(sessionController)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(24);

    auto* title = new QLabel("Seats Overview", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: white;");
    layout->addWidget(title);

    auto* seatsLayout = new QHBoxLayout();
    
    // Seat 1
    auto* seat1Frame = new QFrame(this);
    seat1Frame->setStyleSheet("background-color: #1A1A1A; border-radius: 8px; padding: 16px;");
    auto* seat1Layout = new QVBoxLayout(seat1Frame);
    auto* seat1Title = new QLabel("Seat 1", seat1Frame);
    seat1Title->setStyleSheet("font-size: 20px; font-weight: bold; color: white;");
    seat1Layout->addWidget(seat1Title);
    
    m_seat1Content = new QLabel("Loading...", seat1Frame);
    m_seat1Content->setStyleSheet("font-size: 14px; line-height: 1.5; color: #CCCCCC;");
    seat1Layout->addWidget(m_seat1Content);
    seat1Layout->addStretch();
    seatsLayout->addWidget(seat1Frame);

    // Seat 2
    auto* seat2Frame = new QFrame(this);
    seat2Frame->setStyleSheet("background-color: #1A1A1A; border-radius: 8px; padding: 16px;");
    auto* seat2Layout = new QVBoxLayout(seat2Frame);
    auto* seat2Title = new QLabel("Seat 2", seat2Frame);
    seat2Title->setStyleSheet("font-size: 20px; font-weight: bold; color: white;");
    seat2Layout->addWidget(seat2Title);

    m_seat2Content = new QLabel("Loading...", seat2Frame);
    m_seat2Content->setStyleSheet("font-size: 14px; line-height: 1.5; color: #CCCCCC;");
    seat2Layout->addWidget(m_seat2Content);
    seat2Layout->addStretch();
    seatsLayout->addWidget(seat2Frame);

    layout->addLayout(seatsLayout);
    layout->addStretch();
}

QString SeatsPage::formatSeatDetails(uint32_t seatId) {
    auto snapshot = m_sessionController->snapshot(seatId);
    if (!snapshot) return "○ UNAVAILABLE";

    QString text;
    if (snapshot->active) {
        text += "● ACTIVE\n\n";
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

    text += "Input Binding\n";
    if (snapshot->controllerBinding) {
        text += QString("XInput Slot %1\n\n").arg(snapshot->controllerBinding->runtimeXInputSlot.value_or(0));
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

void SeatsPage::updateState(const EngineStatePayload& payload) {
    (void)payload; // We only need SessionController state for this page currently
    m_seat1Content->setText(formatSeatDetails(1));
    m_seat2Content->setText(formatSeatDetails(2));
}

} // namespace hydra::ui
