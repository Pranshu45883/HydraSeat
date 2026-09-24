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
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: white;");
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

QString ApplicationsPage::getAssignedSeat(uint32_t pid) const {
    auto checkSeat = [&](uint32_t seatId) -> QString {
        if (auto s = m_sessionController->snapshot(seatId)) {
            if (s->process && s->process->pid == pid) {
                return QString("Seat %1").arg(seatId);
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
        err->setStyleSheet("color: #FF5555;");
        m_listLayout->addWidget(err);
        m_listLayout->addStretch();
        return;
    }

    if (payload.audioSessions.empty()) {
        auto* empty = new QLabel("No applications currently have observable audio sessions.");
        empty->setStyleSheet("color: #888888; font-style: italic;");
        m_listLayout->addWidget(empty);
        m_listLayout->addStretch();
        return;
    }

    for (const auto& session : payload.audioSessions) {
        // Skip system/unnamed sessions for cleaner UI
        QString displayName = QString::fromStdWString(session.displayName.value_or(L"Unknown"));
        if (displayName == "Unknown" || displayName.startsWith("@%SystemRoot%")) {
            // If we have a valid PID but no name, we could resolve it via Process API,
            // but for this first version we will just show "Process <PID>" if empty.
            if (session.processId != 0) {
                displayName = QString("Process %1").arg(session.processId);
            } else {
                continue;
            }
        }

        auto* frame = new QFrame();
        frame->setStyleSheet("background-color: #1A1A1A; border-radius: 6px; padding: 12px;");
        auto* fl = new QVBoxLayout(frame);
        
        auto* nameLabel = new QLabel(displayName, frame);
        nameLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: white;");
        fl->addWidget(nameLabel);

        QString seatStr = getAssignedSeat(session.processId);
        QString stateStr = (session.state == hydra::windows::AudioSessionState::Active) ? "Active" : "Inactive";
        QString detailsStr = QString("PID: %1  |  Audio Session: %2  |  Seat: %3")
                                .arg(session.processId)
                                .arg(stateStr)
                                .arg(seatStr);

        auto* detailsLabel = new QLabel(detailsStr, frame);
        detailsLabel->setStyleSheet("font-size: 13px; color: #AAAAAA;");
        fl->addWidget(detailsLabel);

        m_listLayout->addWidget(frame);
    }
    
    m_listLayout->addStretch();
}

} // namespace hydra::ui
