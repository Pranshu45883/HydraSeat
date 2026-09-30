#include "ui/pages/diagnostics_page.hpp"
#include <QFrame>

namespace hydra::ui {

DiagnosticsPage::DiagnosticsPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent)
    : QWidget(parent), m_sessionController(std::move(sessionController)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(24);

    auto* title = new QLabel("Diagnostics", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5; font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);
    
    auto* subtitle = new QLabel("Authoritative runtime snapshots and state data", this);
    subtitle->setStyleSheet("font-size: 14px; color: #B5B5B5; font-family: 'Segoe UI', sans-serif; margin-bottom: 8px;");
    layout->addWidget(subtitle);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");

    auto* listContainer = new QWidget(scrollArea);
    listContainer->setStyleSheet("background-color: transparent;");
    m_listLayout = new QVBoxLayout(listContainer);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(16);
    
    for (uint32_t i = 1; i <= 2; ++i) {
        auto* frame = buildSeatDiagnostics(i);
        m_listLayout->addWidget(frame);
        m_seatFrames.append(frame);
    }
    
    m_listLayout->addStretch();
    scrollArea->setWidget(listContainer);
    layout->addWidget(scrollArea);
}

static void addRow(QGridLayout* layout, int& row, const QString& label, const QString& value) {
    auto* l = new QLabel(label);
    l->setStyleSheet("font-size: 12px; color: #777777; font-family: 'Consolas', monospace;");
    auto* v = new QLabel(value);
    v->setStyleSheet("font-size: 13px; color: #F5F5F5; font-family: 'Consolas', monospace;");
    layout->addWidget(l, row, 0);
    layout->addWidget(v, row, 1);
    row++;
}

QFrame* DiagnosticsPage::buildSeatDiagnostics(uint32_t seatId) {
    auto* frame = new QFrame();
    frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 16px; border: 1px solid #292929;");
    auto* fl = new QVBoxLayout(frame);
    fl->setContentsMargins(0,0,0,0);
    fl->setSpacing(16);
    
    auto* title = new QLabel(QString("SEAT %1").arg(seatId), frame);
    title->setStyleSheet("font-size: 15px; font-weight: bold; color: #F5F5F5; text-transform: uppercase; border: none;");
    fl->addWidget(title);
    
    auto* gridLayout = new QGridLayout();
    gridLayout->setColumnMinimumWidth(0, 160);
    gridLayout->setSpacing(8);
    fl->addLayout(gridLayout);
    
    // Store grid layout in frame for easy access later
    frame->setProperty("gridLayout", QVariant::fromValue(static_cast<void*>(gridLayout)));
    
    return frame;
}

void DiagnosticsPage::updateSeatDiagnostics(uint32_t seatId, QFrame* frame) {
    auto* gridLayout = static_cast<QGridLayout*>(frame->property("gridLayout").value<void*>());
    if (!gridLayout) return;
    
    // Clear layout
    while (QLayoutItem* item = gridLayout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    
    auto snapshot = m_sessionController->snapshot(seatId);
    if (!snapshot) {
        auto* l = new QLabel("Status");
        l->setStyleSheet("font-size: 12px; color: #777777; font-family: 'Consolas', monospace;");
        auto* v = new QLabel("UNAVAILABLE");
        v->setStyleSheet("font-size: 13px; color: #E10600; font-family: 'Consolas', monospace;");
        gridLayout->addWidget(l, 0, 0);
        gridLayout->addWidget(v, 0, 1);
        return;
    }
    
    int r = 0;
    
    auto addHeader = [&](const QString& text) {
        auto* l = new QLabel(text);
        l->setStyleSheet("font-size: 12px; font-weight: bold; color: #777777; margin-top: 8px; margin-bottom: 2px; text-transform: uppercase;");
        gridLayout->addWidget(l, r++, 0, 1, 2);
    };
    
    addHeader("RUNTIME AUTHORITY");
    addRow(gridLayout, r, "Seat ID", QString::number(snapshot->seatId));
    addRow(gridLayout, r, "Generation", QString::number(snapshot->generation));
    addRow(gridLayout, r, "UI Lease", snapshot->uiLeaseActive ? "ACTIVE" : "INACTIVE");
    addRow(gridLayout, r, "Game Lease", snapshot->gameLeaseActive ? "ACTIVE" : "INACTIVE");
    
    addHeader("PROCESS IDENTITY");
    if (snapshot->process) {
        addRow(gridLayout, r, "PID", QString::number(snapshot->process->pid));
        addRow(gridLayout, r, "Creation Identity", QString::number(snapshot->process->creationIdentity));
    } else {
        addRow(gridLayout, r, "Status", "None");
    }
    
    addHeader("CONTROLLER");
    if (snapshot->controllerBinding) {
        const auto& b = *snapshot->controllerBinding;
        QString pIdStr = b.persistentControllerId ? QString::fromStdWString(*b.persistentControllerId) : "None";
        addRow(gridLayout, r, "persistentId", pIdStr);
        addRow(gridLayout, r, "runtimeXInputSlot", b.runtimeXInputSlot ? QString::number(*b.runtimeXInputSlot) : "N/A");
        addRow(gridLayout, r, "sourceGeneration", QString::number(b.sourceGeneration));
    } else {
        addRow(gridLayout, r, "Status", "None");
    }
    
    addHeader("AUDIO");
    if (snapshot->audioEndpoint) {
        addRow(gridLayout, r, "endpointId", QString::fromStdWString(snapshot->audioEndpoint->endpointId));
        QString sIdStr = snapshot->audioEndpoint->stableId ? QString::fromStdWString(*snapshot->audioEndpoint->stableId) : "None";
        addRow(gridLayout, r, "endpointStableId", sIdStr);
    } else {
        addRow(gridLayout, r, "Status", "None");
    }
}

void DiagnosticsPage::updateState(const EngineStatePayload& payload) {
    (void)payload;
    if (m_seatFrames.size() >= 2) {
        updateSeatDiagnostics(1, m_seatFrames[0]);
        updateSeatDiagnostics(2, m_seatFrames[1]);
    }
}

} // namespace hydra::ui
