#include "ui/app_window.hpp"
#include "ui/engine_poller.hpp"
#include <QTimer>

#include "ui/pages/dashboard_page.hpp"
#include "ui/pages/seats_page.hpp"
#include "ui/pages/applications_page.hpp"
#include "ui/pages/audio_page.hpp"
#include "ui/pages/hardware_page.hpp"
#include "ui/pages/diagnostics_page.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>

namespace hydra::ui {

AppWindow::AppWindow(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent)
    : QMainWindow(parent), m_sessionController(std::move(sessionController))
{
    m_authorityBridge = std::make_shared<hydra::runtime::AuthorityBridge>(m_sessionController);
    m_hardwareDetector = std::make_shared<hydra::HardwareDetector>();
    m_routingController = std::make_unique<RoutingController>(m_authorityBridge, this);
    m_enginePoller = std::make_unique<EnginePoller>(m_hardwareDetector, this);

    setupUi();

    QTimer::singleShot(0, this, [this]() {
        m_enginePoller->startPolling(2000);
    });
}

AppWindow::~AppWindow() = default;

void AppWindow::setupUi() {
    setWindowTitle("HydraSeat");
    resize(1024, 768);

    setStyleSheet(R"(
        QMainWindow { background-color: #0A0A0A; color: #F5F5F5; }
        QListWidget { background-color: #111111; color: #B5B5B5; border: none; font-size: 14px; border-right: 1px solid #2A2A2A; }
        QListWidget::item { padding: 12px; }
        QListWidget::item:hover:!selected { background-color: #292929; color: #F5F5F5; }
        QListWidget::item:selected { background-color: #151515; color: #FFFFFF; border-left: 4px solid #E10600; font-weight: bold; }
        QLabel { color: #F5F5F5; }
        QScrollBar:vertical { background: #111111; width: 12px; margin: 0px 0px 0px 0px; }
        QScrollBar::handle:vertical { background: #2A2A2A; min-height: 20px; border-radius: 6px; }
        QScrollBar::handle:vertical:hover { background: #E10600; }
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

    setupSidebar();
    m_sidebar->setParent(innerWidget);
    
    // Main Workspace Stack
    m_workspaceStack = new QStackedWidget(innerWidget);
    
    // Instantiate Pages
    auto* dashboardPage = new DashboardPage(m_sessionController, m_workspaceStack);
    auto* seatsPage = new SeatsPage(m_sessionController, m_authorityBridge, m_workspaceStack);
    auto* applicationsPage = new ApplicationsPage(m_sessionController, m_workspaceStack);
    auto* audioPage = new AudioPage(m_sessionController, m_routingController.get(), m_workspaceStack);
    auto* hardwarePage = new HardwarePage(m_sessionController, m_workspaceStack);
    auto* diagnosticsPage = new DiagnosticsPage(m_sessionController, m_workspaceStack);

    m_workspaceStack->addWidget(dashboardPage);
    m_workspaceStack->addWidget(seatsPage);
    m_workspaceStack->addWidget(applicationsPage);
    m_workspaceStack->addWidget(audioPage);
    m_workspaceStack->addWidget(hardwarePage);
    m_workspaceStack->addWidget(diagnosticsPage);

    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, dashboardPage, &DashboardPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, seatsPage, &SeatsPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, applicationsPage, &ApplicationsPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, audioPage, &AudioPage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, hardwarePage, &HardwarePage::updateState);
    connect(m_enginePoller.get(), &EnginePoller::stateUpdated, diagnosticsPage, &DiagnosticsPage::updateState);

    mainLayout->addWidget(m_sidebar);
    mainLayout->addWidget(m_workspaceStack, 1);

    outerLayout->addWidget(innerWidget, 1);

    setupStatusbar();
    
    m_sidebar->setCurrentRow(0);
}

void AppWindow::setupSidebar() {
    m_sidebar = new QListWidget();
    m_sidebar->setFixedWidth(200);
    m_sidebar->setFocusPolicy(Qt::NoFocus);

    m_sidebar->addItem("Dashboard");
    m_sidebar->addItem("Seats");
    m_sidebar->addItem("Applications");
    m_sidebar->addItem("Audio");
    m_sidebar->addItem("Hardware");
    m_sidebar->addItem("Diagnostics");
    m_sidebar->addItem("Settings");

    connect(m_sidebar, &QListWidget::currentRowChanged, this, &AppWindow::onNavigationChanged);
}

void AppWindow::setupStatusbar() {
    auto* statusContainer = new QWidget(this);
    statusContainer->setStyleSheet("background-color: #111111; border-top: 1px solid #2A2A2A; padding: 4px;");
    auto* statusLayout = new QHBoxLayout(statusContainer);
    statusLayout->setContentsMargins(16, 4, 16, 4);

    auto* brandLabel = new QLabel("HydraSeat Engine", statusContainer);
    brandLabel->setStyleSheet("font-weight: bold; color: #777777; border: none;");
    
    m_statusLabel = new QLabel("● Running", statusContainer);
    m_statusLabel->setStyleSheet("color: #E10600; font-weight: bold; border: none;");

    statusLayout->addWidget(brandLabel);
    statusLayout->addStretch();
    statusLayout->addWidget(m_statusLabel);

    // Append status container to the outer VBox layout
    auto* outerLayout = qobject_cast<QVBoxLayout*>(centralWidget()->layout());
    if (outerLayout) {
        outerLayout->addWidget(statusContainer);
    }
}

void AppWindow::onNavigationChanged(int index) {
    if (m_workspaceStack && index >= 0 && index < m_workspaceStack->count()) {
        m_workspaceStack->setCurrentIndex(index);
    }
}

} // namespace hydra::ui
