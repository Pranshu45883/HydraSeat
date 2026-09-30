#include "ui/pages/applications_page.hpp"
#include <QLabel>
#include <QFrame>

namespace hydra::ui {

ApplicationsPage::ApplicationsPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent)
    : QWidget(parent), m_sessionController(std::move(sessionController)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(24);

    auto* title = new QLabel("Applications", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5; font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);

    auto* subtitle = new QLabel("View all observable applications and their current seat and audio routing assignments", this);
    subtitle->setStyleSheet("font-size: 14px; color: #B5B5B5; font-family: 'Segoe UI', sans-serif; margin-bottom: 8px;");
    layout->addWidget(subtitle);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");

    auto* listContainer = new QWidget(scrollArea);
    listContainer->setStyleSheet("background-color: transparent;");
    m_listLayout = new QVBoxLayout(listContainer);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(8);
    m_listLayout->addStretch();

    scrollArea->setWidget(listContainer);
    layout->addWidget(scrollArea);
}

QString ApplicationsPage::getAssignedSeat(const std::optional<hydra::runtime::ProcessIdentity>& identity) const {
    if (!identity || !identity->valid()) return "Unassigned";

    if (auto s1 = m_sessionController->snapshot(1); s1 && s1->process && *s1->process == *identity) return "Seat 1";
    if (auto s2 = m_sessionController->snapshot(2); s2 && s2->process && *s2->process == *identity) return "Seat 2";

    return "Unassigned";
}

void ApplicationsPage::updateState(const EngineStatePayload& payload) {
    while (QLayoutItem* item = m_listLayout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (payload.audioSessionError) {
        auto* err = new QLabel("Failed to enumerate audio sessions (used to track applications).");
        err->setStyleSheet("color: #E10600; font-size: 13px; font-weight: bold;");
        m_listLayout->addWidget(err);
        m_listLayout->addStretch();
        return;
    }

    if (payload.audioSessions.empty()) {
        auto* empty = new QLabel("No audio-producing applications detected.");
        empty->setStyleSheet("color: #777777; font-size: 13px;");
        m_listLayout->addWidget(empty);
        m_listLayout->addStretch();
        return;
    }

    for (const auto& session : payload.audioSessions) {
        auto* frame = new QFrame();
        frame->setMaximumWidth(800);
        frame->setStyleSheet("background-color: #151515; border-radius: 6px; border: 1px solid #292929; padding: 12px 16px;");
        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(0,0,0,0);
        fl->setSpacing(8);
        
        auto* nameLabel = new QLabel(session.displayName ? QString::fromStdWString(*session.displayName) : "Unknown", frame);
        nameLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: #F5F5F5; border: none;");
        fl->addWidget(nameLabel);

        QString creationIdStr = session.processIdentity 
            ? QString::number(session.processIdentity->creationIdentity) 
            : "N/A";
            
        auto* pidLabel = new QLabel(QString("PID %1    CID %2").arg(session.processId).arg(creationIdStr), frame);
        pidLabel->setStyleSheet("font-size: 12px; color: #777777; font-family: 'Consolas', monospace; border: none;");
        fl->addWidget(pidLabel);
        
        auto* bottomLayout = new QHBoxLayout();
        
        QString stateText = session.state == hydra::windows::AudioSessionState::Active ? "● Active" : "○ Inactive";
        QString stateColor = session.state == hydra::windows::AudioSessionState::Active ? "#E10600" : "#777777";
        auto* stateLabel = new QLabel(stateText, frame);
        stateLabel->setStyleSheet(QString("font-size: 13px; font-weight: bold; color: %1; border: none;").arg(stateColor));
        stateLabel->setFixedWidth(100);
        bottomLayout->addWidget(stateLabel);
        
        QString seatName = getAssignedSeat(session.processIdentity);
        auto* seatLabel = new QLabel(QString("Seat: %1").arg(seatName), frame);
        seatLabel->setStyleSheet("font-size: 13px; color: #B5B5B5; border: none;");
        seatLabel->setFixedWidth(120);
        bottomLayout->addWidget(seatLabel);
        
        QString audioStr = session.endpointId.empty() ? "Unassigned" : QString::fromStdWString(session.endpointId);
        auto* audioLabel = new QLabel(QString("Audio: %1").arg(audioStr), frame);
        audioLabel->setStyleSheet("font-size: 13px; color: #B5B5B5; border: none;");
        bottomLayout->addWidget(audioLabel);
        
        bottomLayout->addStretch();
        fl->addLayout(bottomLayout);

        m_listLayout->addWidget(frame);
    }
    
    m_listLayout->addStretch();
}

} // namespace hydra::ui
