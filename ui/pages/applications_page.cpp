#include "ui/pages/applications_page.hpp"

#include <QByteArray>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>

#include <utility>

namespace hydra::ui {
namespace {

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

std::string utf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

} // namespace

ApplicationsPage::ApplicationsPage(
    std::shared_ptr<HostControlClient> hostControl,
    QWidget* parent)
    : QWidget(parent),
      m_hostControl(std::move(hostControl)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(20);

    auto* title = new QLabel("Applications", this);
    title->setStyleSheet(
        "font-size: 28px; font-weight: bold; color: #F5F5F5; "
        "font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);

    auto* subtitle = new QLabel(
        "Launch a host-owned custom executable or inspect observable application sessions",
        this);
    subtitle->setStyleSheet(
        "font-size: 14px; color: #B5B5B5; font-family: 'Segoe UI', sans-serif; "
        "margin-bottom: 8px;");
    layout->addWidget(subtitle);

    auto* launchFrame = new QFrame(this);
    launchFrame->setMaximumWidth(900);
    launchFrame->setStyleSheet(
        "QFrame { background-color: #151515; border-radius: 8px; "
        "border: 1px solid #292929; padding: 16px; }"
        "QLabel { border: none; }");
    auto* launchLayout = new QVBoxLayout(launchFrame);
    launchLayout->setContentsMargins(0, 0, 0, 0);
    launchLayout->setSpacing(10);

    auto* launchTitle = new QLabel("HOST-OWNED CUSTOM EXECUTABLE", launchFrame);
    launchTitle->setStyleSheet(
        "font-size: 12px; font-weight: bold; color: #777777; border: none;");
    launchLayout->addWidget(launchTitle);

    auto* targetRow = new QHBoxLayout();
    auto* seatLabel = new QLabel("Seat", launchFrame);
    seatLabel->setFixedWidth(70);
    seatLabel->setStyleSheet("font-size: 13px; color: #B5B5B5; border: none;");
    targetRow->addWidget(seatLabel);

    m_seatCombo = new QComboBox(launchFrame);
    m_seatCombo->addItem("Seat 1", QVariant::fromValue(1u));
    m_seatCombo->addItem("Seat 2", QVariant::fromValue(2u));
    m_seatCombo->setFixedWidth(120);
    m_seatCombo->setStyleSheet(
        "QComboBox { padding: 4px 8px; background-color: #101010; "
        "color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; "
        "height: 32px; }");
    targetRow->addWidget(m_seatCombo);

    m_executableEdit = new QLineEdit(launchFrame);
    m_executableEdit->setPlaceholderText("C:\\Games\\Example\\game.exe");
    m_executableEdit->setStyleSheet(
        "QLineEdit { padding: 6px 8px; background-color: #101010; "
        "color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; }"
        "QLineEdit:focus { border: 1px solid #E10600; }");
    targetRow->addWidget(m_executableEdit, 1);

    auto* browseButton = new QPushButton("Browse", launchFrame);
    browseButton->setStyleSheet(secondaryButtonStyle());
    targetRow->addWidget(browseButton);
    launchLayout->addLayout(targetRow);

    auto* argumentsRow = new QHBoxLayout();
    auto* argumentsLabel = new QLabel("Arguments", launchFrame);
    argumentsLabel->setFixedWidth(70);
    argumentsLabel->setStyleSheet(
        "font-size: 13px; color: #B5B5B5; border: none;");
    argumentsRow->addWidget(argumentsLabel);

    m_argumentsEdit = new QLineEdit(launchFrame);
    m_argumentsEdit->setPlaceholderText("Optional command-line arguments");
    m_argumentsEdit->setStyleSheet(
        "QLineEdit { padding: 6px 8px; background-color: #101010; "
        "color: #F5F5F5; border: 1px solid #333333; border-radius: 6px; }"
        "QLineEdit:focus { border: 1px solid #E10600; }");
    argumentsRow->addWidget(m_argumentsEdit, 1);
    launchLayout->addLayout(argumentsRow);

    auto* actionRow = new QHBoxLayout();
    actionRow->addStretch();

    m_stopButton = new QPushButton("Stop Seat Game", launchFrame);
    m_stopButton->setStyleSheet(secondaryButtonStyle());
    actionRow->addWidget(m_stopButton);

    m_launchButton = new QPushButton("Launch on Seat", launchFrame);
    m_launchButton->setStyleSheet(primaryButtonStyle());
    actionRow->addWidget(m_launchButton);
    launchLayout->addLayout(actionRow);

    m_launchFeedback = new QLabel("", launchFrame);
    m_launchFeedback->setWordWrap(true);
    m_launchFeedback->setVisible(false);
    launchLayout->addWidget(m_launchFeedback);

    layout->addWidget(launchFrame);

    auto* listTitle = new QLabel("OBSERVED APPLICATION SESSIONS", this);
    listTitle->setStyleSheet(
        "font-size: 12px; font-weight: bold; color: #777777; "
        "margin-top: 4px;");
    layout->addWidget(listTitle);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet(
        "QScrollArea { border: none; background-color: transparent; }");

    auto* listContainer = new QWidget(scrollArea);
    listContainer->setStyleSheet("background-color: transparent;");
    m_listLayout = new QVBoxLayout(listContainer);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(8);
    m_listLayout->addStretch();

    scrollArea->setWidget(listContainer);
    layout->addWidget(scrollArea, 1);

    connect(
        browseButton,
        &QPushButton::clicked,
        this,
        &ApplicationsPage::onBrowseExecutable);
    connect(
        m_launchButton,
        &QPushButton::clicked,
        this,
        &ApplicationsPage::onLaunchRequested);
    connect(
        m_stopButton,
        &QPushButton::clicked,
        this,
        &ApplicationsPage::onStopRequested);
    connect(
        m_seatCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int) { refreshLaunchControls(); });
    connect(
        m_executableEdit,
        &QLineEdit::textChanged,
        this,
        [this](const QString&) { refreshLaunchControls(); });

    refreshLaunchControls();
}

std::uint32_t ApplicationsPage::selectedSeatId() const noexcept {
    if (!m_seatCombo) return 0;
    bool ok = false;
    const auto value = m_seatCombo->currentData().toUInt(&ok);
    return ok ? value : 0;
}

const hydra::hostipc::SeatSnapshot*
ApplicationsPage::selectedSeatSnapshot() const noexcept {
    if (!m_lastPayload.hostSnapshot) return nullptr;
    const auto seatId = selectedSeatId();
    if (seatId == 0 || seatId > m_lastPayload.hostSnapshot->seats.size()) {
        return nullptr;
    }
    return &m_lastPayload.hostSnapshot->seats[seatId - 1u];
}

void ApplicationsPage::setLaunchFeedback(const QString& text, bool error) {
    if (!m_launchFeedback) return;
    m_launchFeedback->setText(text);
    m_launchFeedback->setStyleSheet(
        error
            ? "font-size: 12px; color: #E10600; border: none;"
            : "font-size: 12px; color: #B5B5B5; border: none;");
    m_launchFeedback->setVisible(!text.isEmpty());
}

void ApplicationsPage::refreshLaunchControls() {
    const auto seatId = selectedSeatId();
    const auto* seat = selectedSeatSnapshot();
    const bool hostReady =
        m_lastPayload.hostConnected && seat != nullptr && m_hostControl;
    const bool leaseAvailable =
        seat != nullptr &&
        (!seat->uiLeaseActive ||
         (m_hostControl && m_hostControl->ownsUiLease(seatId)));
    const bool hasExecutable =
        m_executableEdit && !m_executableEdit->text().trimmed().isEmpty();

    if (m_launchButton) {
        m_launchButton->setEnabled(
            hostReady && leaseAvailable && !seat->gameLeaseActive &&
            hasExecutable);
    }
    if (m_stopButton) {
        m_stopButton->setEnabled(
            hostReady && leaseAvailable && seat->gameLeaseActive);
    }
}

void ApplicationsPage::onBrowseExecutable() {
    const QString selected = QFileDialog::getOpenFileName(
        this,
        "Select game executable",
        {},
        "Windows applications (*.exe);;All files (*.*)");
    if (!selected.isEmpty()) {
        m_executableEdit->setText(selected);
    }
}

void ApplicationsPage::onLaunchRequested() {
    if (!m_hostControl) {
        setLaunchFeedback("Canonical host control is unavailable.", true);
        return;
    }

    const auto seatId = selectedSeatId();
    const QString path = m_executableEdit->text().trimmed();
    const QFileInfo file(path);
    if (seatId == 0 || path.isEmpty() || !file.isAbsolute() ||
        !file.exists() || !file.isFile()) {
        setLaunchFeedback(
            "Select an existing absolute executable path before launching.",
            true);
        return;
    }

    std::string error;
    const auto result = m_hostControl->launchGame(
        seatId,
        utf8(file.fileName()),
        utf8(file.absoluteFilePath()),
        utf8(m_argumentsEdit->text()),
        utf8(file.absolutePath()),
        &error);
    if (!result) {
        setLaunchFeedback(
            error.empty()
                ? "The canonical host rejected the launch request."
                : QString::fromStdString(error),
            true);
        return;
    }

    m_lastPayload.hostSnapshot = *result;
    setLaunchFeedback(
        QString("Seat %1 launch accepted by hydra_host.").arg(seatId),
        false);
    refreshLaunchControls();
}

void ApplicationsPage::onStopRequested() {
    if (!m_hostControl) {
        setLaunchFeedback("Canonical host control is unavailable.", true);
        return;
    }

    const auto seatId = selectedSeatId();
    if (seatId == 0) {
        setLaunchFeedback("Select Seat 1 or Seat 2.", true);
        return;
    }

    std::string error;
    const auto result = m_hostControl->stopGame(seatId, &error);
    if (!result) {
        setLaunchFeedback(
            error.empty()
                ? "The canonical host could not stop this Seat safely."
                : QString::fromStdString(error),
            true);
        return;
    }

    m_lastPayload.hostSnapshot = *result;
    setLaunchFeedback(
        QString("Seat %1 game stopped and authority released.").arg(seatId),
        false);
    refreshLaunchControls();
}

QString ApplicationsPage::getAssignedSeat(
    const std::optional<hydra::runtime::ProcessIdentity>& identity,
    const std::optional<hydra::hostipc::HostSnapshot>& hostSnapshot) {
    if (!identity || !identity->valid() || !hostSnapshot) return "Unassigned";

    for (const auto& seat : hostSnapshot->seats) {
        if (seat.processOwned &&
            seat.processId == identity->pid &&
            seat.processCreationIdentity == identity->creationIdentity) {
            return QString("Seat %1").arg(seat.seatId);
        }
    }
    return "Unassigned";
}

void ApplicationsPage::updateState(const EngineStatePayload& payload) {
    m_lastPayload = payload;
    refreshLaunchControls();

    while (QLayoutItem* item = m_listLayout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (payload.audioSessionError) {
        auto* err = new QLabel(
            "Failed to enumerate audio sessions (used to track applications).");
        err->setStyleSheet(
            "color: #E10600; font-size: 13px; font-weight: bold;");
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
        frame->setStyleSheet(
            "background-color: #151515; border-radius: 6px; "
            "border: 1px solid #292929; padding: 12px 16px;");
        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(8);

        auto* nameLabel = new QLabel(
            session.displayName
                ? QString::fromStdWString(*session.displayName)
                : "Unknown",
            frame);
        nameLabel->setStyleSheet(
            "font-size: 15px; font-weight: bold; color: #F5F5F5; "
            "border: none;");
        fl->addWidget(nameLabel);

        const QString creationIdStr = session.processIdentity
            ? QString::number(session.processIdentity->creationIdentity)
            : "N/A";

        auto* pidLabel = new QLabel(
            QString("PID %1    CID %2")
                .arg(session.processId)
                .arg(creationIdStr),
            frame);
        pidLabel->setStyleSheet(
            "font-size: 12px; color: #777777; "
            "font-family: 'Consolas', monospace; border: none;");
        fl->addWidget(pidLabel);

        auto* bottomLayout = new QHBoxLayout();

        const QString stateText =
            session.state == hydra::windows::AudioSessionState::Active
                ? "● Active"
                : "○ Inactive";
        const QString stateColor =
            session.state == hydra::windows::AudioSessionState::Active
                ? "#E10600"
                : "#777777";
        auto* stateLabel = new QLabel(stateText, frame);
        stateLabel->setStyleSheet(
            QString(
                "font-size: 13px; font-weight: bold; color: %1; border: none;")
                .arg(stateColor));
        stateLabel->setFixedWidth(100);
        bottomLayout->addWidget(stateLabel);

        const QString seatName =
            getAssignedSeat(session.processIdentity, payload.hostSnapshot);
        auto* seatLabel =
            new QLabel(QString("Seat: %1").arg(seatName), frame);
        seatLabel->setStyleSheet(
            "font-size: 13px; color: #B5B5B5; border: none;");
        seatLabel->setFixedWidth(120);
        bottomLayout->addWidget(seatLabel);

        const QString audioStr = session.endpointId.empty()
            ? "Unassigned"
            : QString::fromStdWString(session.endpointId);
        auto* audioLabel =
            new QLabel(QString("Audio: %1").arg(audioStr), frame);
        audioLabel->setStyleSheet(
            "font-size: 13px; color: #B5B5B5; border: none;");
        bottomLayout->addWidget(audioLabel);

        bottomLayout->addStretch();
        fl->addLayout(bottomLayout);

        m_listLayout->addWidget(frame);
    }

    m_listLayout->addStretch();
}

} // namespace hydra::ui
