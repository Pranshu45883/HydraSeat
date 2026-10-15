#include "ui/pages/seats_page.hpp"
#include "ui/application_library.hpp"
#include "ui/ui_settings.hpp"

#include <QByteArray>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QSettings>
#include <QVariant>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace hydra::ui {
namespace {

QSettings settingsStore() {
    return QSettings(
        QSettings::NativeFormat,
        QSettings::UserScope,
        QStringLiteral("HydraSeat"),
        QStringLiteral("HydraSeat"));
}

QString seatApplicationKey(std::uint32_t seatId) {
    return QStringLiteral("seats/%1/applicationPath").arg(seatId);
}

QString seatPreferredAudioKey(std::uint32_t seatId) {
    return QStringLiteral("seats/%1/preferredAudioEndpoint").arg(seatId);
}

QString shortStableId(const QString& id) {
    if (id.size() <= 10) return id;
    return QStringLiteral("…%1").arg(id.right(9));
}

QString providerDisplayName(const std::string& providerId) {
    if (providerId == "steam") return QStringLiteral("Steam");
    if (providerId == "custom") return QStringLiteral("Custom");
    return QString::fromUtf8(providerId.c_str());
}

QString browseApplicationSentinel() {
    return QStringLiteral("hydraseat://browse-application");
}

struct SeatHostTaskResult {
    std::optional<hydra::hostipc::HostSnapshot> snapshot;
    std::string error;
};

struct SeatAudioTaskResult {
    std::optional<hydra::hostipc::AudioMutationStatus> status;
    std::string error;
};

struct SeatConfigureTaskResult {
    std::optional<hydra::hostipc::SeatHardwareAssignment> assignment;
    std::optional<hydra::hostipc::HostSnapshot> snapshot;
    std::string error;
    bool pairingRequested{false};
};

std::string toUtf8String(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(
        bytes.constData(),
        static_cast<std::size_t>(bytes.size()));
}

} // namespace

SeatsPage::SeatsPage(
    std::shared_ptr<HostControlClient> hostControl,
    QWidget* parent)
    : QWidget(parent), m_hostControl(std::move(hostControl)) {

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");
    auto* container = new QWidget();
    container->setStyleSheet("background-color: transparent;");
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(24);

    auto* title = new QLabel("Seats Management", container);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5; font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);

    auto* subtitle = new QLabel("Configure hardware assignments and application states", container);
    subtitle->setStyleSheet("font-size: 14px; color: #B5B5B5; font-family: 'Segoe UI', sans-serif; margin-bottom: 8px;");
    layout->addWidget(subtitle);

    auto* seatsLayout = new QHBoxLayout();
    seatsLayout->setSpacing(16);

    seatsLayout->addWidget(buildSeat(1, m_seat1));
    seatsLayout->addWidget(buildSeat(2, m_seat2));
    seatsLayout->addStretch();

    layout->addLayout(seatsLayout);
    layout->addStretch();
    scrollArea->setWidget(container);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(scrollArea);
}

QWidget* SeatsPage::buildSeat(std::uint32_t seatId, SeatWidgets& w) {
    auto* frame = new QFrame(this);
    frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 20px; border: 1px solid #292929;");
    frame->setMinimumWidth(400);
    frame->setMaximumWidth(500);
    
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(16);

    auto* headerLayout = new QHBoxLayout();
    auto* title = new QLabel(QString("SEAT %1").arg(seatId), frame);
    title->setStyleSheet("font-size: 16px; font-weight: bold; color: #F5F5F5; border: none;");
    w.stateBadge = new QLabel("Available", frame);
    w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-left: 12px;");
    headerLayout->addWidget(title);
    headerLayout->addWidget(w.stateBadge);
    headerLayout->addStretch();
    layout->addLayout(headerLayout);

    w.runtimeLabel = new QLabel(frame);
    w.runtimeLabel->setStyleSheet(
        "font-size: 12px; color: #B5B5B5; border: none; margin-top: -8px;");
    w.runtimeLabel->setWordWrap(true);
    w.runtimeLabel->setVisible(false);
    layout->addWidget(w.runtimeLabel);

    auto createRow = [&](const QString& labelText, QComboBox*& combo) {
        auto* l = new QLabel(labelText, frame);
        l->setStyleSheet("font-size: 11px; font-weight: bold; color: #777777; margin-bottom: 2px; border: none; text-transform: uppercase;");
        combo = new QComboBox(frame);
        combo->setStyleSheet("QComboBox { background-color: #0A0A0A; color: #F5F5F5; border: 1px solid #333333; border-radius: 4px; padding: 4px 8px; font-size: 13px; } QComboBox::drop-down { border: none; } QComboBox:disabled { color: #777777; background-color: #151515; }");
        layout->addWidget(l);
        layout->addWidget(combo);
    };

    createRow("APPLICATION", w.appCombo);
    createRow("DISPLAY", w.displayCombo);
    createRow("KEYBOARD", w.keyboardCombo);
    w.keyboardIdentifyBtn = new QPushButton("Identify by key press", frame);
    w.keyboardIdentifyBtn->setStyleSheet(
        "QPushButton { background-color: #202020; color: #B5B5B5; border: 1px solid #333333; "
        "border-radius: 4px; padding: 5px 10px; font-size: 12px; } "
        "QPushButton:hover { background-color: #292929; color: #F5F5F5; }");
    layout->addWidget(w.keyboardIdentifyBtn);
    createRow("MOUSE", w.mouseCombo);
    w.mouseIdentifyBtn = new QPushButton("Identify by movement", frame);
    w.mouseIdentifyBtn->setStyleSheet(
        "QPushButton { background-color: #202020; color: #B5B5B5; border: 1px solid #333333; "
        "border-radius: 4px; padding: 5px 10px; font-size: 12px; } "
        "QPushButton:hover { background-color: #292929; color: #F5F5F5; }");
    layout->addWidget(w.mouseIdentifyBtn);
    
    auto* ctrlLabel = new QLabel("CONTROLLER", frame);
    ctrlLabel->setStyleSheet("font-size: 11px; font-weight: bold; color: #777777; margin-bottom: 2px; border: none; text-transform: uppercase;");
    layout->addWidget(ctrlLabel);
    
    auto* ctrlLayout = new QHBoxLayout();
    w.ctrlPhysCombo = new QComboBox(frame);
    w.ctrlSrcCombo = new QComboBox(frame);
    w.ctrlPhysCombo->setStyleSheet("QComboBox { background-color: #0A0A0A; color: #F5F5F5; border: 1px solid #333333; border-radius: 4px; padding: 4px 8px; font-size: 13px; } QComboBox::drop-down { border: none; } QComboBox:disabled { color: #777777; background-color: #151515; }");
    w.ctrlSrcCombo->setStyleSheet("QComboBox { background-color: #0A0A0A; color: #F5F5F5; border: 1px solid #333333; border-radius: 4px; padding: 4px 8px; font-size: 13px; } QComboBox::drop-down { border: none; } QComboBox:disabled { color: #777777; background-color: #151515; }");
    w.ctrlSrcCombo->setToolTip(
        "XInput source is detected from a controller button press; manual slot selection is disabled.");
    ctrlLayout->addWidget(w.ctrlPhysCombo);
    ctrlLayout->addWidget(w.ctrlSrcCombo);
    layout->addLayout(ctrlLayout);

    createRow("AUDIO", w.audioCombo);

    auto* btnLayout = new QHBoxLayout();
    
    auto styleBtnPrimary = "QPushButton { background-color: #E10600; color: #F5F5F5; font-size: 14px; font-weight: bold; padding: 8px 16px; border-radius: 4px; border: none; } QPushButton:hover { background-color: #FF0A04; } QPushButton:disabled { background-color: #333333; color: #777777; }";
    auto styleBtnSecondary = "QPushButton { background-color: #292929; color: #F5F5F5; font-size: 14px; font-weight: bold; padding: 8px 16px; border-radius: 4px; border: 1px solid #333333; } QPushButton:hover { background-color: #333333; } QPushButton:disabled { background-color: #151515; color: #777777; border: 1px solid #222222; }";

    w.configureBtn = new QPushButton("Configure Seat", frame);
    w.configureBtn->setStyleSheet(styleBtnSecondary);
    w.launchBtn = new QPushButton("Launch", frame);
    w.launchBtn->setStyleSheet(styleBtnPrimary);
    w.stopBtn = new QPushButton("Stop", frame);
    w.stopBtn->setStyleSheet(styleBtnSecondary);
    w.reconfigureBtn = new QPushButton("Reconfigure", frame);
    w.reconfigureBtn->setStyleSheet(styleBtnSecondary);

    btnLayout->addWidget(w.configureBtn);
    btnLayout->addWidget(w.launchBtn);
    btnLayout->addWidget(w.stopBtn);
    btnLayout->addWidget(w.reconfigureBtn);
    layout->addLayout(btnLayout);

    w.feedbackLabel = new QLabel(frame);
    w.feedbackLabel->setStyleSheet("font-size: 12px; color: #B5B5B5; border: none; margin-top: 8px;");
    w.feedbackLabel->setWordWrap(true);
    w.feedbackLabel->setVisible(false);
    layout->addWidget(w.feedbackLabel);

    connect(
        w.appCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this, seatId](int) { onApplicationSelectionChanged(seatId); });
    connect(
        w.ctrlPhysCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this, seatId](int) { onControllerSelectionChanged(seatId); });
    connect(w.keyboardIdentifyBtn, &QPushButton::clicked, this, [this, seatId]() {
        onIdentifyInput(seatId, true);
    });
    connect(w.mouseIdentifyBtn, &QPushButton::clicked, this, [this, seatId]() {
        onIdentifyInput(seatId, false);
    });
    connect(
        w.audioCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this, seatId](int) { onAudioSelectionChanged(seatId); });
    connect(w.configureBtn, &QPushButton::clicked, this, [this, seatId]() { onConfigureRequested(seatId); });
    connect(w.launchBtn, &QPushButton::clicked, this, [this, seatId]() { onLaunchRequested(seatId); });
    connect(w.stopBtn, &QPushButton::clicked, this, [this, seatId]() { onStopRequested(seatId); });
    connect(w.reconfigureBtn, &QPushButton::clicked, this, [this, seatId]() { onConfigureRequested(seatId); });

    return frame;
}

const hydra::hostipc::SeatSnapshot* SeatsPage::seatSnapshot(
    std::uint32_t seatId) const noexcept {
    if (!m_lastPayload.hostSnapshot) return nullptr;
    if (seatId > 0 && seatId <= m_lastPayload.hostSnapshot->seats.size()) {
        return &m_lastPayload.hostSnapshot->seats[seatId - 1u];
    }
    return nullptr;
}

void SeatsPage::updateState(const EngineStatePayload& payload) {
    m_lastPayload = payload;
    // Populate first so button enablement reflects the persisted/current
    // application in the same refresh cycle.
    populateCombos(1, m_seat1);
    populateCombos(2, m_seat2);
    applyInputIdentification(1, m_seat1);
    applyInputIdentification(2, m_seat2);
    updateSeatData(1, m_seat1);
    updateSeatData(2, m_seat2);
    maybeAutoRouteAudio(1, m_seat1);
    maybeAutoRouteAudio(2, m_seat2);
}

void SeatsPage::refreshApplications() {
    populateCombos(1, m_seat1);
    populateCombos(2, m_seat2);
    updateSeatData(1, m_seat1);
    updateSeatData(2, m_seat2);
}

void SeatsPage::updateSeatData(std::uint32_t seatId, SeatWidgets& w) {
    const auto* snapshot = seatSnapshot(seatId);
    const bool hasApplication =
        w.appCombo && !w.appCombo->currentData().toString().isEmpty();
    const hydra::hostipc::SeatRuntimeInfo* runtimeInfo = nullptr;
    if (seatId > 0 && seatId <= m_lastPayload.seatRuntimeInfo.size() &&
        m_lastPayload.seatRuntimeInfo[seatId - 1u]) {
        runtimeInfo = &*m_lastPayload.seatRuntimeInfo[seatId - 1u];
    }
    if (w.runtimeLabel) {
        const bool hasRuntimePresentation =
            runtimeInfo && !runtimeInfo->titleUtf8.empty();
        w.runtimeLabel->setVisible(hasRuntimePresentation);
        if (hasRuntimePresentation) {
            w.runtimeLabel->setText(
                QStringLiteral("%1 · %2")
                    .arg(QString::fromUtf8(runtimeInfo->titleUtf8.c_str()))
                    .arg(providerDisplayName(runtimeInfo->providerIdUtf8)));
        }
    }

    if (!snapshot) {
        w.stateBadge->setText("Host unavailable");
        w.stateBadge->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none; margin-left: 12px;");
        w.configureBtn->setDisabled(true);
        w.launchBtn->setDisabled(true);
        w.stopBtn->setDisabled(true);
        w.reconfigureBtn->setDisabled(true);
        if (w.appCombo) w.appCombo->setDisabled(true);
        if (w.keyboardIdentifyBtn) w.keyboardIdentifyBtn->setDisabled(true);
        if (w.mouseIdentifyBtn) w.mouseIdentifyBtn->setDisabled(true);
        if (w.audioCombo) w.audioCombo->setDisabled(true);
        return;
    }

    if (w.operationInFlight) {
        w.stateBadge->setText("Working...");
        w.stateBadge->setStyleSheet(
            "font-size: 13px; font-weight: bold; color: #B5B5B5; "
            "border: none; margin-left: 12px;");
        w.configureBtn->setDisabled(true);
        w.launchBtn->setDisabled(true);
        w.stopBtn->setDisabled(true);
        w.reconfigureBtn->setDisabled(true);
        if (w.appCombo) w.appCombo->setDisabled(true);
        if (w.displayCombo) w.displayCombo->setDisabled(true);
        if (w.keyboardCombo) w.keyboardCombo->setDisabled(true);
        if (w.mouseCombo) w.mouseCombo->setDisabled(true);
        if (w.ctrlPhysCombo) w.ctrlPhysCombo->setDisabled(true);
        if (w.ctrlSrcCombo) w.ctrlSrcCombo->setDisabled(true);
        if (w.keyboardIdentifyBtn) w.keyboardIdentifyBtn->setDisabled(true);
        if (w.mouseIdentifyBtn) w.mouseIdentifyBtn->setDisabled(true);
        if (w.audioCombo) w.audioCombo->setDisabled(true);
        return;
    }

    if (snapshot->gameLeaseActive) {
        const bool running = snapshot->processOwned;
        w.stateBadge->setText(running ? "Running" : "Busy");
        w.stateBadge->setStyleSheet(
            running
                ? "font-size: 13px; font-weight: bold; color: #E10600; border: none; margin-left: 12px;"
                : "font-size: 13px; font-weight: bold; color: #B5B5B5; border: none; margin-left: 12px;");

        w.configureBtn->setVisible(false);
        w.launchBtn->setVisible(false);
        w.stopBtn->setVisible(running);
        w.reconfigureBtn->setVisible(true);

        w.stopBtn->setDisabled(!running);
        w.reconfigureBtn->setDisabled(true);
        w.reconfigureBtn->setToolTip(
            running
                ? "Stop the running Seat application before changing hardware."
                : "Seat activation/cleanup is still in progress.");
        if (w.appCombo) w.appCombo->setDisabled(true);
        if (w.displayCombo) w.displayCombo->setDisabled(true);
        if (w.keyboardCombo) w.keyboardCombo->setDisabled(true);
        if (w.mouseCombo) w.mouseCombo->setDisabled(true);
        if (w.ctrlPhysCombo) w.ctrlPhysCombo->setDisabled(true);
        if (w.ctrlSrcCombo) w.ctrlSrcCombo->setDisabled(true);
        if (w.keyboardIdentifyBtn) w.keyboardIdentifyBtn->setDisabled(true);
        if (w.mouseIdentifyBtn) w.mouseIdentifyBtn->setDisabled(true);
        if (w.audioCombo) {
            w.audioCombo->setDisabled(m_lastPayload.audioEndpointError);
        }
        return;
    }

    w.audioRouteAttemptPid = 0;
    w.audioRouteAttemptCreation = 0;
    w.audioRouteAttemptEndpoint.clear();

    const bool leaseOwnedElsewhere =
        snapshot->uiLeaseActive &&
        (!m_hostControl || !m_hostControl->ownsUiLease(seatId));

    w.stateBadge->setText(leaseOwnedElsewhere ? "Locked" : "Ready");
    w.stateBadge->setStyleSheet(
        "font-size: 13px; font-weight: bold; color: #777777; border: none; margin-left: 12px;");

    w.configureBtn->setVisible(true);
    w.launchBtn->setVisible(true);
    w.stopBtn->setVisible(false);
    w.reconfigureBtn->setVisible(false);

    w.configureBtn->setDisabled(leaseOwnedElsewhere);
    w.launchBtn->setDisabled(leaseOwnedElsewhere || !hasApplication);
    w.reconfigureBtn->setToolTip({});
    if (w.appCombo) {
        w.appCombo->setDisabled(
            leaseOwnedElsewhere || w.appCombo->count() <= 1);
    }
    if (w.displayCombo) {
        w.displayCombo->setDisabled(
            leaseOwnedElsewhere || w.displayCombo->count() <= 1);
    }
    if (w.keyboardCombo) {
        w.keyboardCombo->setDisabled(
            leaseOwnedElsewhere || w.keyboardCombo->count() <= 1);
    }
    if (w.mouseCombo) {
        w.mouseCombo->setDisabled(
            leaseOwnedElsewhere || w.mouseCombo->count() <= 1);
    }
    if (w.ctrlPhysCombo) {
        w.ctrlPhysCombo->setDisabled(
            leaseOwnedElsewhere ||
            m_lastPayload.controllerInventoryError);
        w.ctrlPhysCombo->setToolTip(
            m_lastPayload.controllerInventoryError
                ? "Controller inventory is unavailable."
                : QString{});
    }
    if (w.ctrlSrcCombo) w.ctrlSrcCombo->setDisabled(true);
    if (w.keyboardIdentifyBtn) {
        w.keyboardIdentifyBtn->setDisabled(
            leaseOwnedElsewhere || m_lastPayload.keyboards.empty());
    }
    if (w.mouseIdentifyBtn) {
        w.mouseIdentifyBtn->setDisabled(
            leaseOwnedElsewhere || m_lastPayload.mice.empty());
    }
    if (w.audioCombo) {
        w.audioCombo->setDisabled(m_lastPayload.audioEndpointError);
    }
}

void SeatsPage::populateCombos(
    std::uint32_t seatId,
    SeatWidgets& w) {
    if (!w.appCombo || !w.displayCombo || !w.keyboardCombo || !w.mouseCombo ||
        !w.ctrlPhysCombo || !w.ctrlSrcCombo || !w.audioCombo) {
        return;
    }

    if (!w.appCombo->hasFocus()) {
        const QString previous = w.appCombo->currentData().toString();
        auto settings = settingsStore();
        const QString persisted =
            settings.value(seatApplicationKey(seatId)).toString();
        const auto applications = ApplicationLibrary::load();

        w.appCombo->blockSignals(true);
        w.appCombo->clear();
        w.appCombo->addItem("-- Select application --", QString());

        for (const auto& game : m_lastPayload.gameCatalog.games) {
            const QString executable =
                QString::fromUtf8(game.executablePathUtf8.c_str());
            if (executable.isEmpty()) continue;
            const QString title = QString::fromUtf8(game.titleUtf8.c_str());
            w.appCombo->addItem(
                QStringLiteral("%1 · Steam").arg(title), executable);
            const int index = w.appCombo->count() - 1;
            w.appCombo->setItemData(index, QString(), Qt::UserRole + 1);
            w.appCombo->setItemData(
                index,
                QString::fromUtf8(game.workingDirectoryUtf8.c_str()),
                Qt::UserRole + 2);
            w.appCombo->setItemData(
                index, QStringLiteral("steam"), Qt::UserRole + 3);
            w.appCombo->setItemData(
                index,
                QStringLiteral("Detected from Steam. HydraSeat launches the validated local executable through the host-owned Seat path."),
                Qt::ToolTipRole);
        }

        for (const auto& app : applications) {
            if (w.appCombo->findData(app.executablePath) >= 0) continue;
            w.appCombo->addItem(app.title, app.executablePath);
            const int index = w.appCombo->count() - 1;
            w.appCombo->setItemData(index, app.arguments, Qt::UserRole + 1);
            w.appCombo->setItemData(
                index, app.workingDirectory, Qt::UserRole + 2);
            w.appCombo->setItemData(
                index, QStringLiteral("custom"), Qt::UserRole + 3);
        }

        // Preserve the existing combo layout while making Seat setup
        // self-contained: a user can select an executable here directly.
        w.appCombo->addItem("Browse...", browseApplicationSentinel());

        const QString wanted =
            !previous.isEmpty() && previous != browseApplicationSentinel()
                ? previous
                : persisted;
        const int index = w.appCombo->findData(wanted);
        w.appCombo->setCurrentIndex(index >= 0 ? index : 0);
        w.appCombo->setEnabled(true);
        w.appCombo->blockSignals(false);
    }

    // Never rewrite a hardware combo while the user is actively choosing a value.
    if (w.displayCombo->hasFocus() || w.keyboardCombo->hasFocus() ||
        w.mouseCombo->hasFocus() || w.ctrlPhysCombo->hasFocus() ||
        w.ctrlSrcCombo->hasFocus()) {
        return;
    }

    QString assignedDisplay;
    QString assignedKeyboard;
    QString assignedMouse;
    QString assignedController;
    if (seatId > 0 && seatId <= m_lastPayload.seatHardware.size()) {
        const auto& assigned = m_lastPayload.seatHardware[seatId - 1u];
        if (assigned) {
            assignedDisplay =
                QString::fromUtf8(assigned->displayIdUtf8.c_str());
            assignedKeyboard =
                QString::fromUtf8(assigned->keyboardIdUtf8.c_str());
            assignedMouse =
                QString::fromUtf8(assigned->mouseIdUtf8.c_str());
            assignedController =
                QString::fromUtf8(assigned->controllerIdUtf8.c_str());
        }
    }

    const auto repopulateHardware =
        [](QComboBox* combo,
           const std::vector<hydra::DeviceInfo>& devices,
           const QString& assignedId,
           const QString& emptyText) {
            const QString previous = combo->currentData().toString();

            combo->blockSignals(true);
            combo->clear();
            combo->addItem(
                devices.empty() ? emptyText : QString("-- Select --"),
                QString());

            for (const auto& device : devices) {
                const QString id = QString::fromStdWString(device.id);
                const QString name = QString::fromStdWString(device.name);
                const auto duplicateCount = std::count_if(
                    devices.cbegin(),
                    devices.cend(),
                    [&](const hydra::DeviceInfo& candidate) {
                        return QString::fromStdWString(candidate.name)
                                   .compare(name, Qt::CaseInsensitive) == 0;
                    });
                const QString visibleName =
                    duplicateCount > 1
                        ? QStringLiteral("%1 · %2")
                              .arg(name)
                              .arg(shortStableId(id))
                        : name;
                combo->addItem(visibleName, id);
                combo->setItemData(
                    combo->count() - 1,
                    QStringLiteral("%1\n%2").arg(name).arg(id),
                    Qt::ToolTipRole);
            }

            QString wanted = previous;
            if (wanted.isEmpty()) wanted = assignedId;
            const int index = combo->findData(wanted);
            if (index >= 0) combo->setCurrentIndex(index);
            combo->setEnabled(!devices.empty());
            combo->blockSignals(false);
        };

    repopulateHardware(
        w.displayCombo,
        m_lastPayload.displays,
        assignedDisplay,
        "No physical display detected");
    repopulateHardware(
        w.keyboardCombo,
        m_lastPayload.keyboards,
        assignedKeyboard,
        "No confirmed keyboard detected");
    repopulateHardware(
        w.mouseCombo,
        m_lastPayload.mice,
        assignedMouse,
        "No confirmed mouse detected");

    const QString previousAudio = w.audioCombo->currentData().toString();
    auto settings = settingsStore();
    const QString persistedAudio =
        settings.value(seatPreferredAudioKey(seatId)).toString();

    w.audioCombo->blockSignals(true);
    w.audioCombo->clear();
    w.audioCombo->addItem("System Default (no override)", QString());
    for (const auto& endpoint : m_lastPayload.audioEndpoints) {
        w.audioCombo->addItem(
            QString::fromStdWString(endpoint.friendlyName),
            QString::fromStdWString(endpoint.endpointId));
    }

    QString wantedAudio = previousAudio;
    if (wantedAudio.isEmpty()) wantedAudio = persistedAudio;
    int audioIndex = w.audioCombo->findData(wantedAudio);
    if (!wantedAudio.isEmpty() && audioIndex < 0) {
        w.audioCombo->addItem(
            "Previously selected output (disconnected)",
            wantedAudio);
        audioIndex = w.audioCombo->count() - 1;
    }
    w.audioCombo->setCurrentIndex(audioIndex >= 0 ? audioIndex : 0);
    w.audioCombo->setEnabled(!m_lastPayload.audioEndpointError);
    w.audioCombo->blockSignals(false);

    const QString previousPhysical = w.ctrlPhysCombo->currentData().toString();
    const QVariant previousSource = w.ctrlSrcCombo->currentData();

    w.ctrlPhysCombo->blockSignals(true);
    w.ctrlSrcCombo->blockSignals(true);
    w.ctrlPhysCombo->clear();
    w.ctrlSrcCombo->clear();

    w.ctrlPhysCombo->addItem(
        m_lastPayload.controllerInventoryError
            ? "-- Controller inventory unavailable --"
            : "-- None --",
        QString());
    if (m_lastPayload.controllerInventoryError &&
        !assignedController.isEmpty()) {
        w.ctrlPhysCombo->addItem(
            "Assigned controller (inventory unavailable)",
            assignedController);
    }
    for (const auto& phys :
         m_lastPayload.controllerInventory.physicalControllers) {
        w.ctrlPhysCombo->addItem(
            QString::fromStdWString(phys.displayName),
            QString::fromStdWString(phys.persistentId));
    }

    w.ctrlSrcCombo->addItem("-- None --", QVariant());
    for (const auto& src : m_lastPayload.controllerInventory.sources) {
        if (!src.connected || !src.runtimeXInputSlot) continue;
        w.ctrlSrcCombo->addItem(
            QString("%1 (XInput %2)")
                .arg(QString::fromStdWString(src.displayName))
                .arg(static_cast<int>(*src.runtimeXInputSlot)),
            QVariant::fromValue(static_cast<int>(*src.runtimeXInputSlot)));
    }

    const QString wantedPhysical =
        previousPhysical.isEmpty() ? assignedController : previousPhysical;
    const int physicalIndex =
        w.ctrlPhysCombo->findData(wantedPhysical);
    if (physicalIndex >= 0) {
        w.ctrlPhysCombo->setCurrentIndex(physicalIndex);
    }

    const QString currentPhysical =
        w.ctrlPhysCombo->currentData().toString();
    if (currentPhysical.isEmpty()) {
        w.controllerPairingBaseline.reset();
        w.controllerPairingId.clear();
    } else if (w.controllerPairingId != currentPhysical ||
               !w.controllerPairingBaseline) {
        w.controllerPairingBaseline =
            hydra::controller::captureXInputPairingSnapshot();
        w.controllerPairingId = currentPhysical;
    }

    const int sourceIndex = w.ctrlSrcCombo->findData(previousSource);
    if (sourceIndex >= 0) {
        w.ctrlSrcCombo->setCurrentIndex(sourceIndex);
    }

    w.ctrlPhysCombo->blockSignals(false);
    w.ctrlSrcCombo->blockSignals(false);
}


void SeatsPage::onIdentifyInput(std::uint32_t seatId, bool keyboard) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    auto* button = keyboard ? w.keyboardIdentifyBtn : w.mouseIdentifyBtn;
    if (!button || !w.feedbackLabel) return;

    const auto& activity =
        keyboard ? m_lastPayload.keyboardActivity : m_lastPayload.mouseActivity;
    auto& baseline =
        keyboard ? w.keyboardIdentifyBaseline : w.mouseIdentifyBaseline;
    baseline = activity ? activity->sequence : 0u;

    button->setText("Waiting for input...");
    w.feedbackLabel->setText(
        keyboard
            ? QString("Press any key on the keyboard for Seat %1.").arg(seatId)
            : QString("Move or click the mouse for Seat %1.").arg(seatId));
    w.feedbackLabel->setStyleSheet(
        "font-size: 12px; color: #B5B5B5; border: none;");
    w.feedbackLabel->setVisible(true);
}

void SeatsPage::applyInputIdentification(
    std::uint32_t seatId,
    SeatWidgets& w) {
    const auto apply = [&](bool keyboard) {
        auto& baseline =
            keyboard ? w.keyboardIdentifyBaseline : w.mouseIdentifyBaseline;
        if (!baseline) return;

        const auto& activity =
            keyboard ? m_lastPayload.keyboardActivity : m_lastPayload.mouseActivity;
        if (!activity || activity->sequence == *baseline ||
            activity->stableIdUtf8.empty()) {
            return;
        }

        QComboBox* combo = keyboard ? w.keyboardCombo : w.mouseCombo;
        QPushButton* button =
            keyboard ? w.keyboardIdentifyBtn : w.mouseIdentifyBtn;
        if (!combo || !button) return;

        const QString stableId =
            QString::fromUtf8(activity->stableIdUtf8.c_str());
        const int index = combo->findData(stableId);
        if (index < 0) {
            // Keep the identification armed. The same host poll may confirm the
            // device before the next inventory refresh exposes it.
            return;
        }

        combo->setCurrentIndex(index);
        baseline.reset();
        button->setText(
            keyboard ? "Identify by key press" : "Identify by movement");
        if (w.feedbackLabel) {
            w.feedbackLabel->setText(
                QString("%1 identified for Seat %2: %3")
                    .arg(keyboard ? "Keyboard" : "Mouse")
                    .arg(seatId)
                    .arg(combo->currentText()));
            w.feedbackLabel->setStyleSheet(
                "font-size: 12px; color: #B5B5B5; border: none;");
            w.feedbackLabel->setVisible(true);
        }
    };

    apply(true);
    apply(false);
}

void SeatsPage::onAudioSelectionChanged(std::uint32_t seatId) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    if (!w.audioCombo) return;

    auto settings = settingsStore();
    settings.setValue(
        seatPreferredAudioKey(seatId),
        w.audioCombo->currentData().toString());
    settings.sync();

    if (!w.audioRouteInFlight) {
        w.audioRouteAttemptPid = 0;
        w.audioRouteAttemptCreation = 0;
        w.audioRouteAttemptEndpoint.clear();
    }
    maybeAutoRouteAudio(seatId, w);
}

void SeatsPage::maybeAutoRouteAudio(
    std::uint32_t seatId,
    SeatWidgets& w) {
    const auto* snapshot = seatSnapshot(seatId);
    if (!m_hostControl || !snapshot || !snapshot->processOwned ||
        !w.audioCombo || m_lastPayload.audioEndpointError ||
        m_lastPayload.audioSessionError || w.audioRouteInFlight) {
        return;
    }

    auto settings = settingsStore();
    const QString preferred =
        settings.value(seatPreferredAudioKey(seatId)).toString();
    if (preferred.isEmpty()) return;

    const bool endpointAvailable = std::any_of(
        m_lastPayload.audioEndpoints.cbegin(),
        m_lastPayload.audioEndpoints.cend(),
        [&](const hydra::windows::AudioRenderEndpoint& endpoint) {
            return QString::fromStdWString(endpoint.endpointId) == preferred;
        });
    if (!endpointAvailable) return;

    const auto session = std::find_if(
        m_lastPayload.audioSessions.cbegin(),
        m_lastPayload.audioSessions.cend(),
        [&](const hydra::windows::AudioSessionObservation& candidate) {
            return candidate.processIdentity &&
                   candidate.processIdentity->pid == snapshot->processId &&
                   candidate.processIdentity->creationIdentity ==
                       snapshot->processCreationIdentity;
        });
    if (session == m_lastPayload.audioSessions.cend()) return;

    if (QString::fromStdWString(session->endpointId) == preferred) {
        w.audioRouteAttemptPid = snapshot->processId;
        w.audioRouteAttemptCreation = snapshot->processCreationIdentity;
        w.audioRouteAttemptEndpoint = preferred;
        return;
    }

    if (w.audioRouteAttemptPid == snapshot->processId &&
        w.audioRouteAttemptCreation == snapshot->processCreationIdentity &&
        w.audioRouteAttemptEndpoint == preferred) {
        return;
    }

    const auto hostControl = m_hostControl;
    const auto pid = snapshot->processId;
    const auto creation = snapshot->processCreationIdentity;
    const std::string endpointUtf8 = toUtf8String(preferred);
    w.audioRouteAttemptPid = pid;
    w.audioRouteAttemptCreation = creation;
    w.audioRouteAttemptEndpoint = preferred;
    w.audioRouteInFlight = true;

    auto* watcher = new QFutureWatcher<SeatAudioTaskResult>(this);
    connect(
        watcher,
        &QFutureWatcher<SeatAudioTaskResult>::finished,
        this,
        [this, watcher, seatId]() {
            const auto task = watcher->result();
            watcher->deleteLater();

            SeatWidgets& current =
                (seatId == 1u) ? m_seat1 : m_seat2;
            current.audioRouteInFlight = false;

            const bool succeeded =
                task.status &&
                *task.status == hydra::hostipc::AudioMutationStatus::Success;
            if (!succeeded) {
                if (current.feedbackLabel) {
                    current.feedbackLabel->setText(
                        task.error.empty()
                            ? "Preferred audio output could not be applied automatically."
                            : QString::fromStdString(task.error));
                    current.feedbackLabel->setStyleSheet(
                        "font-size: 12px; color: #E10600; border: none;");
                    current.feedbackLabel->setVisible(true);
                }
                return;
            }

            if (current.feedbackLabel) {
                current.feedbackLabel->setText(
                    "Preferred audio output applied automatically.");
                current.feedbackLabel->setStyleSheet(
                    "font-size: 12px; color: #B5B5B5; border: none;");
                current.feedbackLabel->setVisible(true);
            }
        });

    watcher->setFuture(QtConcurrent::run(
        [hostControl, pid, creation, endpointUtf8]() mutable {
            SeatAudioTaskResult task;
            task.status = hostControl->routeAudio(
                pid, creation, endpointUtf8, &task.error);
            return task;
        }));
}

void SeatsPage::onControllerSelectionChanged(std::uint32_t seatId) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    if (!w.ctrlPhysCombo || !w.ctrlSrcCombo || !w.feedbackLabel) return;

    const QString physicalControllerId =
        w.ctrlPhysCombo->currentData().toString();
    if (physicalControllerId.isEmpty()) {
        w.controllerPairingBaseline.reset();
        w.controllerPairingId.clear();
        w.ctrlSrcCombo->setCurrentIndex(0);
        return;
    }

    w.controllerPairingBaseline =
        hydra::controller::captureXInputPairingSnapshot();
    w.controllerPairingId = physicalControllerId;

    std::vector<int> connectedSlots;
    for (const auto& source : m_lastPayload.controllerInventory.sources) {
        if (!source.connected || !source.runtimeXInputSlot) continue;
        connectedSlots.push_back(
            static_cast<int>(*source.runtimeXInputSlot));
    }

    if (connectedSlots.size() == 1u) {
        const int index = w.ctrlSrcCombo->findData(connectedSlots.front());
        if (index >= 0) w.ctrlSrcCombo->setCurrentIndex(index);
        return;
    }

    if (connectedSlots.size() > 1u) {
        w.ctrlSrcCombo->setCurrentIndex(0);
        w.feedbackLabel->setText(
            "Multiple controllers are connected. Hold any button on the selected "
            "physical controller, then click Configure Seat to identify its "
            "XInput source.");
        w.feedbackLabel->setStyleSheet(
            "font-size: 12px; color: #B5B5B5; border: none;");
        w.feedbackLabel->setVisible(true);
    }
}

void SeatsPage::onConfigureRequested(std::uint32_t seatId) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    w.feedbackLabel->setVisible(false);

    if (!m_hostControl || w.operationInFlight) {
        if (!m_hostControl) {
            w.feedbackLabel->setText("Canonical host control is unavailable.");
            w.feedbackLabel->setStyleSheet(
                "font-size: 12px; color: #E10600; border: none;");
            w.feedbackLabel->setVisible(true);
        }
        return;
    }

    if (m_lastPayload.controllerInventoryError &&
        !w.ctrlPhysCombo->currentData().toString().isEmpty()) {
        w.feedbackLabel->setText(
            "Controller inventory is unavailable; configuration was not changed.");
        w.feedbackLabel->setStyleSheet(
            "font-size: 12px; color: #E10600; border: none;");
        w.feedbackLabel->setVisible(true);
        return;
    }

    const QString displayId = w.displayCombo->currentData().toString();
    const QString keyboardId = w.keyboardCombo->currentData().toString();
    const QString mouseId = w.mouseCombo->currentData().toString();
    const QString physicalControllerId =
        w.ctrlPhysCombo->currentData().toString();
    const bool hasPhysicalController = !physicalControllerId.isEmpty();

    if (displayId.isEmpty() || keyboardId.isEmpty() || mouseId.isEmpty()) {
        w.feedbackLabel->setText(
            "Select a confirmed display, keyboard, and mouse before configuring the Seat.");
        w.feedbackLabel->setStyleSheet(
            "font-size: 12px; color: #E10600; border: none;");
        w.feedbackLabel->setVisible(true);
        return;
    }

    std::optional<std::uint8_t> resolvedSlot;
    if (hasPhysicalController) {
        std::vector<std::uint8_t> connectedSlots;
        for (const auto& source : m_lastPayload.controllerInventory.sources) {
            if (!source.connected || !source.runtimeXInputSlot) continue;
            connectedSlots.push_back(*source.runtimeXInputSlot);
        }

        if (connectedSlots.empty()) {
            w.feedbackLabel->setText(
                "No connected XInput controller source is available; Seat hardware was not changed.");
            w.feedbackLabel->setStyleSheet(
                "font-size: 12px; color: #E10600; border: none;");
            w.feedbackLabel->setVisible(true);
            return;
        }

        bool selectedIsOnlyPhysical = false;
        if (m_lastPayload.controllerInventory.physicalControllers.size() == 1u) {
            selectedIsOnlyPhysical =
                QString::fromStdWString(
                    m_lastPayload.controllerInventory.physicalControllers.front()
                        .persistentId)
                    .compare(physicalControllerId, Qt::CaseInsensitive) == 0;
        }

        if (connectedSlots.size() == 1u && selectedIsOnlyPhysical) {
            // One physical controller and one XInput source is a bounded
            // one-to-one mapping; no gesture is needed.
            resolvedSlot = connectedSlots.front();
        } else {
            if (!w.controllerPairingBaseline ||
                !w.controllerPairingBaseline->authoritative ||
                w.controllerPairingId != physicalControllerId) {
                w.controllerPairingBaseline =
                    hydra::controller::captureXInputPairingSnapshot();
                w.controllerPairingId = physicalControllerId;
                w.feedbackLabel->setText(
                    "Hold a button on the selected physical controller, then "
                    "click Configure Seat again. No Seat settings were changed.");
                w.feedbackLabel->setStyleSheet(
                    "font-size: 12px; color: #B5B5B5; border: none;");
                w.feedbackLabel->setVisible(true);
                return;
            }

            const auto after =
                hydra::controller::captureXInputPairingSnapshot();
            const auto probe =
                hydra::controller::detectUniqueXInputButtonPress(
                    *w.controllerPairingBaseline, after);
            w.controllerPairingBaseline = after;

            if (probe.status !=
                    hydra::controller::PairingProbeStatus::UniqueButtonPress ||
                !probe.runtimeSlot) {
                const QString diagnostic =
                    probe.status ==
                            hydra::controller::PairingProbeStatus::AmbiguousButtonPress
                        ? QStringLiteral(
                              "More than one controller changed. Hold a button "
                              "on only the selected controller and try again.")
                        : QStringLiteral(
                              "Hold a button on the selected controller while "
                              "clicking Configure Seat so HydraSeat can identify "
                              "its XInput source.");
                w.feedbackLabel->setText(
                    QString("%1 No Seat settings were changed.").arg(diagnostic));
                w.feedbackLabel->setStyleSheet(
                    "font-size: 12px; color: #B5B5B5; border: none;");
                w.feedbackLabel->setVisible(true);
                return;
            }
            resolvedSlot = *probe.runtimeSlot;
        }
    }

    const auto hostControl = m_hostControl;
    const std::string displayUtf8 = toUtf8String(displayId);
    const std::string keyboardUtf8 = toUtf8String(keyboardId);
    const std::string mouseUtf8 = toUtf8String(mouseId);
    const std::string controllerUtf8 =
        toUtf8String(physicalControllerId);

    w.operationInFlight = true;
    w.feedbackLabel->setText("Saving Seat hardware...");
    w.feedbackLabel->setStyleSheet(
        "font-size: 12px; color: #B5B5B5; border: none;");
    w.feedbackLabel->setVisible(true);
    updateSeatData(seatId, w);

    auto* watcher =
        new QFutureWatcher<SeatConfigureTaskResult>(this);
    connect(
        watcher,
        &QFutureWatcher<SeatConfigureTaskResult>::finished,
        this,
        [this,
         watcher,
         seatId,
         hasPhysicalController,
         resolvedSlot]() {
            const auto task = watcher->result();
            watcher->deleteLater();

            SeatWidgets& current =
                (seatId == 1u) ? m_seat1 : m_seat2;
            current.operationInFlight = false;

            if (!task.assignment) {
                current.feedbackLabel->setText(
                    QString("Hardware assignment failed: %1")
                        .arg(QString::fromStdString(task.error)));
                current.feedbackLabel->setStyleSheet(
                    "font-size: 12px; color: #E10600; border: none;");
                current.feedbackLabel->setVisible(true);
                updateSeatData(seatId, current);
                return;
            }

            if (seatId > 0 &&
                seatId <= m_lastPayload.seatHardware.size()) {
                m_lastPayload.seatHardware[seatId - 1u] =
                    *task.assignment;
            }

            if (task.pairingRequested && !task.snapshot) {
                current.feedbackLabel->setText(
                    QString(
                        "The stable controller assignment was saved, but "
                        "current XInput pairing failed: %1")
                        .arg(QString::fromStdString(task.error)));
                current.feedbackLabel->setStyleSheet(
                    "font-size: 12px; color: #E10600; border: none;");
                current.feedbackLabel->setVisible(true);
                updateSeatData(seatId, current);
                return;
            }

            if (task.snapshot) {
                m_lastPayload.hostSnapshot = *task.snapshot;
            }

            if (hasPhysicalController && resolvedSlot) {
                const int sourceIndex =
                    current.ctrlSrcCombo->findData(
                        static_cast<int>(*resolvedSlot));
                if (sourceIndex >= 0) {
                    current.ctrlSrcCombo->setCurrentIndex(sourceIndex);
                }
            } else {
                current.ctrlSrcCombo->setCurrentIndex(0);
                current.controllerPairingBaseline.reset();
                current.controllerPairingId.clear();
            }

            current.feedbackLabel->setText(
                hasPhysicalController
                    ? "Seat hardware saved; controller identity persisted and current XInput source paired."
                    : "Seat hardware assignment saved.");
            current.feedbackLabel->setStyleSheet(
                "font-size: 12px; color: #B5B5B5; border: none;");
            current.feedbackLabel->setVisible(true);
            updateSeatData(seatId, current);
        });

    watcher->setFuture(QtConcurrent::run(
        [hostControl,
         seatId,
         displayUtf8,
         keyboardUtf8,
         mouseUtf8,
         controllerUtf8,
         resolvedSlot]() mutable {
            SeatConfigureTaskResult task;
            task.assignment = hostControl->assignSeatHardware(
                seatId,
                displayUtf8,
                keyboardUtf8,
                mouseUtf8,
                controllerUtf8,
                &task.error);
            if (!task.assignment) {
                return task;
            }

            if (!controllerUtf8.empty() && resolvedSlot) {
                task.pairingRequested = true;
                task.snapshot = hostControl->pairController(
                    seatId,
                    controllerUtf8,
                    *resolvedSlot,
                    &task.error);
            }
            return task;
        }));
}
void SeatsPage::onApplicationSelectionChanged(std::uint32_t seatId) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    if (!w.appCombo) return;

    const QString selected = w.appCombo->currentData().toString();
    if (selected == browseApplicationSentinel()) {
        const QString executablePath = QFileDialog::getOpenFileName(
            this,
            "Select game executable",
            {},
            "Windows applications (*.exe);;All files (*.*)");
        if (executablePath.isEmpty()) {
            // Restore the persisted/current application immediately instead of
            // flashing back to "-- Select application --" until the next poll.
            refreshApplications();
            return;
        }

        const QFileInfo executable(executablePath);
        ApplicationLibrary::remember(ApplicationLaunchEntry{
            executable.completeBaseName(),
            executable.absoluteFilePath(),
            {},
            executable.absolutePath(),
        });
        auto settings = settingsStore();
        settings.setValue(
            seatApplicationKey(seatId), executable.absoluteFilePath());
        settings.sync();
        refreshApplications();
        return;
    }

    auto settings = settingsStore();
    settings.setValue(seatApplicationKey(seatId), selected);
    settings.sync();
    updateSeatData(seatId, w);
}

void SeatsPage::onLaunchRequested(std::uint32_t seatId) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    w.feedbackLabel->setVisible(false);

    if (!m_hostControl || w.operationInFlight) {
        if (!m_hostControl) {
            w.feedbackLabel->setText("Canonical host control is unavailable.");
            w.feedbackLabel->setStyleSheet(
                "font-size: 12px; color: #E10600; border: none;");
            w.feedbackLabel->setVisible(true);
        }
        return;
    }

    const QString executablePath = w.appCombo->currentData().toString();
    const int selectedIndex = w.appCombo->currentIndex();
    const QString title = w.appCombo->currentText();
    const QString arguments =
        w.appCombo->itemData(selectedIndex, Qt::UserRole + 1).toString();
    QString workingDirectory =
        w.appCombo->itemData(selectedIndex, Qt::UserRole + 2).toString();
    const QFileInfo executable(executablePath);

    if (executablePath.isEmpty() || !executable.isAbsolute() ||
        !executable.exists() || !executable.isFile()) {
        w.feedbackLabel->setText(
            "Select a valid application before launching.");
        w.feedbackLabel->setStyleSheet(
            "font-size: 12px; color: #E10600; border: none;");
        w.feedbackLabel->setVisible(true);
        return;
    }
    if (workingDirectory.isEmpty()) {
        workingDirectory = executable.absolutePath();
    }

    const auto hostControl = m_hostControl;
    w.operationInFlight = true;
    w.feedbackLabel->setText("Starting application...");
    w.feedbackLabel->setStyleSheet(
        "font-size: 12px; color: #B5B5B5; border: none;");
    w.feedbackLabel->setVisible(true);
    updateSeatData(seatId, w);

    auto* watcher = new QFutureWatcher<SeatHostTaskResult>(this);
    connect(
        watcher,
        &QFutureWatcher<SeatHostTaskResult>::finished,
        this,
        [this, watcher, seatId]() {
            const auto task = watcher->result();
            watcher->deleteLater();

            SeatWidgets& current =
                (seatId == 1u) ? m_seat1 : m_seat2;
            current.operationInFlight = false;

            if (!task.snapshot) {
                current.feedbackLabel->setText(
                    QString("The application could not be launched. %1")
                        .arg(QString::fromStdString(task.error)));
                current.feedbackLabel->setStyleSheet(
                    "font-size: 12px; color: #E10600; border: none;");
                current.feedbackLabel->setVisible(true);
                updateSeatData(seatId, current);
                return;
            }

            m_lastPayload.hostSnapshot = *task.snapshot;
            current.feedbackLabel->setText(
                "Application launched on this Seat.");
            current.feedbackLabel->setStyleSheet(
                "font-size: 12px; color: #B5B5B5; border: none;");
            current.feedbackLabel->setVisible(true);
            updateSeatData(seatId, current);
        });

    watcher->setFuture(QtConcurrent::run(
        [hostControl,
         seatId,
         titleUtf8 = toUtf8String(title),
         executableUtf8 = toUtf8String(executable.absoluteFilePath()),
         argumentsUtf8 = toUtf8String(arguments),
         workingDirectoryUtf8 = toUtf8String(workingDirectory)]() mutable {
            SeatHostTaskResult task;
            task.snapshot = hostControl->launchGame(
                seatId,
                titleUtf8,
                executableUtf8,
                argumentsUtf8,
                workingDirectoryUtf8,
                &task.error);
            return task;
        }));
}
void SeatsPage::onStopRequested(std::uint32_t seatId) {
    SeatWidgets& w = (seatId == 1u) ? m_seat1 : m_seat2;
    w.feedbackLabel->setVisible(false);

    if (!m_hostControl || w.operationInFlight) {
        if (!m_hostControl) {
            w.feedbackLabel->setText("Canonical host control is unavailable.");
            w.feedbackLabel->setStyleSheet(
                "font-size: 12px; color: #E10600; border: none;");
            w.feedbackLabel->setVisible(true);
        }
        return;
    }

    if (UiSettings::load().confirmSeatStop) {
        const auto answer = QMessageBox::question(
            this,
            "Stop Seat application",
            QString("Stop the application running on Seat %1?").arg(seatId),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;
    }

    const auto hostControl = m_hostControl;
    w.operationInFlight = true;
    w.feedbackLabel->setText("Stopping application...");
    w.feedbackLabel->setStyleSheet(
        "font-size: 12px; color: #B5B5B5; border: none;");
    w.feedbackLabel->setVisible(true);
    updateSeatData(seatId, w);

    auto* watcher = new QFutureWatcher<SeatHostTaskResult>(this);
    connect(
        watcher,
        &QFutureWatcher<SeatHostTaskResult>::finished,
        this,
        [this, watcher, seatId]() {
            const auto task = watcher->result();
            watcher->deleteLater();

            SeatWidgets& current =
                (seatId == 1u) ? m_seat1 : m_seat2;
            current.operationInFlight = false;

            if (!task.snapshot) {
                current.feedbackLabel->setText(
                    QString("The application could not be stopped. %1")
                        .arg(QString::fromStdString(task.error)));
                current.feedbackLabel->setStyleSheet(
                    "font-size: 12px; color: #E10600; border: none;");
                current.feedbackLabel->setVisible(true);
                updateSeatData(seatId, current);
                return;
            }

            m_lastPayload.hostSnapshot = *task.snapshot;
            current.feedbackLabel->setText(
                "Application stopped and Seat authority released.");
            current.feedbackLabel->setStyleSheet(
                "font-size: 12px; color: #B5B5B5; border: none;");
            current.feedbackLabel->setVisible(true);
            updateSeatData(seatId, current);
        });

    watcher->setFuture(QtConcurrent::run(
        [hostControl, seatId]() {
            SeatHostTaskResult task;
            task.snapshot =
                hostControl->stopGame(seatId, &task.error);
            return task;
        }));
}

} // namespace hydra::ui
