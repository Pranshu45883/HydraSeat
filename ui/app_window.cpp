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

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>

namespace hydra::ui {

AppWindow::AppWindow(QWidget* parent)
    : QMainWindow(parent) {
    m_hostControl = std::make_shared<HostControlClient>();
    m_hardwareDetector = std::make_shared<hydra::HardwareDetector>();
    m_routingController =
        std::make_unique<RoutingController>(m_hostControl, this);
    m_enginePoller =
        std::make_unique<EnginePoller>(m_hardwareDetector, this);

    setupUi();

    QTimer::singleShot(0, this, [this]() {
        m_enginePoller->startPolling(2000);
    });
}

AppWindow::~AppWindow() = default;

void AppWindow::setupUi() {
    setWindowTitle("HydraSeat");
    resize(1150, 750);
    setMinimumSize(1100, 700);

    setStyleSheet(R"(
        QMainWindow { background-color: #0A0A0A; color: #F5F5F5; font-family: 'Segoe UI', Arial, sans-serif; }
        QLabel { color: #F5F5F5; font-family: 'Segoe UI', Arial, sans-serif; }
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

    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, dashboardPage, &DashboardPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, seatsPage, &SeatsPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, applicationsPage, &ApplicationsPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, audioPage, &AudioPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, hardwarePage, &HardwarePage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, diagnosticsPage, &DiagnosticsPage::updateState);

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
                m_statusLabel->setText("● Running");
                m_statusLabel->setToolTip({});
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

    m_statusLabel = new QLabel("● Running", statusContainer);
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

} // namespace hydra::ui
