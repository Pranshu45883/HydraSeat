#include "ui/pages/diagnostics_page.hpp"

#include <QFrame>
#include <QVariant>

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
    const hydra::hostipc::SeatSnapshot* seat1 = nullptr;
    const hydra::hostipc::SeatSnapshot* seat2 = nullptr;
    if (payload.hostSnapshot) {
        seat1 = &payload.hostSnapshot->seats[0];
        seat2 = &payload.hostSnapshot->seats[1];
    }

    if (m_seatFrames.size() >= 2) {
        updateSeatDiagnostics(seat1, m_seatFrames[0]);
        updateSeatDiagnostics(seat2, m_seatFrames[1]);
    }
}

} // namespace hydra::ui
