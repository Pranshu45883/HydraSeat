#include "ui/pages/audio_page.hpp"

#include <QLabel>
#include <QFrame>
#include <QFileInfo>
#include <QTimer>
#include <QStandardItemModel>
#include <QPainter>

#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")

namespace hydra::ui {

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

QString AudioPage::resolveProcessName(uint32_t pid) {
    if (pid == 0) return QStringLiteral("System");
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return QString("PID %1").arg(pid);
    wchar_t buf[MAX_PATH] = {};
    DWORD sz = MAX_PATH;
    QueryFullProcessImageNameW(h, 0, buf, &sz);
    CloseHandle(h);
    QString full = QString::fromWCharArray(buf);
    if (full.isEmpty()) return QString("PID %1").arg(pid);
    return QFileInfo(full).baseName();
}

QString AudioPage::resolveProcessNameCached(uint32_t pid) {
    if (m_processNameCache.contains(pid)) {
        return m_processNameCache.value(pid);
    }
    QString name = resolveProcessName(pid);
    m_processNameCache.insert(pid, name);
    return name;
}

QPixmap AudioPage::resolveProcessIcon(uint32_t pid) {
    // Return a simple colored circle as a fallback icon.
    // Qt6 removed QImage::fromHICON from the public API.
    // Shell icon extraction can be added via QtWin if that module is available,
    // but we keep this dependency-free for now.
    (void)pid;
    QPixmap px(20, 20);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(0x0A, 0x84, 0xFF, 180));
    p.setPen(Qt::NoPen);
    p.drawEllipse(0, 0, 20, 20);
    return px;
}

QString AudioPage::resolveEndpointFriendlyName(
    const std::wstring& endpointId,
    const std::vector<hydra::windows::AudioRenderEndpoint>& endpoints)
{
    if (endpointId.empty()) return QStringLiteral("Not reported");
    for (const auto& ep : endpoints) {
        if (ep.endpointId == endpointId)
            return QString::fromStdWString(ep.friendlyName);
    }
    return QStringLiteral("Unknown endpoint");
}

QString AudioPage::stateText(hydra::windows::AudioSessionState state) {
    switch (state) {
        case hydra::windows::AudioSessionState::Active:   return QStringLiteral("● ACTIVE");
        case hydra::windows::AudioSessionState::Inactive: return QStringLiteral("○ INACTIVE");
        case hydra::windows::AudioSessionState::Expired:  return QStringLiteral("✕ EXPIRED");
        default:                                           return QStringLiteral("? UNKNOWN");
    }
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

AudioPage::AudioPage(
    std::shared_ptr<hydra::runtime::SessionController> sessionController,
    RoutingController* routingController,
    QWidget* parent)
    : QWidget(parent)
    , m_sessionController(std::move(sessionController))
    , m_routingController(routingController)
{
    setStyleSheet(R"(
        QWidget { background-color: #121212; color: #EEEEEE; }
        QFrame#sessionCard {
            background-color: #1C1C1E;
            border-radius: 10px;
            border: 1px solid #2A2A2C;
        }
        QFrame#endpointCard {
            background-color: #1C1C1E;
            border-radius: 8px;
            border: 1px solid #2A2A2C;
        }
        QLineEdit {
            background-color: #1C1C1E;
            border: 1px solid #3A3A3C;
            border-radius: 6px;
            color: #EEEEEE;
            padding: 6px 10px;
            font-size: 13px;
        }
        QLineEdit:focus { border-color: #0A84FF; }
        QComboBox {
            background-color: #2C2C2E;
            border: 1px solid #3A3A3C;
            border-radius: 6px;
            color: #EEEEEE;
            padding: 5px 10px;
            font-size: 13px;
        }
        QComboBox::drop-down { border: none; }
        QComboBox QAbstractItemView {
            background-color: #2C2C2E;
            color: #EEEEEE;
            selection-background-color: #0A84FF;
        }
        QPushButton#routeBtn {
            background-color: #0A84FF;
            color: white;
            border-radius: 6px;
            padding: 7px 16px;
            font-weight: bold;
            font-size: 13px;
        }
        QPushButton#routeBtn:hover { background-color: #2196F3; }
        QPushButton#routeBtn:disabled { background-color: #3A3A3C; color: #888; }
        QPushButton#resetBtn {
            background-color: #3A3A3C;
            color: #EEEEEE;
            border-radius: 6px;
            padding: 7px 16px;
            font-size: 13px;
        }
        QPushButton#resetBtn:hover { background-color: #4A4A4C; }
        QPushButton#resetBtn:disabled { background-color: #2C2C2E; color: #666; }
        QScrollArea { border: none; background-color: transparent; }
    )");

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(24, 24, 24, 24);
    outerLayout->setSpacing(0);

    // --- Page header ---
    auto* headerLayout = new QVBoxLayout();
    auto* title = new QLabel(QStringLiteral("Audio Routing"), this);
    title->setStyleSheet(QStringLiteral("font-size: 28px; font-weight: bold; color: white; margin-bottom: 4px;"));
    auto* subtitle = new QLabel(QStringLiteral("Manage per-application audio output assignments."), this);
    subtitle->setStyleSheet(QStringLiteral("font-size: 13px; color: #888888; margin-bottom: 20px;"));
    headerLayout->addWidget(title);
    headerLayout->addWidget(subtitle);
    outerLayout->addLayout(headerLayout);

    // --- Search + filter bar ---
    auto* searchRow = new QHBoxLayout();
    searchRow->setSpacing(10);

    m_searchBox = new QLineEdit(this);
    m_searchBox->setPlaceholderText(QStringLiteral("Search applications..."));
    connect(m_searchBox, &QLineEdit::textChanged, this, &AudioPage::onSearchOrFilterChanged);

    m_filterCombo = new QComboBox(this);
    m_filterCombo->setFixedWidth(120);
    m_filterCombo->addItem(QStringLiteral("All"));
    m_filterCombo->addItem(QStringLiteral("Active"));
    m_filterCombo->addItem(QStringLiteral("Inactive"));
    connect(m_filterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AudioPage::onSearchOrFilterChanged);

    searchRow->addWidget(m_searchBox, 1);
    searchRow->addWidget(m_filterCombo);
    outerLayout->addLayout(searchRow);
    outerLayout->addSpacing(16);

    // --- Two-column split ---
    auto* splitLayout = new QHBoxLayout();
    splitLayout->setSpacing(24);

    // LEFT: Sessions (65%)
    auto* sessionsContainer = new QWidget(this);
    auto* sessionsVLayout = new QVBoxLayout(sessionsContainer);
    sessionsVLayout->setContentsMargins(0, 0, 0, 0);
    sessionsVLayout->setSpacing(8);

    auto* sessionsHeaderRow = new QHBoxLayout();
    auto* sessionsTitle = new QLabel(QStringLiteral("AUDIO SESSIONS"), sessionsContainer);
    sessionsTitle->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: bold; color: #888888; letter-spacing: 1px;"));
    m_sessionCountLabel = new QLabel(QStringLiteral(""), sessionsContainer);
    m_sessionCountLabel->setStyleSheet(QStringLiteral("font-size: 11px; color: #888888;"));
    sessionsHeaderRow->addWidget(sessionsTitle);
    sessionsHeaderRow->addStretch();
    sessionsHeaderRow->addWidget(m_sessionCountLabel);
    sessionsVLayout->addLayout(sessionsHeaderRow);
    sessionsVLayout->addSpacing(4);

    auto* sessionsScroll = new QScrollArea(sessionsContainer);
    sessionsScroll->setWidgetResizable(true);
    auto* sessionsList = new QWidget(sessionsScroll);
    sessionsList->setStyleSheet(QStringLiteral("background-color: transparent;"));
    m_sessionsLayout = new QVBoxLayout(sessionsList);
    m_sessionsLayout->setContentsMargins(0, 0, 6, 0);
    m_sessionsLayout->setSpacing(10);
    sessionsScroll->setWidget(sessionsList);
    sessionsVLayout->addWidget(sessionsScroll);
    splitLayout->addWidget(sessionsContainer, 65);

    // RIGHT: Outputs (35%)
    auto* outputsContainer = new QWidget(this);
    auto* outputsVLayout = new QVBoxLayout(outputsContainer);
    outputsVLayout->setContentsMargins(0, 0, 0, 0);
    outputsVLayout->setSpacing(8);

    auto* outputsHeaderRow = new QHBoxLayout();
    auto* outputsTitle = new QLabel(QStringLiteral("AUDIO OUTPUTS"), outputsContainer);
    outputsTitle->setStyleSheet(QStringLiteral("font-size: 11px; font-weight: bold; color: #888888; letter-spacing: 1px;"));
    m_endpointCountLabel = new QLabel(QStringLiteral(""), outputsContainer);
    m_endpointCountLabel->setStyleSheet(QStringLiteral("font-size: 11px; color: #888888;"));
    outputsHeaderRow->addWidget(outputsTitle);
    outputsHeaderRow->addStretch();
    outputsHeaderRow->addWidget(m_endpointCountLabel);
    outputsVLayout->addLayout(outputsHeaderRow);
    outputsVLayout->addSpacing(4);

    auto* outputsScroll = new QScrollArea(outputsContainer);
    outputsScroll->setWidgetResizable(true);
    auto* outputsList = new QWidget(outputsScroll);
    outputsList->setStyleSheet(QStringLiteral("background-color: transparent;"));
    m_outputsLayout = new QVBoxLayout(outputsList);
    m_outputsLayout->setContentsMargins(0, 0, 6, 0);
    m_outputsLayout->setSpacing(8);
    outputsScroll->setWidget(outputsList);
    outputsVLayout->addWidget(outputsScroll);
    splitLayout->addWidget(outputsContainer, 35);

    outerLayout->addLayout(splitLayout, 1);

    // Wire routing controller signals
    connect(m_routingController, &RoutingController::routingCompleted,
            this, &AudioPage::onRoutingCompleted);
    connect(m_routingController, &RoutingController::resetCompleted,
            this, &AudioPage::onResetCompleted);
}

// ---------------------------------------------------------------------------
// Public slot: called every 2s by EnginePoller
// ---------------------------------------------------------------------------

void AudioPage::updateState(const EngineStatePayload& payload) {
    m_lastPayload = payload;
    updateCounts();
    renderSessions();
    renderOutputs();
}

// ---------------------------------------------------------------------------
// Filter helpers
// ---------------------------------------------------------------------------

QString AudioPage::currentSearchText() const {
    return m_searchBox ? m_searchBox->text().trimmed().toLower() : QString();
}

int AudioPage::currentFilterIndex() const {
    return m_filterCombo ? m_filterCombo->currentIndex() : 0;
}

bool AudioPage::sessionMatchesFilter(
    const hydra::windows::AudioSessionObservation& session,
    const QString& searchText,
    int filterIndex,
    const QString& resolvedName) const
{
    // State filter
    if (filterIndex == 1 && session.state != hydra::windows::AudioSessionState::Active)
        return false;
    if (filterIndex == 2 && session.state == hydra::windows::AudioSessionState::Active)
        return false;

    // Text filter
    if (!searchText.isEmpty()) {
        bool nameMatch = resolvedName.toLower().contains(searchText);
        bool pidMatch  = QString::number(session.processId).contains(searchText);
        if (!nameMatch && !pidMatch) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Counts
// ---------------------------------------------------------------------------

void AudioPage::updateCounts() {
    int totalSessions = static_cast<int>(m_lastPayload.audioSessions.size());
    int activeSessions = 0;
    for (const auto& s : m_lastPayload.audioSessions)
        if (s.state == hydra::windows::AudioSessionState::Active) activeSessions++;

    m_sessionCountLabel->setText(
        QString("%1 total / %2 active").arg(totalSessions).arg(activeSessions));

    int totalEp = static_cast<int>(m_lastPayload.audioEndpoints.size());
    int activeEp = 0;
    for (const auto& e : m_lastPayload.audioEndpoints)
        if (e.isAvailable()) activeEp++;

    m_endpointCountLabel->setText(
        QString("%1 total / %2 active").arg(totalEp).arg(activeEp));
}

// ---------------------------------------------------------------------------
// Endpoint combo population (active first, then separator, then inactive)
// ---------------------------------------------------------------------------

void AudioPage::populateEndpointCombo(
    QComboBox* combo,
    const std::vector<hydra::windows::AudioRenderEndpoint>& endpoints)
{
    combo->blockSignals(true);
    QString prevData = combo->currentData().toString();
    combo->clear();

    bool hasActive = false, hasInactive = false;
    for (const auto& ep : endpoints) {
        if (ep.isAvailable()) hasActive = true;
        else hasInactive = true;
    }

    if (hasActive) {
        combo->insertSeparator(combo->count()); // visual group: ACTIVE
        // Use a disabled item as group header
        combo->addItem(QStringLiteral("── ACTIVE OUTPUTS ──"), QStringLiteral("__header__"));
        auto* model = qobject_cast<QStandardItemModel*>(combo->model());
        if (model) {
            auto* hdr = model->item(combo->count() - 1);
            if (hdr) { hdr->setEnabled(false); hdr->setForeground(QColor("#888888")); }
        }
        for (const auto& ep : endpoints) {
            if (ep.isAvailable())
                combo->addItem(QStringLiteral("● ") + QString::fromStdWString(ep.friendlyName),
                               QString::fromStdWString(ep.endpointId));
        }
    }
    if (hasInactive) {
        combo->addItem(QStringLiteral("── OTHER OUTPUTS ──"), QStringLiteral("__header__"));
        auto* model = qobject_cast<QStandardItemModel*>(combo->model());
        if (model) {
            auto* hdr = model->item(combo->count() - 1);
            if (hdr) { hdr->setEnabled(false); hdr->setForeground(QColor("#888888")); }
        }
        for (const auto& ep : endpoints) {
            if (!ep.isAvailable())
                combo->addItem(QStringLiteral("○ ") + QString::fromStdWString(ep.friendlyName),
                               QString::fromStdWString(ep.endpointId));
        }
    }

    // Restore previous selection if it still exists
    int idx = combo->findData(prevData);
    if (idx >= 0) combo->setCurrentIndex(idx);
    else {
        // Select first non-header item
        for (int i = 0; i < combo->count(); i++) {
            if (combo->itemData(i).toString() != QStringLiteral("__header__")) {
                combo->setCurrentIndex(i);
                break;
            }
        }
    }
    combo->blockSignals(false);
}

// ---------------------------------------------------------------------------
// Build a new session card widget
// ---------------------------------------------------------------------------

void AudioPage::buildSessionCard(
    const hydra::windows::AudioSessionObservation& session,
    const QString& processName,
    const QPixmap& icon)
{
    AudioSessionCard card;
    card.pid = session.processId;
    card.creationIdentity = session.processIdentity
        ? session.processIdentity->creationIdentity : 0;
    card.state = session.state;
    card.endpointId = session.endpointId;
    card.displayName = session.displayName;

    auto* frame = new QFrame();
    frame->setObjectName(QStringLiteral("sessionCard"));
    frame->setContentsMargins(0, 0, 0, 0);

    auto* cardLayout = new QVBoxLayout(frame);
    cardLayout->setContentsMargins(16, 14, 16, 14);
    cardLayout->setSpacing(8);

    // --- Top row: icon + name + state badge ---
    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(10);

    if (!icon.isNull()) {
        auto* iconLabel = new QLabel(frame);
        iconLabel->setPixmap(icon);
        iconLabel->setFixedSize(24, 24);
        topRow->addWidget(iconLabel);
    }

    auto* nameLabel = new QLabel(processName, frame);
    nameLabel->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: bold; color: #FFFFFF;"));
    topRow->addWidget(nameLabel, 1);

    bool isActive = (session.state == hydra::windows::AudioSessionState::Active);
    card.stateLabel = new QLabel(stateText(session.state), frame);
    card.stateLabel->setStyleSheet(
        isActive
            ? QStringLiteral("font-size: 12px; font-weight: bold; color: #32D74B; background-color: #1A3A1A; padding: 3px 8px; border-radius: 4px;")
            : QStringLiteral("font-size: 12px; font-weight: bold; color: #888888; background-color: #2A2A2C; padding: 3px 8px; border-radius: 4px;"));
    topRow->addWidget(card.stateLabel);
    cardLayout->addLayout(topRow);

    // --- PID + Current output ---
    QString currentOutputName = resolveEndpointFriendlyName(
        session.endpointId, m_lastPayload.audioEndpoints);
    auto* metaLabel = new QLabel(
        QString("PID: %1").arg(session.processId), frame);
    metaLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #888888;"));
    cardLayout->addWidget(metaLabel);

    card.currentOutputLabel = new QLabel(
        QStringLiteral("Current Output: ") + currentOutputName, frame);
    card.currentOutputLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #AAAAAA;"));
    cardLayout->addWidget(card.currentOutputLabel);

    // --- Separator ---
    auto* sep = new QFrame(frame);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QStringLiteral("color: #2A2A2C; background-color: #2A2A2C; max-height: 1px;"));
    cardLayout->addWidget(sep);

    // --- Endpoint dropdown ---
    auto* routeRow = new QHBoxLayout();
    auto* routeToLabel = new QLabel(QStringLiteral("Route to:"), frame);
    routeToLabel->setStyleSheet(QStringLiteral("font-size: 13px; color: #CCCCCC;"));
    routeRow->addWidget(routeToLabel);

    card.endpointCombo = new QComboBox(frame);
    populateEndpointCombo(card.endpointCombo, m_lastPayload.audioEndpoints);
    routeRow->addWidget(card.endpointCombo, 1);
    cardLayout->addLayout(routeRow);

    // --- Buttons ---
    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(8);
    btnRow->addStretch();

    card.feedbackLabel = new QLabel(QStringLiteral(""), frame);
    card.feedbackLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #32D74B;"));
    card.feedbackLabel->setVisible(false);
    btnRow->addWidget(card.feedbackLabel);

    card.resetBtn = new QPushButton(QStringLiteral("Reset"), frame);
    card.resetBtn->setObjectName(QStringLiteral("resetBtn"));
    card.resetBtn->setFixedHeight(32);
    btnRow->addWidget(card.resetBtn);

    card.routeBtn = new QPushButton(QStringLiteral("Route Audio"), frame);
    card.routeBtn->setObjectName(QStringLiteral("routeBtn"));
    card.routeBtn->setFixedHeight(32);

    // Disable routing for PID 0 (system audio)
    if (session.processId == 0 || !session.processIdentity) {
        card.routeBtn->setEnabled(false);
        card.resetBtn->setEnabled(false);
        card.routeBtn->setToolTip(QStringLiteral("System audio sessions cannot be individually routed"));
    }

    btnRow->addWidget(card.routeBtn);
    cardLayout->addLayout(btnRow);

    // Wire buttons — capture pid and creationIdentity by value
    uint32_t pid = session.processId;
    uint64_t cid = card.creationIdentity;
    QComboBox* combo = card.endpointCombo;

    connect(card.routeBtn, &QPushButton::clicked, [this, pid, cid, combo]() {
        // Skip header items
        QString epId = combo->currentData().toString();
        if (epId.isEmpty() || epId == QStringLiteral("__header__")) return;
        onRouteRequested(pid, cid, epId);
    });
    connect(card.resetBtn, &QPushButton::clicked, [this, pid, cid]() {
        onResetRequested(pid, cid);
    });

    // Apply in-progress state if this PID is currently routing
    if (m_routingInProgress.value(pid, false)) {
        card.routeBtn->setEnabled(false);
        card.resetBtn->setEnabled(false);
        card.feedbackLabel->setText(QStringLiteral("Routing..."));
        card.feedbackLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #FF9F0A;"));
        card.feedbackLabel->setVisible(true);
    }

    card.frame = frame;
    m_sessionCards.append(card);
    m_sessionsLayout->addWidget(frame);
}

// ---------------------------------------------------------------------------
// Try to update an existing card in-place (avoids rebuild flicker)
// ---------------------------------------------------------------------------

bool AudioPage::tryUpdateExistingCard(
    const hydra::windows::AudioSessionObservation& session)
{
    for (auto& card : m_sessionCards) {
        if (card.pid != session.processId) continue;

        uint64_t currentCid = session.processIdentity ? session.processIdentity->creationIdentity : 0;
        if (card.creationIdentity != currentCid) {
            // PID reused by another process, must recreate card
            m_processNameCache.remove(card.pid);
            return false;
        }

        bool changed = false;

        // Update state badge
        if (card.state != session.state) {
            card.state = session.state;
            changed = true;
            bool isActive = (session.state == hydra::windows::AudioSessionState::Active);
            card.stateLabel->setText(stateText(session.state));
            card.stateLabel->setStyleSheet(
                isActive
                    ? QStringLiteral("font-size: 12px; font-weight: bold; color: #32D74B; background-color: #1A3A1A; padding: 3px 8px; border-radius: 4px;")
                    : QStringLiteral("font-size: 12px; font-weight: bold; color: #888888; background-color: #2A2A2C; padding: 3px 8px; border-radius: 4px;"));
        }

        // Update current output
        if (card.endpointId != session.endpointId) {
            card.endpointId = session.endpointId;
            changed = true;
            QString currentOutputName = resolveEndpointFriendlyName(
                session.endpointId, m_lastPayload.audioEndpoints);
            card.currentOutputLabel->setText(
                QStringLiteral("Current Output: ") + currentOutputName);
        }

        // Track displayName change
        if (card.displayName != session.displayName) {
            card.displayName = session.displayName;
            changed = true;
        }

        // Always ensure dropdown and route button disabled state is correct if routing is in progress
        if (m_routingInProgress.value(card.pid, false)) {
            card.routeBtn->setEnabled(false);
            card.resetBtn->setEnabled(false);
            card.endpointCombo->setEnabled(false);
        } else {
            if (card.pid != 0 && session.processIdentity) {
                card.routeBtn->setEnabled(true);
                card.resetBtn->setEnabled(true);
                card.endpointCombo->setEnabled(true);
            }
        }

        if (changed) {
            // Re-populate combo to ensure endpoint states (active/inactive) are fresh
            populateEndpointCombo(card.endpointCombo, m_lastPayload.audioEndpoints);
        }

        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Render sessions
// ---------------------------------------------------------------------------

void AudioPage::renderSessions() {
    QString searchText = currentSearchText();
    int filterIdx = currentFilterIndex();

    // Determine which PIDs should be visible after filtering
    QSet<uint32_t> visiblePids;
    struct SessionEntry {
        hydra::windows::AudioSessionObservation session;
        QString name;
        QPixmap icon;
    };
    QList<SessionEntry> entries;

    for (const auto& session : m_lastPayload.audioSessions) {
        if (session.processId == 0) continue; // Skip PID 0 system sessions

        QString name = resolveProcessNameCached(session.processId);
        if (!sessionMatchesFilter(session, searchText, filterIdx, name)) continue;

        visiblePids.insert(session.processId);
        entries.append({session, name, {}});
    }

    // Determine which existing cards are no longer needed
    QSet<uint32_t> existingPids;
    for (const auto& card : m_sessionCards)
        existingPids.insert(card.pid);

    // Remove cards for PIDs that are no longer visible
    QSet<uint32_t> toRemove = existingPids - visiblePids;
    if (!toRemove.isEmpty()) {
        m_sessionCards.removeIf([&toRemove, this](const AudioSessionCard& card) {
            if (toRemove.contains(card.pid)) {
                if (card.frame) card.frame->deleteLater();
                return true;
            }
            return false;
        });
    }

    // For each visible session: update existing card or build new one
    // Resolve icons lazily only for new cards
    for (auto& entry : entries) {
        bool updated = tryUpdateExistingCard(entry.session);
        if (!updated) {
            // New card — resolve icon now
            entry.icon = resolveProcessIcon(entry.session.processId);
            buildSessionCard(entry.session, entry.name, entry.icon);
        }
    }

    // Show empty state
    bool hasCards = !m_sessionCards.isEmpty();
    // Remove any existing "no sessions" label if we now have cards
    // (The label is not tracked — we just check layout item count vs card count)
    // Simple approach: if no entries after filter, show message
    if (entries.isEmpty()) {
        // Clear all existing cards
        for (auto& card : m_sessionCards) {
            if (card.frame) card.frame->deleteLater();
        }
        m_sessionCards.clear();

        // Add empty state label if not already there
        if (m_sessionsLayout->count() == 0) {
            int nonSystemSessionCount = 0;
            for (const auto& s : m_lastPayload.audioSessions) {
                if (s.processId != 0) nonSystemSessionCount++;
            }

            QString emptyText;
            if (nonSystemSessionCount == 0) {
                emptyText = QStringLiteral("No audio applications detected.");
            } else if (searchText.isEmpty() && filterIdx == 1) { // 1 = Active
                emptyText = QString("%1 sessions detected — 0 currently active.").arg(nonSystemSessionCount);
            } else if (!searchText.isEmpty()) {
                emptyText = QStringLiteral("No sessions match the current filter.");
            } else {
                emptyText = QStringLiteral("No audio applications detected.");
            }

            auto* empty = new QLabel(emptyText, nullptr);
            empty->setStyleSheet(QStringLiteral("color: #555555; font-size: 14px;"));
            empty->setAlignment(Qt::AlignCenter);
            m_sessionsLayout->addWidget(empty);
            m_sessionsLayout->addStretch();
        }
    } else {
        // Remove any empty-state label (first widget if it's a QLabel with no objectName)
        if (m_sessionsLayout->count() > 0 && m_sessionCards.isEmpty()) {
            while (QLayoutItem* item = m_sessionsLayout->takeAt(0)) {
                if (item->widget()) item->widget()->deleteLater();
                delete item;
            }
        }

        // Ensure stretch at bottom
        // Remove trailing stretch if present, then re-add
        int last = m_sessionsLayout->count() - 1;
        if (last >= 0 && m_sessionsLayout->itemAt(last)->spacerItem()) {
            delete m_sessionsLayout->takeAt(last);
        }
        m_sessionsLayout->addStretch();
    }
    (void)hasCards;
}

// ---------------------------------------------------------------------------
// Render endpoints
// ---------------------------------------------------------------------------

void AudioPage::renderOutputs() {
    // Full rebuild for outputs (there are at most ~20, rarely changes)
    while (QLayoutItem* item = m_outputsLayout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    // Sort: active first
    auto endpoints = m_lastPayload.audioEndpoints;
    std::stable_sort(endpoints.begin(), endpoints.end(),
        [](const hydra::windows::AudioRenderEndpoint& a,
           const hydra::windows::AudioRenderEndpoint& b) {
            return a.isAvailable() > b.isAvailable();
        });

    if (endpoints.empty()) {
        auto* lbl = new QLabel(QStringLiteral("No audio outputs detected."));
        lbl->setStyleSheet(QStringLiteral("color: #555555; font-size: 13px;"));
        m_outputsLayout->addWidget(lbl);
        m_outputsLayout->addStretch();
        return;
    }

    for (const auto& ep : endpoints) {
        auto* frame = new QFrame();
        frame->setObjectName(QStringLiteral("endpointCard"));

        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(12, 10, 12, 10);
        fl->setSpacing(3);

        bool active = ep.isAvailable();
        auto* nameLabel = new QLabel(QString::fromStdWString(ep.friendlyName), frame);
        nameLabel->setStyleSheet(
            active
                ? QStringLiteral("font-size: 14px; font-weight: bold; color: #FFFFFF;")
                : QStringLiteral("font-size: 14px; font-weight: bold; color: #666666;"));
        nameLabel->setWordWrap(true);
        fl->addWidget(nameLabel);

        QString stateStr;
        QString stateColor;
        switch (ep.state) {
            case hydra::windows::AudioEndpointState::Active:
                stateStr = QStringLiteral("● Active");
                stateColor = QStringLiteral("#32D74B");
                break;
            case hydra::windows::AudioEndpointState::Disabled:
                stateStr = QStringLiteral("○ Disabled");
                stateColor = QStringLiteral("#FF453A");
                break;
            case hydra::windows::AudioEndpointState::Unplugged:
                stateStr = QStringLiteral("○ Unplugged");
                stateColor = QStringLiteral("#FF9F0A");
                break;
            case hydra::windows::AudioEndpointState::NotPresent:
                stateStr = QStringLiteral("○ Not Present");
                stateColor = QStringLiteral("#555555");
                break;
            default:
                stateStr = QStringLiteral("? Unknown");
                stateColor = QStringLiteral("#888888");
                break;
        }

        auto* stateLabel = new QLabel(stateStr, frame);
        stateLabel->setStyleSheet(
            QString("font-size: 12px; color: %1;").arg(stateColor));
        fl->addWidget(stateLabel);

        m_outputsLayout->addWidget(frame);
    }

    m_outputsLayout->addStretch();
}

// ---------------------------------------------------------------------------
// Filter changed (search box or combo)
// ---------------------------------------------------------------------------

void AudioPage::onSearchOrFilterChanged() {
    // Clear all existing session cards and rebuild with new filter
    for (auto& card : m_sessionCards) {
        if (card.frame) card.frame->deleteLater();
    }
    m_sessionCards.clear();

    while (QLayoutItem* item = m_sessionsLayout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    renderSessions();
}

// ---------------------------------------------------------------------------
// Routing actions
// ---------------------------------------------------------------------------

void AudioPage::onRouteRequested(uint32_t pid, uint64_t /*creationIdentity*/, const QString& endpointId) {
    if (m_routingInProgress.value(pid, false)) return; // Prevent double-click
    m_routingInProgress[pid] = true;

    // Update card UI immediately
    for (auto& card : m_sessionCards) {
        if (card.pid == pid) {
            card.routeBtn->setEnabled(false);
            card.resetBtn->setEnabled(false);
            card.endpointCombo->setEnabled(false);
            card.feedbackLabel->setText(QStringLiteral("Routing..."));
            card.feedbackLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #FF9F0A;"));
            card.feedbackLabel->setVisible(true);
            break;
        }
    }

    m_routingController->requestRoute(pid, endpointId);
}

void AudioPage::onResetRequested(uint32_t pid, uint64_t /*creationIdentity*/) {
    if (m_routingInProgress.value(pid, false)) return;
    m_routingInProgress[pid] = true;

    for (auto& card : m_sessionCards) {
        if (card.pid == pid) {
            card.routeBtn->setEnabled(false);
            card.resetBtn->setEnabled(false);
            card.endpointCombo->setEnabled(false);
            card.feedbackLabel->setText(QStringLiteral("Resetting..."));
            card.feedbackLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #FF9F0A;"));
            card.feedbackLabel->setVisible(true);
            break;
        }
    }

    m_routingController->requestReset(pid);
}

void AudioPage::onRoutingCompleted(uint32_t pid, bool success, const QString& errorMessage) {
    m_routingInProgress[pid] = false;

    for (auto& card : m_sessionCards) {
        if (card.pid == pid) {
            card.routeBtn->setEnabled(true);
            card.resetBtn->setEnabled(true);
            card.endpointCombo->setEnabled(true);
            if (success) {
                QString epId = card.endpointCombo->currentData().toString();
                QString epName = resolveEndpointFriendlyName(epId.toStdWString(), m_lastPayload.audioEndpoints);
                card.feedbackLabel->setText(QStringLiteral("✓ Routed to ") + epName);
                card.feedbackLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #32D74B;"));
            } else {
                card.feedbackLabel->setText(QStringLiteral("✕ Routing failed\n") + errorMessage);
                card.feedbackLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #FF453A;"));
            }
            card.feedbackLabel->setVisible(true);
            // Auto-hide feedback after 5s
            QTimer::singleShot(5000, card.feedbackLabel, [lbl = card.feedbackLabel]() {
                if (lbl) lbl->setVisible(false);
            });
            break;
        }
    }
}

void AudioPage::onResetCompleted(uint32_t pid, bool success, const QString& errorMessage) {
    m_routingInProgress[pid] = false;

    for (auto& card : m_sessionCards) {
        if (card.pid == pid) {
            card.routeBtn->setEnabled(true);
            card.resetBtn->setEnabled(true);
            card.endpointCombo->setEnabled(true);
            if (success) {
                card.feedbackLabel->setText(QStringLiteral("✓ Routing reset"));
                card.feedbackLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #32D74B;"));
            } else {
                card.feedbackLabel->setText(QStringLiteral("✕ Reset failed\n") + errorMessage);
                card.feedbackLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #FF453A;"));
            }
            card.feedbackLabel->setVisible(true);
            QTimer::singleShot(5000, card.feedbackLabel, [lbl = card.feedbackLabel]() {
                if (lbl) lbl->setVisible(false);
            });
            break;
        }
    }
}

} // namespace hydra::ui
