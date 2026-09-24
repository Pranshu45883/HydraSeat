#include "ui/pages/diagnostics_page.hpp"
#include <QLabel>
#include <QDateTime>

namespace hydra::ui {

DiagnosticsPage::DiagnosticsPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent)
    : QWidget(parent), m_sessionController(std::move(sessionController)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(24);

    auto* title = new QLabel("Diagnostics", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: white;");
    layout->addWidget(title);

    m_logText = new QTextEdit(this);
    m_logText->setReadOnly(true);
    m_logText->setStyleSheet("background-color: #1A1A1A; color: #CCCCCC; font-family: 'Consolas', 'Courier New', monospace; font-size: 13px; border: none; border-radius: 6px; padding: 12px;");
    layout->addWidget(m_logText);
}

void DiagnosticsPage::updateState(const EngineStatePayload& payload) {
    // Only update occasionally or append
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss");
    
    QString logEntry = QString("[%1] Engine Poll Triggered\n").arg(timeStr);
    
    if (payload.hardwareError) {
        logEntry += "  [ERROR] Hardware detector failed\n";
    } else {
        logEntry += QString("  [OK] Hardware: %1 displays, %2 keyboards, %3 mice, %4 controllers\n")
                    .arg(payload.displays.size())
                    .arg(payload.keyboards.size())
                    .arg(payload.mice.size())
                    .arg(payload.controllers.size());
    }

    if (payload.audioEndpointError) {
        logEntry += "  [ERROR] Audio endpoint enumeration failed\n";
    } else {
        logEntry += QString("  [OK] Audio: %1 endpoints\n").arg(payload.audioEndpoints.size());
    }

    if (payload.audioSessionError) {
        logEntry += "  [ERROR] Audio session enumeration failed\n";
    } else {
        logEntry += QString("  [OK] Audio: %1 sessions\n").arg(payload.audioSessions.size());
    }

    logEntry += "----------------------------------------\n";
    
    m_logText->append(logEntry);
    
    // Prevent unbounded growth
    if (m_logText->document()->blockCount() > 1000) {
        m_logText->clear();
    }
}

} // namespace hydra::ui
