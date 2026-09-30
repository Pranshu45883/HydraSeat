#include "ui/pages/audio_page.hpp"

namespace hydra::ui {

AudioPage::AudioPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, RoutingController* router, QWidget* parent)
    : QWidget(parent), m_sessionController(std::move(sessionController)), m_router(router) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(24);

    auto* title = new QLabel("Audio Routing", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5; font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);

    auto* subtitle = new QLabel("Manage per-application audio output assignments", this);
    subtitle->setStyleSheet("font-size: 14px; color: #B5B5B5; font-family: 'Segoe UI', sans-serif; margin-bottom: 8px;");
    layout->addWidget(subtitle);

    auto* toolbarLayout = new QHBoxLayout();
    m_searchBox = new QLineEdit(this);
    m_searchBox->setPlaceholderText("Search applications...");
    m_searchBox->setFixedWidth(300);
    m_searchBox->setStyleSheet("padding: 6px; background-color: #151515; color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; height: 34px;");
    toolbarLayout->addWidget(m_searchBox);

    m_filterCombo = new QComboBox(this);
    m_filterCombo->addItem("All");
    m_filterCombo->addItem("Active Only");
    m_filterCombo->setStyleSheet("padding: 4px 8px; background-color: #151515; color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; height: 34px;");
    toolbarLayout->addWidget(m_filterCombo);
    toolbarLayout->addStretch();
    layout->addLayout(toolbarLayout);
    
    auto* splitLayout = new QHBoxLayout();
    splitLayout->setSpacing(24);
    
    // Left side: Sessions
    auto* leftWidget = new QWidget();
    auto* leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0,0,0,0);
    
    m_sessionsCountLabel = new QLabel("AUDIO SESSIONS   0 total / 0 active");
    m_sessionsCountLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; margin-bottom: 8px;");
    leftLayout->addWidget(m_sessionsCountLabel);
    
    auto* sessScroll = new QScrollArea();
    sessScroll->setWidgetResizable(true);
    sessScroll->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");
    auto* sessContainer = new QWidget();
    sessContainer->setStyleSheet("background-color: transparent;");
    m_sessionsLayout = new QVBoxLayout(sessContainer);
    m_sessionsLayout->setContentsMargins(0,0,0,0);
    m_sessionsLayout->setSpacing(12);
    m_sessionsLayout->addStretch();
    sessScroll->setWidget(sessContainer);
    leftLayout->addWidget(sessScroll);
    splitLayout->addWidget(leftWidget, 2);
    
    // Right side: Outputs
    auto* rightWidget = new QWidget();
    auto* rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0,0,0,0);
    
    m_outputsCountLabel = new QLabel("AUDIO OUTPUTS   0 total / 0 active");
    m_outputsCountLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; margin-bottom: 8px;");
    rightLayout->addWidget(m_outputsCountLabel);
    
    auto* outScroll = new QScrollArea();
    outScroll->setWidgetResizable(true);
    outScroll->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");
    auto* outContainer = new QWidget();
    outContainer->setStyleSheet("background-color: transparent;");
    m_outputsLayout = new QVBoxLayout(outContainer);
    m_outputsLayout->setContentsMargins(0,0,0,0);
    m_outputsLayout->setSpacing(8);
    m_outputsLayout->addStretch();
    outScroll->setWidget(outContainer);
    rightLayout->addWidget(outScroll);
    splitLayout->addWidget(rightWidget, 1);
    
    layout->addLayout(splitLayout);

    if (m_router) {
        connect(m_router, &RoutingController::routingCompleted, this, &AudioPage::onRoutingCompleted);
        connect(m_router, &RoutingController::resetCompleted, this, &AudioPage::onResetCompleted);
    }
}

QString AudioPage::stateText(hydra::windows::AudioSessionState state) {
    switch (state) {
        case hydra::windows::AudioSessionState::Active: return "● ACTIVE";
        case hydra::windows::AudioSessionState::Inactive: return "● INACTIVE";
        default: return "○ EXPIRED";
    }
}

QString AudioPage::stateColor(hydra::windows::AudioSessionState state) {
    switch (state) {
        case hydra::windows::AudioSessionState::Active: return "#E10600";
        case hydra::windows::AudioSessionState::Inactive: return "#B5B5B5";
        default: return "#777777";
    }
}

QString AudioPage::resolveEndpointFriendlyName(const std::wstring& endpointId) const {
    if (endpointId.empty()) return "System Default";
    for (const auto& ep : m_lastPayload.audioEndpoints) {
        if (ep.endpointId == endpointId) return QString::fromStdWString(ep.friendlyName);
    }
    return QString::fromStdWString(endpointId);
}

bool AudioPage::sessionMatchesFilter(const hydra::windows::AudioSessionObservation& session, const QString& filterText, int filterType) const {
    if (filterType == 1 && session.state != hydra::windows::AudioSessionState::Active) return false;
    if (filterText.isEmpty()) return true;
    return (session.displayName ? QString::fromStdWString(*session.displayName) : "Unknown").contains(filterText, Qt::CaseInsensitive);
}

void AudioPage::buildSessionCard(const hydra::windows::AudioSessionObservation& session) {
    AudioSessionCard card;
    card.pid = session.processId;
    card.creationIdentity = session.processIdentity ? session.processIdentity->creationIdentity : 0;
    card.currentEndpointId = session.endpointId;

    card.frame = new QFrame();
    card.frame->setMaximumWidth(600);
    card.frame->setStyleSheet("background-color: #151515; border-radius: 8px; border: 1px solid #292929; padding: 16px;");
    auto* fl = new QVBoxLayout(card.frame);
    fl->setContentsMargins(0,0,0,0);
    fl->setSpacing(8);

    auto* headerLayout = new QHBoxLayout();
    card.nameLabel = new QLabel((session.displayName ? QString::fromStdWString(*session.displayName) : "Unknown"), card.frame);
    card.nameLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: #F5F5F5; border: none;");
    headerLayout->addWidget(card.nameLabel);
    headerLayout->addStretch();
    card.stateLabel = new QLabel(stateText(session.state), card.frame);
    card.stateLabel->setStyleSheet(QString("font-size: 12px; font-weight: bold; color: %1; border: none;").arg(stateColor(session.state)));
    headerLayout->addWidget(card.stateLabel);
    fl->addLayout(headerLayout);

    card.pidLabel = new QLabel(QString("PID: %1").arg(session.processId), card.frame);
    card.pidLabel->setStyleSheet("font-size: 12px; color: #777777; font-family: 'Consolas', monospace; border: none;");
    fl->addWidget(card.pidLabel);

    card.currentOutputLabel = new QLabel(QString("Current Output: %1").arg(resolveEndpointFriendlyName(session.endpointId)), card.frame);
    card.currentOutputLabel->setStyleSheet("font-size: 13px; color: #B5B5B5; border: none; margin-bottom: 8px;");
    fl->addWidget(card.currentOutputLabel);
    
    auto* routeToLbl = new QLabel("Route to:", card.frame);
    routeToLbl->setStyleSheet("font-size: 12px; color: #777777; border: none;");
    fl->addWidget(routeToLbl);

    card.routeCombo = new QComboBox(card.frame);
    card.routeCombo->setStyleSheet("QComboBox { padding: 4px 8px; background-color: #101010; color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; height: 34px; }"
                                   "QComboBox:focus { border: 1px solid #E10600; }");
    card.routeCombo->addItem("System Default", "");
    for (const auto& ep : m_lastPayload.audioEndpoints) {
        if (!ep.isAvailable()) continue;
        card.routeCombo->addItem(QString::fromStdWString(ep.friendlyName), QString::fromStdWString(ep.endpointId));
    }
    
    int cIdx = card.routeCombo->findData(QString::fromStdWString(session.endpointId));
    if (cIdx >= 0) card.routeCombo->setCurrentIndex(cIdx);
    
    fl->addWidget(card.routeCombo);

    auto* btnLayout = new QHBoxLayout();
    card.feedbackLabel = new QLabel("", card.frame);
    card.feedbackLabel->setStyleSheet("font-size: 12px; color: #E10600; border: none;");
    card.feedbackLabel->setVisible(false);
    btnLayout->addWidget(card.feedbackLabel);
    btnLayout->addStretch();

    card.resetBtn = new QPushButton("Reset", card.frame);
    card.resetBtn->setStyleSheet(
        "QPushButton { background-color: #202020; color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; height: 32px; padding: 0 16px; }"
        "QPushButton:hover { background-color: #2A2A2A; }"
    );
    btnLayout->addWidget(card.resetBtn);

    card.routeBtn = new QPushButton("Route Audio", card.frame);
    card.routeBtn->setStyleSheet(
        "QPushButton { background-color: #E10600; color: #F5F5F5; border: none; border-radius: 6px; height: 32px; padding: 0 16px; font-weight: bold; }"
        "QPushButton:hover { background-color: #FF1A1A; }"
    );
    btnLayout->addWidget(card.routeBtn);
    fl->addLayout(btnLayout);

    uint32_t cpid = card.pid;
    uint64_t ccid = card.creationIdentity;
    connect(card.resetBtn, &QPushButton::clicked, [this, cpid, ccid]() {
        if (m_router) m_router->requestReset(cpid, ccid);
    });
    connect(card.routeBtn, &QPushButton::clicked, [this, cpid, ccid, combo = card.routeCombo]() {
        QString ep = combo->currentData().toString();
        if (m_router) m_router->requestRoute(cpid, ccid, ep);
    });

    m_sessionsLayout->insertWidget(m_sessionsLayout->count() - 1, card.frame);
    m_sessionCards.append(card);
}

bool AudioPage::tryUpdateExistingCard(const hydra::windows::AudioSessionObservation& session) {
    for (auto& card : m_sessionCards) {
        if (card.pid == session.processId && card.creationIdentity == (session.processIdentity ? session.processIdentity->creationIdentity : 0)) {
            card.stateLabel->setText(stateText(session.state));
            card.stateLabel->setStyleSheet(QString("font-size: 12px; font-weight: bold; color: %1; border: none;").arg(stateColor(session.state)));
            if (card.currentEndpointId != session.endpointId) {
                card.currentEndpointId = session.endpointId;
                card.currentOutputLabel->setText(QString("Current Output: %1").arg(resolveEndpointFriendlyName(session.endpointId)));
            }
            return true;
        }
    }
    return false;
}

void AudioPage::updateState(const EngineStatePayload& payload) {
    m_lastPayload = payload;

    int totalSess = payload.audioSessions.size();
    int actSess = 0;
    for (const auto& s : payload.audioSessions) if (s.state == hydra::windows::AudioSessionState::Active) actSess++;
    m_sessionsCountLabel->setText(QString("AUDIO SESSIONS   %1 total / %2 active").arg(totalSess).arg(actSess));

    int totalOut = payload.audioEndpoints.size();
    int actOut = 0;
    
    // Process outputs
    while (QLayoutItem* item = m_outputsLayout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    for (const auto& ep : payload.audioEndpoints) {
        if (ep.isAvailable()) actOut++;
        
        auto* frame = new QFrame();
        frame->setMaximumWidth(400);
        frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 12px; border: 1px solid #292929;");
        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(0,0,0,0);
        
        auto* nameLabel = new QLabel(QString::fromStdWString(ep.friendlyName), frame);
        nameLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: #F5F5F5; border: none;");
        fl->addWidget(nameLabel);
        
        auto* stLabel = new QLabel(ep.isAvailable() ? "● Active" : "○ Not Present", frame);
        stLabel->setStyleSheet(QString("font-size: 12px; font-weight: bold; color: %1; border: none;").arg(ep.isAvailable() ? "#E10600" : "#777777"));
        fl->addWidget(stLabel);
        
        m_outputsLayout->addWidget(frame);
    }
    m_outputsLayout->addStretch();
    
    m_outputsCountLabel->setText(QString("AUDIO OUTPUTS   %1 total / %2 active").arg(totalOut).arg(actOut));

    // Update Sessions
    QString filterText = m_searchBox->text();
    int filterType = m_filterCombo->currentIndex();

    for (int i = m_sessionCards.size() - 1; i >= 0; --i) {
        bool found = false;
        for (const auto& session : payload.audioSessions) {
            if (m_sessionCards[i].pid == session.processId && 
                m_sessionCards[i].creationIdentity == (session.processIdentity ? session.processIdentity->creationIdentity : 0) &&
                sessionMatchesFilter(session, filterText, filterType)) {
                found = true;
                break;
            }
        }
        if (!found) {
            m_sessionCards[i].frame->deleteLater();
            m_sessionCards.removeAt(i);
        }
    }

    for (const auto& session : payload.audioSessions) {
        if (!sessionMatchesFilter(session, filterText, filterType)) continue;
        if (!tryUpdateExistingCard(session)) {
            buildSessionCard(session);
        }
    }
}

void AudioPage::onRoutingCompleted(uint32_t pid, RouteVerificationResult result, const QString& errorMessage) {
    for (auto& card : m_sessionCards) {
        if (card.pid == pid) {
            card.feedbackLabel->setVisible(true);
            if (result == RouteVerificationResult::Success) {
                card.feedbackLabel->setText("Routing successful.");
                card.feedbackLabel->setStyleSheet("color: #00FF00;");
            } else {
                card.feedbackLabel->setText(errorMessage);
                card.feedbackLabel->setStyleSheet("color: #E10600;");
            }
            return;
        }
    }
}

void AudioPage::onResetCompleted(uint32_t pid, bool success, const QString& errorMessage) {
    for (auto& card : m_sessionCards) {
        if (card.pid == pid) {
            card.feedbackLabel->setVisible(true);
            if (success) {
                card.feedbackLabel->setText("Reset successful.");
                card.feedbackLabel->setStyleSheet("color: #00FF00;");
            } else {
                card.feedbackLabel->setText(errorMessage);
                card.feedbackLabel->setStyleSheet("color: #E10600;");
            }
            return;
        }
    }
}

} // namespace hydra::ui
