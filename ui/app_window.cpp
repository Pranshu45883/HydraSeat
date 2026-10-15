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
    if (m_firstRunWizardHandled || !payload.hostConnected || payload.hardwareError) {
        return;
    }

    QSettings settings(
        QSettings::NativeFormat,
        QSettings::UserScope,
        QStringLiteral("HydraSeat"),
        QStringLiteral("HydraSeat"));
    if (settings.value(QStringLiteral("ui/firstRunGuideDismissed"), false).toBool()) {
        m_firstRunWizardHandled = true;
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

    if (seat1Ready && seat2Ready && gameCount > 0u) {
        settings.setValue(QStringLiteral("ui/firstRunGuideDismissed"), true);
        settings.sync();
        m_firstRunWizardHandled = true;
        return;
    }

    m_firstRunWizardHandled = true;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Welcome to HydraSeat"));
    dialog.setMinimumWidth(560);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(14);

    auto* heading = new QLabel(QStringLiteral("Finish your two-Seat setup"), &dialog);
    heading->setStyleSheet(
        "font-size: 22px; font-weight: bold; color: #F5F5F5;");
    layout->addWidget(heading);

    auto* intro = new QLabel(
        QStringLiteral(
            "HydraSeat is ready to guide the initial setup. Configure both physical Seats, "
            "choose a detected or custom game, then launch from the Seats page."),
        &dialog);
    intro->setWordWrap(true);
    intro->setStyleSheet("font-size: 13px; color: #B5B5B5;");
    layout->addWidget(intro);

    auto* checklist = new QGroupBox(QStringLiteral("Setup readiness"), &dialog);
    auto* grid = new QGridLayout(checklist);
    const auto addStatus = [&](int row, const QString& name, bool ready,
                               const QString& readyText, const QString& missingText) {
        auto* nameLabel = new QLabel(name, checklist);
        auto* statusLabel = new QLabel(ready ? readyText : missingText, checklist);
        statusLabel->setStyleSheet(
            ready ? "font-weight: bold; color: #B5B5B5;"
                  : "font-weight: bold; color: #E10600;");
        grid->addWidget(nameLabel, row, 0);
        grid->addWidget(statusLabel, row, 1);
    };
    addStatus(0, QStringLiteral("Seat 1 hardware"), seat1Ready,
              QStringLiteral("Ready"), QStringLiteral("Needs setup"));
    addStatus(1, QStringLiteral("Seat 2 hardware"), seat2Ready,
              QStringLiteral("Ready"), QStringLiteral("Needs setup"));
    addStatus(2, QStringLiteral("Games"), gameCount > 0u,
              QStringLiteral("%1 detected/saved").arg(gameCount),
              QStringLiteral("Add or detect a game"));
    addStatus(3, QStringLiteral("Audio outputs"), !payload.audioEndpoints.empty(),
              QStringLiteral("%1 available").arg(payload.audioEndpoints.size()),
              QStringLiteral("Optional; none detected"));
    layout->addWidget(checklist);

    auto* hint = new QLabel(
        QStringLiteral(
            "Tip: on the Seats page you can identify duplicate keyboards by pressing a key "
            "and duplicate mice by moving them. Preferred audio can also be saved per Seat."),
        &dialog);
    hint->setWordWrap(true);
    hint->setStyleSheet("font-size: 12px; color: #777777;");
    layout->addWidget(hint);

    auto* actions = new QHBoxLayout();
    auto* seatsButton = new QPushButton(QStringLiteral("Set up Seats"), &dialog);
    auto* gamesButton = new QPushButton(QStringLiteral("Review Games"), &dialog);
    auto* laterButton = new QPushButton(QStringLiteral("Set up later"), &dialog);
    actions->addWidget(seatsButton);
    actions->addWidget(gamesButton);
    actions->addStretch();
    actions->addWidget(laterButton);
    layout->addLayout(actions);

    enum class WizardAction { None, Seats, Games, Later };
    WizardAction action = WizardAction::None;
    connect(seatsButton, &QPushButton::clicked, &dialog, [&]() {
        action = WizardAction::Seats;
        dialog.accept();
    });
    connect(gamesButton, &QPushButton::clicked, &dialog, [&]() {
        action = WizardAction::Games;
        dialog.accept();
    });
    connect(laterButton, &QPushButton::clicked, &dialog, [&]() {
        action = WizardAction::Later;
        dialog.accept();
    });

    dialog.exec();
    if (action == WizardAction::Later) {
        settings.setValue(QStringLiteral("ui/firstRunGuideDismissed"), true);
        settings.sync();
    } else if (action == WizardAction::Seats) {
        navigateToPage(1);
    } else if (action == WizardAction::Games) {
        navigateToPage(2);
    }
}

} // namespace hydra::ui
