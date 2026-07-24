#include "ui/main_window.hpp"

#include <QMessageBox>
#include <QApplication>
#include <QIcon>
#include <QDebug>

namespace hydra {
namespace ui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("HydraSeat - Local Gaming Multiseat Framework");
    resize(960, 680);
    setStyleSheet("QMainWindow { background-color: #0F172A; }");

    setupUi();

    m_inputRouter.initialize();
    refreshHardware();

    // Add default Player 1 and Player 2 workspaces
    onAddWorkspaceClicked();
    onAddWorkspaceClicked();
}

void MainWindow::setupUi() {
    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    // Header layout
    auto* headerLayout = new QHBoxLayout();
    auto* titleLabel = new QLabel("🎮 HydraSeat Multiseat Control Center", this);
    titleLabel->setStyleSheet("font-size: 20px; font-weight: bold; color: #38BDF8;");
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();

    auto* refreshBtn = new QPushButton("🔄 Refresh Devices", this);
    refreshBtn->setStyleSheet(
        "QPushButton { background-color: #334155; color: #F8FAFC; border: 1px solid #475569; border-radius: 6px; padding: 8px 16px; font-weight: bold; }"
        "QPushButton:hover { background-color: #475569; }"
    );
    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefreshHardwareClicked);
    headerLayout->addWidget(refreshBtn);

    auto* addWsBtn = new QPushButton("➕ Add Workspace", this);
    addWsBtn->setStyleSheet(
        "QPushButton { background-color: #2563EB; color: white; border: none; border-radius: 6px; padding: 8px 16px; font-weight: bold; }"
        "QPushButton:hover { background-color: #1D4ED8; }"
    );
    connect(addWsBtn, &QPushButton::clicked, this, &MainWindow::onAddWorkspaceClicked);
    headerLayout->addWidget(addWsBtn);

    mainLayout->addLayout(headerLayout);

    // Workspace scroll container
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");

    auto* scrollContent = new QWidget();
    scrollContent->setStyleSheet("background-color: transparent;");
    m_workspaceContainerLayout = new QVBoxLayout(scrollContent);
    m_workspaceContainerLayout->setContentsMargins(0, 0, 0, 0);
    m_workspaceContainerLayout->setSpacing(12);
    m_workspaceContainerLayout->addStretch();

    scrollArea->setWidget(scrollContent);
    mainLayout->addWidget(scrollArea, 1);

    // Action Footer
    auto* footerLayout = new QHBoxLayout();
    m_statusLabel = new QLabel("Ready.", this);
    m_statusLabel->setStyleSheet("color: #94A3B8; font-size: 13px;");
    footerLayout->addWidget(m_statusLabel);
    footerLayout->addStretch();

    auto* launchBtn = new QPushButton("🚀 Launch Multiseat Game", this);
    launchBtn->setStyleSheet(
        "QPushButton { background-color: #10B981; color: white; border: none; border-radius: 6px; padding: 12px 24px; font-size: 15px; font-weight: bold; }"
        "QPushButton:hover { background-color: #059669; }"
    );
    connect(launchBtn, &QPushButton::clicked, this, &MainWindow::onLaunchGameClicked);
    footerLayout->addWidget(launchBtn);

    mainLayout->addLayout(footerLayout);

    statusBar()->showMessage("HydraSeat Engine v0.1.0 Ready.");
}

void MainWindow::refreshHardware() {
    m_cachedDisplays = m_hardwareDetector.detectDisplays();
    m_cachedKeyboards = m_hardwareDetector.detectKeyboards();
    m_cachedMice = m_hardwareDetector.detectMice();
    m_cachedControllers = m_hardwareDetector.detectControllers();

    for (auto* wsWidget : m_workspaceWidgets) {
        wsWidget->updateDeviceLists(m_cachedDisplays, m_cachedKeyboards, m_cachedMice, m_cachedControllers);
    }

    QString statusMsg = QString("Detected: %1 Displays | %2 Keyboards | %3 Mice | %4 Controllers")
        .arg(m_cachedDisplays.size())
        .arg(m_cachedKeyboards.size())
        .arg(m_cachedMice.size())
        .arg(m_cachedControllers.size());

    m_statusLabel->setText(statusMsg);
}

void MainWindow::onAddWorkspaceClicked() {
    uint32_t id = m_workspaceManager.createWorkspace(L"Player Workspace");
    auto* widget = new WorkspaceWidget(id, this);
    widget->updateDeviceLists(m_cachedDisplays, m_cachedKeyboards, m_cachedMice, m_cachedControllers);

    connect(widget, &WorkspaceWidget::removeRequested, this, &MainWindow::onRemoveWorkspaceRequested);

    // Insert before bottom stretch
    int stretchIndex = m_workspaceContainerLayout->count() - 1;
    m_workspaceContainerLayout->insertWidget(stretchIndex, widget);
    m_workspaceWidgets.insert(id, widget);
}

void MainWindow::onRemoveWorkspaceRequested(uint32_t workspaceId) {
    if (m_workspaceWidgets.contains(workspaceId)) {
        auto* widget = m_workspaceWidgets.take(workspaceId);
        m_workspaceContainerLayout->removeWidget(widget);
        widget->deleteLater();
        m_workspaceManager.removeWorkspace(workspaceId);
    }
}

void MainWindow::onRefreshHardwareClicked() {
    refreshHardware();
    statusBar()->showMessage("Hardware device list refreshed.", 3000);
}

void MainWindow::onLaunchGameClicked() {
    if (m_workspaceWidgets.isEmpty()) {
        QMessageBox::warning(this, "No Workspaces", "Please create at least one player workspace before launching.");
        return;
    }

    QMessageBox::information(this, "HydraSeat Launcher", "Multiseat inputs routed! Launching workspace games...");
}

} // namespace ui
} // namespace hydra
