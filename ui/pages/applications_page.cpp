#include "ui/pages/applications_page.hpp"
#include <QLabel>
#include <QFrame>

namespace hydra::ui {

ApplicationsPage::ApplicationsPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent)
    : QWidget(parent), m_sessionController(std::move(sessionController)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(24);

    auto* title = new QLabel("Observable Applications", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5;");
    layout->addWidget(title);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");

    auto* listContainer = new QWidget(scrollArea);
    listContainer->setStyleSheet("background-color: transparent;");
    m_listLayout = new QVBoxLayout(listContainer);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(12);
    m_listLayout->addStretch(); // Push items to top

    scrollArea->setWidget(listContainer);
    layout->addWidget(scrollArea);
}

QString ApplicationsPage::getAssignedSeat(const std::optional<hydra::runtime::ProcessIdentity>& identity) const {
    if (!identity) return "Unverified";

    auto checkSeat = [&](uint32_t seatId) -> QString {
        if (auto s = m_sessionController->snapshot(seatId)) {
            if (s->process && *s->process == *identity) {
                return QString("Seat %1 — Owned").arg(seatId);
            }
        }
        return "";
    };

    QString seat = checkSeat(1);
    if (!seat.isEmpty()) return seat;
    
    seat = checkSeat(2);
    if (!seat.isEmpty()) return seat;

    return "Unassigned";
}

void ApplicationsPage::updateState(const EngineStatePayload& payload) {
    // Clear layout (except stretch)
    while (QLayoutItem* item = m_listLayout->takeAt(0)) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    if (payload.audioSessionError) {
        auto* err = new QLabel("Failed to enumerate applications.");
        err->setStyleSheet("color: #E10600;");
        m_listLayout->addWidget(err);
        m_listLayout->addStretch();
        return;
    }

    if (payload.audioSessions.empty()) {
        auto* empty = new QLabel("No applications currently have observable audio sessions.");
        empty->setStyleSheet("color: #777777; font-style: italic;");
        m_listLayout->addWidget(empty);
        m_listLayout->addStretch();
        return;
    }

    for (const auto& session : payload.audioSessions) {
        // Skip system/unnamed sessions for cleaner UI
        QString displayName = QString::fromStdWString(session.displayName.value_or(L"Unknown"));
        if (displayName == "Unknown" || displayName.startsWith("@%SystemRoot%")) {
            if (session.processId != 0) {
                displayName = QString("Process %1").arg(session.processId);
            } else {
                continue;
            }
        }

        auto* frame = new QFrame();
        frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 12px; border: 1px solid #2A2A2A;");
        auto* fl = new QVBoxLayout(frame);
        
        auto* nameLabel = new QLabel(displayName, frame);
        nameLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: #F5F5F5; border: none;");
        fl->addWidget(nameLabel);

        QString seatStr = getAssignedSeat(session.processIdentity);
        QString stateStr = (session.state == hydra::windows::AudioSessionState::Active) ? "Active" : "Inactive";
        
        QString creationIdStr = session.processIdentity 
            ? QString::number(session.processIdentity->creationIdentity) 
            : "N/A";

        QString detailsStr = QString("PID: %1  |  CID: %2  |  Audio Session: %3  |  %4")
                                .arg(session.processId)
                                .arg(creationIdStr)
                                .arg(stateStr)
                                .arg(seatStr);

        auto* detailsLabel = new QLabel(detailsStr, frame);
        detailsLabel->setStyleSheet("font-size: 13px; color: #B5B5B5; border: none;");
        fl->addWidget(detailsLabel);

        m_listLayout->addWidget(frame);
    }
    
    m_listLayout->addStretch();
}

} // namespace hydra::ui
