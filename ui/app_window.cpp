#include "ui/app_window.hpp"
#include "ui/engine_poller.hpp"
#include <QTimer>

#include "ui/pages/dashboard_page.hpp"
#include "ui/pages/seats_page.hpp"
#include "ui/pages/applications_page.hpp"
#include "ui/pages/audio_page.hpp"
#include "ui/pages/hardware_page.hpp"
#include "ui/pages/diagnostics_page.hpp"
#include "ui/pages/settings_page.hpp"
#include "ui/application_library.hpp"
#include "ui/ui_settings.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace hydra::ui {

AppWindow::AppWindow(QWidget* parent)
    : QMainWindow(parent) {
    m_hostControl = std::make_shared<HostControlClient>();
    m_routingController =
        std::make_unique<RoutingController>(m_hostControl, this);
    m_enginePoller =
        std::make_unique<EnginePoller>(this);

    setupUi();

    QTimer::singleShot(0, this, [this]() {
        // Host bootstrap belongs on the poller's worker thread. The shared
        // bootstrap path already serializes sibling-host launch attempts, so a
        // control connection does not need to block the GUI for up to the pipe
        // timeout during startup. The control client connects lazily on the
        // first user mutation after the host snapshot becomes available.
        m_enginePoller->startPolling(
            UiSettings::load().refreshIntervalMs);
    });
}

AppWindow::~AppWindow() = default;

void AppWindow::setupUi() {
    setWindowTitle("HydraSeat");
    resize(1150, 750);
    setMinimumSize(1100, 700);

    setStyleSheet(R"(
        QMainWindow { background-color: #0A0A0A; color: #F5F5F5; font-family: 'Segoe UI', Arial, sans-serif; }
        QWidget { color: #F5F5F5; font-family: 'Segoe UI', Arial, sans-serif; }
        QLabel { color: #F5F5F5; }
        QPushButton, QCheckBox, QRadioButton { color: #F5F5F5; }
        QLineEdit, QComboBox, QSpinBox {
            color: #F5F5F5;
            selection-color: #F5F5F5;
            selection-background-color: #E10600;
        }
        QComboBox QAbstractItemView {
            background-color: #151515;
            color: #F5F5F5;
            selection-background-color: #292929;
            selection-color: #F5F5F5;
            border: 1px solid #333333;
        }
        QToolTip {
            background-color: #202020;
            color: #F5F5F5;
            border: 1px solid #444444;
        }
        QScrollBar:vertical { background: #101010; width: 10px; margin: 0px; }
        QScrollBar::handle:vertical { background: #333333; border-radius: 5px; min-height: 30px; }
        QScrollBar::handle:vertical:hover { background: #444444; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
    )");

    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto* outerLayout = new QVBoxLayout(centralWidget);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    auto* innerWidget = new QWidget(centralWidget);
    auto* mainLayout = new QHBoxLayout(innerWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    auto* sidebarContainer = new QWidget(innerWidget);
    sidebarContainer->setFixedWidth(220);
    sidebarContainer->setStyleSheet("background-color: #0A0A0A; border-right: 1px solid #1C1C1C;");
    auto* sidebarLayout = new QVBoxLayout(sidebarContainer);
    sidebarLayout->setContentsMargins(16, 24, 16, 24);
    sidebarLayout->setSpacing(8);

    auto* brandLabel = new QLabel("HYDRASEAT", sidebarContainer);
    brandLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: #F5F5F5; letter-spacing: 2px; margin-bottom: 16px; border: none;");
    sidebarLayout->addWidget(brandLabel);

    setupSidebar();
    m_sidebar->setParent(sidebarContainer);
    sidebarLayout->addWidget(m_sidebar);

    mainLayout->addWidget(sidebarContainer);

    // Main Workspace Stack
    m_workspaceStack = new QStackedWidget(innerWidget);
    m_workspaceStack->setStyleSheet("background-color: #0A0A0A; border: none;");

    // Instantiate Pages
    auto* dashboardPage = new DashboardPage(m_workspaceStack);
    auto* seatsPage = new SeatsPage(m_hostControl, m_workspaceStack);
    auto* applicationsPage =
        new ApplicationsPage(m_hostControl, m_workspaceStack);
    auto* audioPage = new AudioPage(m_routingController.get(), m_workspaceStack);
    auto* hardwarePage = new HardwarePage(m_workspaceStack);
    auto* diagnosticsPage = new DiagnosticsPage(m_workspaceStack);

    // We MUST add them in the exact order that matches the indices in setupSidebar
    m_workspaceStack->addWidget(dashboardPage);
    m_workspaceStack->addWidget(seatsPage);
    m_workspaceStack->addWidget(applicationsPage);
    m_workspaceStack->addWidget(audioPage);
    m_workspaceStack->addWidget(hardwarePage);
    m_workspaceStack->addWidget(diagnosticsPage);

    auto* settingsPageNode = new SettingsPage(m_workspaceStack);
    m_workspaceStack->addWidget(settingsPageNode);

    connect(
        applicationsPage,
        &ApplicationsPage::applicationLibraryChanged,
        seatsPage,
        &SeatsPage::refreshApplications);

    connect(
        dashboardPage, &DashboardPage::manageSeatsRequested,
        this, [this]() { navigateToPage(1); });
    connect(
        dashboardPage, &DashboardPage::applicationsRequested,
        this, [this]() { navigateToPage(2); });

    connect(
        settingsPageNode,
        &SettingsPage::preferencesChanged,
        this,
        [this]() {
            if (m_enginePoller) {
                m_enginePoller->startPolling(
                    UiSettings::load().refreshIntervalMs);
            }
        });

    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, dashboardPage, &DashboardPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, seatsPage, &SeatsPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, applicationsPage, &ApplicationsPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, audioPage, &AudioPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, hardwarePage, &HardwarePage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, diagnosticsPage, &DiagnosticsPage::updateState);
    connect(
        m_enginePoller.get(), &EnginePoller::stateUpdated,
        this, &AppWindow::maybeShowFirstRunWizard);

    mainLayout->addWidget(m_workspaceStack, 1);

    outerLayout->addWidget(innerWidget, 1);

    setupStatusbar();

    connect(
        m_enginePoller.get(),
        &EnginePoller::stateUpdated,
        this,
        [this](const EngineStatePayload& payload) {
            if (!m_connectionLabel || !m_statusLabel) return;
            if (payload.hostConnected) {
                m_connectionLabel->setText("Canonical host connected");
                const bool subsystemDegraded =
                    payload.hardwareError ||
                    payload.gameCatalogError ||
                    payload.controllerInventoryError ||
                    payload.audioEndpointError ||
                    payload.audioSessionError;
                m_statusLabel->setText(
                    subsystemDegraded ? "● Degraded" : "● Running");
                m_statusLabel->setToolTip(
                    subsystemDegraded
                        ? (payload.hostError.empty()
                               ? QStringLiteral(
                                     "One or more runtime subsystems are unavailable.")
                               : QString::fromStdString(payload.hostError))
                        : QString{});
            } else {
                m_connectionLabel->setText("Canonical host unavailable");
                m_statusLabel->setText("● Degraded");
                m_statusLabel->setToolTip(
                    QString::fromStdString(payload.hostError));
            }
        });

    // Select first interactive item (Dashboard)
    m_sidebar->setCurrentRow(1);
}

void AppWindow::setupSidebar() {
    m_sidebar = new QListWidget();
    m_sidebar->setFocusPolicy(Qt::NoFocus);
    m_sidebar->setStyleSheet(R"(
        QListWidget { background-color: transparent; border: none; outline: 0; }
        QListWidget::item { padding: 8px 12px; margin-bottom: 4px; border-radius: 6px; color: #B5B5B5; font-size: 13px; font-family: 'Segoe UI', sans-serif; }
        QListWidget::item:hover:!selected { background-color: #151515; color: #F5F5F5; }
        QListWidget::item:selected { background-color: #181818; color: #F5F5F5; font-weight: bold; border-left: 3px solid #E10600; padding-left: 9px; }
    )");

    auto addHeader = [&](const QString& text) {
        auto* item = new QListWidgetItem(text);
        item->setFlags(Qt::NoItemFlags);
        item->setFont(QFont("Segoe UI", 10, QFont::Bold));
        item->setForeground(QColor("#777777"));
        m_sidebar->addItem(item);
    };

    auto addNav = [&](const QString& text, int targetIndex) {
        auto* item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, targetIndex);
        m_sidebar->addItem(item);
    };

    addHeader("OVERVIEW");
    addNav("Dashboard", 0);

    addHeader("MANAGEMENT");
    addNav("Seats", 1);
    addNav("Applications", 2);
    addNav("Audio", 3);
    addNav("Hardware", 4);

    addHeader("SYSTEM");
    addNav("Diagnostics", 5);
    addNav("Settings", 6);

    connect(m_sidebar, &QListWidget::currentRowChanged, this, &AppWindow::onNavigationChanged);
}

void AppWindow::setupStatusbar() {
    auto* statusContainer = new QWidget(this);
    statusContainer->setStyleSheet("background-color: #101010; border-top: 1px solid #1C1C1C; padding: 4px;");
    auto* statusLayout = new QHBoxLayout(statusContainer);
    statusLayout->setContentsMargins(16, 4, 16, 4);

    auto* brandLabel = new QLabel("HydraSeat Engine", statusContainer);
    brandLabel->setStyleSheet("font-size: 12px; color: #B5B5B5; border: none;");

    m_connectionLabel = new QLabel("Canonical host connecting", statusContainer);
    m_connectionLabel->setStyleSheet("font-size: 12px; color: #777777; border: none; margin-left: 12px;");

    m_statusLabel = new QLabel("● Connecting", statusContainer);
    m_statusLabel->setStyleSheet("font-size: 12px; color: #E10600; font-weight: bold; border: none;");

    statusLayout->addWidget(brandLabel);
    statusLayout->addWidget(m_connectionLabel);
    statusLayout->addStretch();
    statusLayout->addWidget(m_statusLabel);

    auto* outerLayout = qobject_cast<QVBoxLayout*>(centralWidget()->layout());
    if (outerLayout) {
        outerLayout->addWidget(statusContainer);
    }
}

void AppWindow::onNavigationChanged(int index) {
    if (!m_sidebar || !m_workspaceStack) return;
    auto* item = m_sidebar->item(index);
    if (!item) return;

    bool ok;
    int targetIndex = item->data(Qt::UserRole).toInt(&ok);
    if (ok && targetIndex >= 0 && targetIndex < m_workspaceStack->count()) {
        m_workspaceStack->setCurrentIndex(targetIndex);
    }
}

void AppWindow::navigateToPage(int targetIndex) {
    if (!m_sidebar || !m_workspaceStack || targetIndex < 0 ||
        targetIndex >= m_workspaceStack->count()) {
        return;
    }
    for (int row = 0; row < m_sidebar->count(); ++row) {
        auto* item = m_sidebar->item(row);
        if (!item) continue;
        bool ok = false;
        const int itemTarget = item->data(Qt::UserRole).toInt(&ok);
        if (ok && itemTarget == targetIndex) {
            m_sidebar->setCurrentRow(row);
            return;
        }
    }
    m_workspaceStack->setCurrentIndex(targetIndex);
}

void AppWindow::maybeShowFirstRunWizard(const EngineStatePayload& payload) {
    if (!payload.hostConnected || payload.hardwareError) {
        return;
    }

    QSettings settings(
        QSettings::NativeFormat,
        QSettings::UserScope,
        QStringLiteral("HydraSeat"),
        QStringLiteral("HydraSeat"));
    if (settings.value(QStringLiteral("ui/firstRunGuideDismissed"), false).toBool()) {
        m_firstRunWizardHandled = true;
        if (m_firstRunWizard && m_firstRunWizard->isVisible()) {
            m_firstRunWizard->close();
        }
        return;
    }

    const auto configured = [&](std::size_t index) {
        if (index >= payload.seatHardware.size() || !payload.seatHardware[index]) {
            return false;
        }
        const auto& seat = *payload.seatHardware[index];
        return !seat.displayIdUtf8.empty() && !seat.keyboardIdUtf8.empty() &&
               !seat.mouseIdUtf8.empty();
    };
    const bool seat1Ready = configured(0);
    const bool seat2Ready = configured(1);
    const qsizetype savedApplications = ApplicationLibrary::load().size();
    const std::size_t gameCount = payload.gameCatalog.games.size() +
        static_cast<std::size_t>(savedApplications);
    const bool gamesReady = gameCount > 0u;
    const bool audioReady = !payload.audioEndpoints.empty();

    if (seat1Ready && seat2Ready && gamesReady) {
        settings.setValue(QStringLiteral("ui/firstRunGuideDismissed"), true);
        settings.sync();
        m_firstRunWizardHandled = true;
        if (m_firstRunWizard) m_firstRunWizard->close();
        return;
    }

    if (!m_firstRunWizard) {
        m_firstRunWizard = new QDialog(this);
        m_firstRunWizard->setWindowTitle(QStringLiteral("HydraSeat Setup Assistant"));
        m_firstRunWizard->setWindowModality(Qt::NonModal);
        m_firstRunWizard->setMinimumWidth(620);
        m_firstRunWizard->setAttribute(Qt::WA_DeleteOnClose, false);

        auto* layout = new QVBoxLayout(m_firstRunWizard);
        layout->setSpacing(14);

        auto* heading = new QLabel(
            QStringLiteral("Finish your two-Seat setup"),
            m_firstRunWizard);
        heading->setStyleSheet(
            "font-size: 22px; font-weight: bold; color: #F5F5F5;");
        layout->addWidget(heading);

        auto* intro = new QLabel(
            QStringLiteral(
                "Complete the steps below in order. This assistant stays open while "
                "you configure HydraSeat and updates automatically from the canonical host."),
            m_firstRunWizard);
        intro->setWordWrap(true);
        intro->setStyleSheet("font-size: 13px; color: #B5B5B5;");
        layout->addWidget(intro);

        auto* steps = new QGroupBox(
            QStringLiteral("Initial setup"),
            m_firstRunWizard);
        auto* grid = new QGridLayout(steps);
        grid->setHorizontalSpacing(16);
        grid->setVerticalSpacing(10);

        const auto addStep = [&](int row,
                                 const QString& step,
                                 QLabel*& status,
                                 QPushButton*& action,
                                 const QString& actionText) {
            auto* name = new QLabel(step, steps);
            status = new QLabel(QStringLiteral("Waiting"), steps);
            action = new QPushButton(actionText, steps);
            grid->addWidget(name, row, 0);
            grid->addWidget(status, row, 1);
            grid->addWidget(action, row, 2);
        };

        addStep(
            0,
            QStringLiteral("1. Seat 1 hardware"),
            m_firstRunSeat1Status,
            m_firstRunSeat1Button,
            QStringLiteral("Configure Seat 1"));
        addStep(
            1,
            QStringLiteral("2. Seat 2 hardware"),
            m_firstRunSeat2Status,
            m_firstRunSeat2Button,
            QStringLiteral("Configure Seat 2"));
        addStep(
            2,
            QStringLiteral("3. Game library"),
            m_firstRunGamesStatus,
            m_firstRunGamesButton,
            QStringLiteral("Choose Games"));

        auto* audioName = new QLabel(QStringLiteral("4. Audio outputs"), steps);
        m_firstRunAudioStatus = new QLabel(QStringLiteral("Optional"), steps);
        auto* audioHint = new QLabel(
            QStringLiteral("Choose a preferred output per Seat on the Seats page."),
            steps);
        audioHint->setWordWrap(true);
        audioHint->setStyleSheet("font-size: 11px; color: #777777;");
        grid->addWidget(audioName, 3, 0);
        grid->addWidget(m_firstRunAudioStatus, 3, 1);
        grid->addWidget(audioHint, 3, 2);
        layout->addWidget(steps);

        auto* identifyHint = new QLabel(
            QStringLiteral(
                "Duplicate keyboards and mice can be identified by physical activity. "
                "Steam games are discovered automatically; custom executables remain available."),
            m_firstRunWizard);
        identifyHint->setWordWrap(true);
        identifyHint->setStyleSheet("font-size: 12px; color: #777777;");
        layout->addWidget(identifyHint);

        auto* actions = new QHBoxLayout();
        actions->addStretch();
        auto* laterButton = new QPushButton(
            QStringLiteral("Set up later"),
            m_firstRunWizard);
        actions->addWidget(laterButton);
        layout->addLayout(actions);

        connect(
            m_firstRunSeat1Button,
            &QPushButton::clicked,
            this,
            [this]() { navigateToPage(1); });
        connect(
            m_firstRunSeat2Button,
            &QPushButton::clicked,
            this,
            [this]() { navigateToPage(1); });
        connect(
            m_firstRunGamesButton,
            &QPushButton::clicked,
            this,
            [this]() { navigateToPage(2); });
        connect(
            laterButton,
            &QPushButton::clicked,
            this,
            [this]() {
                QSettings localSettings(
                    QSettings::NativeFormat,
                    QSettings::UserScope,
                    QStringLiteral("HydraSeat"),
                    QStringLiteral("HydraSeat"));
                localSettings.setValue(
                    QStringLiteral("ui/firstRunGuideDismissed"),
                    true);
                localSettings.sync();
                m_firstRunWizardHandled = true;
                if (m_firstRunWizard) m_firstRunWizard->close();
            });
        connect(
            m_firstRunWizard,
            &QDialog::rejected,
            this,
            [this]() {
                // Closing the assistant is session-only. A future launch will
                // offer it again unless the user chose Set up later or completed
                // the required setup.
                m_firstRunWizardHandled = true;
            });
    }

    const auto setStatus = [](QLabel* label,
                              bool ready,
                              const QString& readyText,
                              const QString& missingText) {
        if (!label) return;
        label->setText(ready ? readyText : missingText);
        label->setStyleSheet(
            ready
                ? "font-weight: bold; color: #B5B5B5;"
                : "font-weight: bold; color: #E10600;");
    };

    setStatus(
        m_firstRunSeat1Status,
        seat1Ready,
        QStringLiteral("Ready"),
        QStringLiteral("Needs setup"));
    setStatus(
        m_firstRunSeat2Status,
        seat2Ready,
        QStringLiteral("Ready"),
        QStringLiteral("Needs setup"));
    setStatus(
        m_firstRunGamesStatus,
        gamesReady,
        QStringLiteral("%1 detected/saved").arg(gameCount),
        QStringLiteral("Add or detect a game"));
    setStatus(
        m_firstRunAudioStatus,
        audioReady,
        QStringLiteral("%1 available").arg(payload.audioEndpoints.size()),
        QStringLiteral("Optional; none detected"));

    if (m_firstRunSeat1Button) m_firstRunSeat1Button->setEnabled(!seat1Ready);
    if (m_firstRunSeat2Button) {
        m_firstRunSeat2Button->setEnabled(seat1Ready && !seat2Ready);
    }
    if (m_firstRunGamesButton) {
        m_firstRunGamesButton->setEnabled(seat1Ready && seat2Ready && !gamesReady);
    }

    if (!m_firstRunWizardHandled && !m_firstRunWizard->isVisible()) {
        m_firstRunWizard->show();
        m_firstRunWizard->raise();
        m_firstRunWizard->activateWindow();
    }
}

} // namespace hydra::ui
