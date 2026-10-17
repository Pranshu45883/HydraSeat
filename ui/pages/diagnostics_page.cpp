#include "ui/pages/diagnostics_page.hpp"

#include "hydra/support_bundle.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QMessageBox>
#include <QOperatingSystemVersion>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSysInfo>
#include <QVariant>

#include <algorithm>

namespace hydra::ui {

DiagnosticsPage::DiagnosticsPage(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(24);

    auto* title = new QLabel("Diagnostics", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5; font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);

    auto* subtitle = new QLabel(
        "Read-only snapshots from the canonical HydraSeat host", this);
    subtitle->setStyleSheet("font-size: 14px; color: #B5B5B5; font-family: 'Segoe UI', sans-serif; margin-bottom: 8px;");
    layout->addWidget(subtitle);

    auto* exportButton = new QPushButton("Export Support Bundle", this);
    exportButton->setMaximumWidth(220);
    exportButton->setStyleSheet(
        "QPushButton { background-color: #202020; color: #F5F5F5; "
        "border: 1px solid #333333; border-radius: 6px; padding: 8px 14px; "
        "font-weight: bold; } QPushButton:hover { background-color: #292929; }");
    exportButton->setToolTip(
        "Preview and export a privacy-redacted HydraSeat support bundle.");
    connect(
        exportButton, &QPushButton::clicked,
        this, &DiagnosticsPage::onExportSupportBundle);
    layout->addWidget(exportButton);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet(
        "QScrollArea { border: none; background-color: transparent; }");

    auto* listContainer = new QWidget(scrollArea);
    listContainer->setStyleSheet("background-color: transparent;");
    m_listLayout = new QVBoxLayout(listContainer);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(16);

    for (std::uint32_t i = 1; i <= 2; ++i) {
        auto* frame = buildSeatDiagnostics(i);
        m_listLayout->addWidget(frame);
        m_seatFrames.append(frame);
    }

    m_listLayout->addStretch();
    scrollArea->setWidget(listContainer);
    layout->addWidget(scrollArea);
}

static void addRow(
    QGridLayout* layout,
    int& row,
    const QString& label,
    const QString& value) {
    auto* l = new QLabel(label);
    l->setStyleSheet(
        "font-size: 12px; color: #777777; font-family: 'Consolas', monospace;");
    auto* v = new QLabel(value);
    v->setStyleSheet(
        "font-size: 13px; color: #F5F5F5; font-family: 'Consolas', monospace;");
    layout->addWidget(l, row, 0);
    layout->addWidget(v, row, 1);
    ++row;
}

QFrame* DiagnosticsPage::buildSeatDiagnostics(std::uint32_t seatId) {
    auto* frame = new QFrame();
    frame->setStyleSheet(
        "background-color: #151515; border-radius: 6px; padding: 16px; "
        "border: 1px solid #292929;");
    auto* fl = new QVBoxLayout(frame);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->setSpacing(16);

    auto* title =
        new QLabel(QString("SEAT %1").arg(seatId), frame);
    title->setStyleSheet(
        "font-size: 15px; font-weight: bold; color: #F5F5F5; "
        "text-transform: uppercase; border: none;");
    fl->addWidget(title);

    auto* gridLayout = new QGridLayout();
    gridLayout->setColumnMinimumWidth(0, 180);
    gridLayout->setSpacing(8);
    fl->addLayout(gridLayout);

    frame->setProperty(
        "gridLayout",
        QVariant::fromValue(static_cast<void*>(gridLayout)));

    return frame;
}

void DiagnosticsPage::updateSeatDiagnostics(
    const hydra::hostipc::SeatSnapshot* snapshot,
    const hydra::hostipc::SeatRuntimeInfo* runtimeInfo,
    QFrame* frame) {
    auto* gridLayout = static_cast<QGridLayout*>(
        frame->property("gridLayout").value<void*>());
    if (!gridLayout) return;

    while (QLayoutItem* item = gridLayout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (!snapshot) {
        auto* l = new QLabel("Status");
        l->setStyleSheet(
            "font-size: 12px; color: #777777; font-family: 'Consolas', monospace;");
        auto* v = new QLabel("HOST UNAVAILABLE");
        v->setStyleSheet(
            "font-size: 13px; color: #E10600; font-family: 'Consolas', monospace;");
        gridLayout->addWidget(l, 0, 0);
        gridLayout->addWidget(v, 0, 1);
        return;
    }

    int row = 0;
    auto addHeader = [&](const QString& text) {
        auto* l = new QLabel(text);
        l->setStyleSheet(
            "font-size: 12px; font-weight: bold; color: #777777; "
            "margin-top: 8px; margin-bottom: 2px; text-transform: uppercase;");
        gridLayout->addWidget(l, row++, 0, 1, 2);
    };

    addHeader("HOST AUTHORITY");
    addRow(gridLayout, row, "Seat ID", QString::number(snapshot->seatId));
    addRow(gridLayout, row, "Generation", QString::number(snapshot->generation));
    addRow(
        gridLayout,
        row,
        "UI Lease",
        snapshot->uiLeaseActive ? "ACTIVE" : "INACTIVE");
    addRow(
        gridLayout,
        row,
        "Game Lease",
        snapshot->gameLeaseActive ? "ACTIVE" : "INACTIVE");

    addHeader("GAME");
    if (snapshot->processOwned && runtimeInfo &&
        !runtimeInfo->titleUtf8.empty()) {
        addRow(
            gridLayout, row, "Title",
            QString::fromUtf8(runtimeInfo->titleUtf8.c_str()));
        const QString provider =
            runtimeInfo->providerIdUtf8 == "steam"
                ? QStringLiteral("Steam")
                : runtimeInfo->providerIdUtf8 == "custom"
                    ? QStringLiteral("Custom")
                    : QString::fromUtf8(runtimeInfo->providerIdUtf8.c_str());
        addRow(gridLayout, row, "Provider", provider);
    } else {
        addRow(gridLayout, row, "Title", snapshot->processOwned ? "Unknown" : "None");
    }

    addHeader("PROCESS IDENTITY");
    if (snapshot->processOwned) {
        addRow(gridLayout, row, "PID", QString::number(snapshot->processId));
        addRow(
            gridLayout,
            row,
            "Creation Identity",
            QString::number(snapshot->processCreationIdentity));
        addRow(
            gridLayout,
            row,
            "Window",
            snapshot->windowOwned
                ? QString("0x%1").arg(
                      static_cast<qulonglong>(snapshot->targetHwnd),
                      0,
                      16)
                : "None");
    } else {
        addRow(gridLayout, row, "Status", "None");
    }

    addHeader("CONTROLLER");
    addRow(
        gridLayout,
        row,
        "Binding",
        snapshot->controllerBound ? "BOUND" : "None");

    addHeader("AUDIO");
    addRow(
        gridLayout,
        row,
        "Authority",
        snapshot->processOwned
            ? "Host-owned mutation; observe on Audio page"
            : "No owned process");
}

void DiagnosticsPage::updateState(const EngineStatePayload& payload) {
    m_lastPayload = payload;
    const hydra::hostipc::SeatSnapshot* seat1 = nullptr;
    const hydra::hostipc::SeatSnapshot* seat2 = nullptr;
    if (payload.hostSnapshot) {
        seat1 = &payload.hostSnapshot->seats[0];
        seat2 = &payload.hostSnapshot->seats[1];
    }
    const hydra::hostipc::SeatRuntimeInfo* seat1Runtime =
        payload.seatRuntimeInfo[0] ? &*payload.seatRuntimeInfo[0] : nullptr;
    const hydra::hostipc::SeatRuntimeInfo* seat2Runtime =
        payload.seatRuntimeInfo[1] ? &*payload.seatRuntimeInfo[1] : nullptr;

    if (m_seatFrames.size() >= 2) {
        updateSeatDiagnostics(seat1, seat1Runtime, m_seatFrames[0]);
        updateSeatDiagnostics(seat2, seat2Runtime, m_seatFrames[1]);
    }
}


void DiagnosticsPage::onExportSupportBundle() {
    hydra::support::SupportBundleInput input;
    input.hydraSeatVersion = "0.1.0";
    input.hydraSeatBuild = "v1-ui-export";

    const auto os = QOperatingSystemVersion::current();
    const int build = os.microVersion();
    if (build >= 22000) {
        input.windowsBuildClass = "win11-" + std::to_string(build);
    } else if (build > 0) {
        input.windowsBuildClass = "win10-" + std::to_string(build);
    } else {
        input.windowsBuildClass = "win11-current";
    }

    const QString qtArchitecture = QSysInfo::currentCpuArchitecture().toLower();
    if (qtArchitecture == "x86_64" || qtArchitecture == "amd64") {
        input.architecture = "x64";
    } else if (qtArchitecture == "i386" || qtArchitecture == "i686" ||
               qtArchitecture == "x86") {
        input.architecture = "x86";
    } else if (qtArchitecture == "arm64" || qtArchitecture == "aarch64") {
        input.architecture = "arm64";
    } else {
        QMessageBox::critical(
            this,
            "Support bundle unavailable",
            "The current CPU architecture cannot be represented by the support bundle schema.");
        return;
    }

    const std::uint64_t hostGeneration =
        m_lastPayload.hostSnapshot
            ? std::max<std::uint64_t>(
                  1u, m_lastPayload.hostSnapshot->authorityRevision)
            : 1u;
    input.events.push_back({
        m_lastPayload.hostConnected ? "host-connected" : "host-unavailable",
        0u,
        hostGeneration,
    });

    const auto addSubsystemEvent =
        [&](bool failed, const char* eventCode) {
            if (failed) {
                input.events.push_back({eventCode, 0u, hostGeneration});
            }
        };
    addSubsystemEvent(m_lastPayload.hardwareError, "hardware-inventory-error");
    addSubsystemEvent(
        m_lastPayload.controllerInventoryError,
        "controller-inventory-error");
    addSubsystemEvent(m_lastPayload.gameCatalogError, "game-catalog-error");
    addSubsystemEvent(m_lastPayload.audioEndpointError, "audio-endpoint-error");
    addSubsystemEvent(m_lastPayload.audioSessionError, "audio-session-error");

    if (m_lastPayload.hostSnapshot) {
        for (const auto& seat : m_lastPayload.hostSnapshot->seats) {
            if (!seat.processOwned) continue;
            input.events.push_back({
                "seat-running",
                seat.seatId,
                std::max<std::uint64_t>(1u, seat.generation),
            });
        }
    }

    hydra::support::SupportBundle bundle;
    const auto built = hydra::support::buildSupportBundle(input, bundle);
    if (!built.succeeded()) {
        QMessageBox::critical(
            this,
            "Support bundle unavailable",
            QString::fromStdString(built.message));
        return;
    }

    hydra::support::SupportExportSession exportSession;
    hydra::support::SupportExportPreview preview;
    const auto prepared = exportSession.prepare(bundle, preview);
    if (!prepared.succeeded()) {
        QMessageBox::critical(
            this,
            "Support bundle unavailable",
            QString::fromStdString(prepared.message));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("Review Support Bundle");
    dialog.resize(760, 560);
    auto* dialogLayout = new QVBoxLayout(&dialog);
    auto* summary = new QLabel(
        QString::fromStdString(preview.humanSummary),
        &dialog);
    summary->setWordWrap(true);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dialogLayout->addWidget(summary);

    auto* exact = new QPlainTextEdit(&dialog);
    exact->setReadOnly(true);
    exact->setPlainText(QString::fromStdString(preview.exactJson));
    exact->setToolTip(
        "These are the exact privacy-redacted bytes that will be exported.");
    dialogLayout->addWidget(exact, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel,
        &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialogLayout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) return;

    const auto approved = exportSession.approve(preview);
    if (!approved.succeeded()) {
        QMessageBox::critical(
            this,
            "Support bundle approval failed",
            QString::fromStdString(approved.message));
        return;
    }

    std::string exactJson;
    const auto exported = exportSession.exportApproved(exactJson);
    if (!exported.succeeded()) {
        QMessageBox::critical(
            this,
            "Support bundle export failed",
            QString::fromStdString(exported.message));
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this,
        "Export HydraSeat Support Bundle",
        QDir::homePath() + QStringLiteral("/HydraSeat-support.json"),
        "JSON files (*.json)");
    if (path.isEmpty()) return;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QByteArray::fromStdString(exactJson)) !=
            static_cast<qint64>(exactJson.size()) ||
        !file.commit()) {
        QMessageBox::critical(
            this,
            "Support bundle export failed",
            "HydraSeat could not write the selected support bundle file.");
        return;
    }

    QMessageBox::information(
        this,
        "Support bundle exported",
        "The privacy-redacted support bundle was exported successfully.");
}

} // namespace hydra::ui
