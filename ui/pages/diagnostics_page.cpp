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
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5;");
    layout->addWidget(title);

    m_logText = new QTextEdit(this);
    m_logText->setReadOnly(true);
    m_logText->setStyleSheet("background-color: #151515; color: #B5B5B5; font-family: 'Consolas', 'Courier New', monospace; font-size: 13px; border: 1px solid #2A2A2A; border-radius: 6px; padding: 12px;");
    layout->addWidget(m_logText);
}

void DiagnosticsPage::updateState(const EngineStatePayload& payload) {
    QString logEntry;
    logEntry += QString("[%1] ENGINE STATE DUMP\n").arg(QDateTime::currentDateTime().toString("HH:mm:ss"));
    logEntry += "========================================\n\n";

    // 1. Physical controllers
    logEntry += "--- PHYSICAL CONTROLLERS ---\n";
    for (const auto& phys : payload.controllerInventory.physicalControllers) {
        logEntry += QString("ID: %1\nName: %2\n\n")
            .arg(QString::fromStdWString(phys.persistentId))
            .arg(QString::fromStdWString(phys.displayName));
    }

    // 2. Runtime sources
    logEntry += "--- RUNTIME SOURCES ---\n";
    for (const auto& src : payload.controllerInventory.sources) {
        QString apiStr = src.api == hydra::controller::ApiSurface::XInput ? "XInput" : "DirectInput";
        logEntry += QString("API: %1\nKey: %2\nSlot: %3\nConnected: %4\nGeneration: %5\n\n")
            .arg(apiStr)
            .arg(QString::fromStdString(src.runtimeKey))
            .arg(src.runtimeXInputSlot ? QString::number(*src.runtimeXInputSlot) : "N/A")
            .arg(src.connected ? "Yes" : "No")
            .arg(src.sourceGeneration);
    }

    // 3. Seat bindings
    logEntry += "--- SEAT BINDINGS ---\n";
    for (uint32_t i = 1; i <= 2; ++i) {
        auto snap = m_sessionController->snapshot(i);
        if (snap && snap->controllerBinding) {
            const auto& b = *snap->controllerBinding;
            QString pIdStr = b.persistentControllerId ? QString::fromStdWString(*b.persistentControllerId) : "None";
            logEntry += QString("Seat: %1\nPersistent ID: %2\nAPI Key: %3\nSlot: %4\nGeneration: %5\n\n")
                .arg(i)
                .arg(pIdStr)
                .arg(QString::fromStdString(b.runtimeKey))
                .arg(b.runtimeXInputSlot ? QString::number(*b.runtimeXInputSlot) : "N/A")
                .arg(b.sourceGeneration);
        } else {
            logEntry += QString("Seat: %1\nNo binding.\n\n").arg(i);
        }
    }

    // 4. Process diagnostics
    logEntry += "--- PROCESS DIAGNOSTICS ---\n";
    for (const auto& session : payload.audioSessions) {
        QString cid = session.processIdentity ? QString::number(session.processIdentity->creationIdentity) : "N/A";
        
        QString owner = "None";
        for (uint32_t i = 1; i <= 2; ++i) {
            auto snap = m_sessionController->snapshot(i);
            if (snap && snap->process && session.processIdentity && *snap->process == *session.processIdentity) {
                owner = QString("Seat %1").arg(i);
            }
        }

        logEntry += QString("PID: %1\nCreation ID: %2\nOwner: %3\n\n")
            .arg(session.processId)
            .arg(cid)
            .arg(owner);
    }

    m_logText->setText(logEntry);
}

} // namespace hydra::ui
