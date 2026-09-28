#include "ui/pages/dashboard_page.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>

namespace hydra::ui {

DashboardPage::DashboardPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent)
    : QWidget(parent), m_sessionController(std::move(sessionController)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(24);

    auto* title = new QLabel("Dashboard", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5;");
    layout->addWidget(title);

    // Stats Section
    auto* statsFrame = new QFrame(this);
    statsFrame->setStyleSheet("background-color: #151515; border-radius: 8px; padding: 16px; border: 1px solid #2A2A2A;");
    auto* statsLayout = new QVBoxLayout(statsFrame);
    m_statsLabel = new QLabel("Loading statistics...", statsFrame);
    m_statsLabel->setStyleSheet("font-size: 14px; line-height: 1.5; color: #B5B5B5; border: none;");
    statsLayout->addWidget(m_statsLabel);
    layout->addWidget(statsFrame);

    // Seats Section
    auto* seatsTitle = new QLabel("Current Seats", this);
    seatsTitle->setStyleSheet("font-size: 20px; font-weight: bold; color: #F5F5F5; margin-top: 16px;");
    layout->addWidget(seatsTitle);

    auto* seatsLayout = new QHBoxLayout();
    
    // Seat 1
    auto* seat1Frame = new QFrame(this);
    seat1Frame->setStyleSheet("background-color: #151515; border-radius: 8px; padding: 16px; border: 1px solid #2A2A2A;");
    auto* seat1Layout = new QVBoxLayout(seat1Frame);
    m_seat1Label = new QLabel("Loading Seat 1...", seat1Frame);
    m_seat1Label->setStyleSheet("font-size: 14px; color: #B5B5B5; border: none;");
    seat1Layout->addWidget(m_seat1Label);
    seatsLayout->addWidget(seat1Frame);

    // Seat 2
    auto* seat2Frame = new QFrame(this);
    seat2Frame->setStyleSheet("background-color: #151515; border-radius: 8px; padding: 16px; border: 1px solid #2A2A2A;");
    auto* seat2Layout = new QVBoxLayout(seat2Frame);
    m_seat2Label = new QLabel("Loading Seat 2...", seat2Frame);
    m_seat2Label->setStyleSheet("font-size: 14px; color: #B5B5B5; border: none;");
    seat2Layout->addWidget(m_seat2Label);
    seatsLayout->addWidget(seat2Frame);

    layout->addLayout(seatsLayout);
    layout->addStretch();
}

QString DashboardPage::formatSeatInfo(uint32_t seatId) {
    auto snapshot = m_sessionController->snapshot(seatId);
    if (!snapshot) return QString("Seat %1\n○ Unavailable").arg(seatId);

    QString text = QString("Seat %1\n").arg(seatId);
    if (snapshot->active()) {
        text += "● AVAILABLE\n\n";
    } else {
        text += "○ AVAILABLE\n\n";
    }

    if (snapshot->process) {
        text += QString("Application: PID %1\n")
                    .arg(snapshot->process->pid);
    } else {
        text += "Application: Not assigned\n";
    }

    // Audio
    if (snapshot->audioEndpoint) {
        text += QString("Audio: %1").arg(QString::fromStdWString(snapshot->audioEndpoint->endpointId));
    } else {
        text += "Audio: Not assigned";
    }

    return text;
}

void DashboardPage::updateState(const EngineStatePayload& payload) {
    // Calculate active audio outputs
    int activeOutputs = 0;
    for (const auto& ep : payload.audioEndpoints) {
        if (ep.isAvailable()) activeOutputs++;
    }

    // Active seats
    int activeSeats = 0;
    if (auto s1 = m_sessionController->snapshot(1); s1 && s1->active()) activeSeats++;
    if (auto s2 = m_sessionController->snapshot(2); s2 && s2->active()) activeSeats++;

    size_t inputCount = payload.keyboards.size() + payload.mice.size() + payload.controllers.size();

    QString stats = QString(
        "Seats:             2\n"
        "Active Seats:      %1\n"
        "Displays:          %2\n"
        "Input Devices:     %3\n"
        "Audio Endpoints:   %4\n"
        "Active Outputs:    %5\n"
        "Audio Sessions:    %6\n"
    ).arg(activeSeats)
     .arg(payload.displays.size())
     .arg(inputCount)
     .arg(payload.audioEndpoints.size())
     .arg(activeOutputs)
     .arg(payload.audioSessions.size());

    m_statsLabel->setText(stats);

    m_seat1Label->setText(formatSeatInfo(1));
    m_seat2Label->setText(formatSeatInfo(2));
}

} // namespace hydra::ui
