#include <QScrollArea>
#include "ui/pages/dashboard_page.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>

namespace hydra::ui {

DashboardPage::DashboardPage(QWidget* parent)
    : QWidget(parent) {
        auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");
    auto* container = new QWidget();
    container->setStyleSheet("background-color: transparent;");
    auto* layout = new QVBoxLayout(container);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(scrollArea);
    scrollArea->setWidget(container);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(24);

    auto* title = new QLabel("Dashboard", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5; font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);

    auto* subtitle = new QLabel("System overview and current HydraSeat runtime state", this);
    subtitle->setStyleSheet("font-size: 14px; color: #B5B5B5; font-family: 'Segoe UI', sans-serif; margin-bottom: 8px;");
    layout->addWidget(subtitle);

    auto* metricsLayout = new QGridLayout();
    metricsLayout->setSpacing(16);
    setupMetrics(metricsLayout);
        metricsLayout->setColumnStretch(3, 1);
    layout->addLayout(metricsLayout);

    auto* seatsTitle = new QLabel("CURRENT SEATS", this);
    seatsTitle->setStyleSheet("font-size: 14px; font-weight: bold; color: #777777; margin-top: 24px; font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(seatsTitle);

    auto* seatsLayout = new QGridLayout();
    seatsLayout->setSpacing(16);
    setupSeats(seatsLayout);
        seatsLayout->setColumnStretch(2, 1);
    layout->addLayout(seatsLayout);

    layout->addStretch();
}

static QFrame* createMetricCard(const QString& title, const QString& subtitle, QLabel*& valLabel) {
    auto* frame = new QFrame();
    frame->setMinimumWidth(400);
    frame->setMaximumWidth(450);
    frame->setMinimumWidth(220);
    frame->setMaximumWidth(300);
    frame->setStyleSheet("background-color: #151515; border-radius: 8px; border: 1px solid #292929; padding: 18px;");
    auto* l = new QVBoxLayout(frame);
    l->setContentsMargins(0,0,0,0);
    l->setSpacing(4);

    auto* titleLbl = new QLabel(title);
    titleLbl->setStyleSheet("font-size: 12px; font-weight: bold; color: #777777; border: none; text-transform: uppercase;");
    l->addWidget(titleLbl);

    valLabel = new QLabel("00");
    valLabel->setStyleSheet("font-size: 32px; font-weight: bold; color: #F5F5F5; border: none;");
    l->addWidget(valLabel);

    auto* subLbl = new QLabel(subtitle);
    subLbl->setStyleSheet("font-size: 11px; color: #B5B5B5; border: none;");
    l->addWidget(subLbl);

    return frame;
}

void DashboardPage::setupMetrics(QGridLayout* layout) {
    layout->addWidget(createMetricCard("SEATS", "Configured seats", m_valSeats), 0, 0);
    layout->addWidget(createMetricCard("ACTIVE", "Active seats", m_valActive), 0, 1);
    layout->addWidget(createMetricCard("DISPLAYS", "Detected displays", m_valDisplays), 0, 2);

    layout->addWidget(createMetricCard("INPUTS", "Keyboards, Mice & Controllers", m_valInputs), 1, 0);
    layout->addWidget(createMetricCard("AUDIO ENDPOINTS", "Available endpoints", m_valAudioEndpoints), 1, 1);
    layout->addWidget(createMetricCard("AUDIO SESSIONS", "Active audio sessions", m_valAudioSessions), 1, 2);
}

static QFrame* createSeatCard(const QString& title, QLabel*& stateLbl, QLabel*& appLbl, QLabel*& audioLbl, QLabel*& ctrlLbl) {
    auto* frame = new QFrame();
    frame->setStyleSheet("background-color: #151515; border-radius: 8px; border: 1px solid #292929; padding: 20px;");
    auto* l = new QVBoxLayout(frame);
    l->setContentsMargins(0,0,0,0);
    l->setSpacing(12);

    auto* header = new QHBoxLayout();
    auto* tLbl = new QLabel(title);
    tLbl->setStyleSheet("font-size: 18px; font-weight: bold; color: #F5F5F5; border: none;");
    stateLbl = new QLabel("○ Unavailable");
    stateLbl->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none;");
    header->addWidget(tLbl);
    header->addStretch();
    header->addWidget(stateLbl);
    l->addLayout(header);

    auto addField = [&](const QString& labelText, QLabel*& val) {
        auto* fl = new QVBoxLayout();
        fl->setSpacing(2);
        auto* lab = new QLabel(labelText);
        lab->setStyleSheet("font-size: 11px; color: #777777; border: none; text-transform: uppercase; font-weight: bold;");
        val = new QLabel("Not assigned");
        val->setStyleSheet("font-size: 13px; color: #F5F5F5; border: none;");
        fl->addWidget(lab);
        fl->addWidget(val);
        l->addLayout(fl);
    };

    addField("Application", appLbl);
    addField("Audio", audioLbl);
    addField("Controller", ctrlLbl);

    l->addStretch();
    return frame;
}

void DashboardPage::setupSeats(QGridLayout* layout) {
    layout->addWidget(createSeatCard("Seat 1", m_seat1State, m_seat1App, m_seat1Audio, m_seat1Controller), 0, 0);
    layout->addWidget(createSeatCard("Seat 2", m_seat2State, m_seat2App, m_seat2Audio, m_seat2Controller), 0, 1);
}

void DashboardPage::updateSeat(
    const hydra::hostipc::SeatSnapshot* snapshot,
    QLabel* stateLbl,
    QLabel* appLbl,
    QLabel* audioLbl,
    QLabel* ctrlLbl) {
    if (!snapshot) {
        stateLbl->setText("○ HOST UNAVAILABLE");
        stateLbl->setStyleSheet("font-size: 13px; font-weight: bold; color: #777777; border: none;");
        appLbl->setText("Unavailable");
        audioLbl->setText("Unavailable");
        ctrlLbl->setText("Unavailable");
        return;
    }

    if (snapshot->gameLeaseActive && snapshot->uiLeaseActive) {
        stateLbl->setText("● UI + GAME ACTIVE");
        stateLbl->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; border: none;");
    } else if (snapshot->gameLeaseActive) {
        stateLbl->setText("● GAME ACTIVE");
        stateLbl->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; border: none;");
    } else if (snapshot->uiLeaseActive) {
        stateLbl->setText("● UI CONFIGURING");
        stateLbl->setStyleSheet("font-size: 13px; font-weight: bold; color: #E10600; border: none;");
    } else {
        stateLbl->setText("● AUTHORITY IDLE");
        stateLbl->setStyleSheet("font-size: 13px; font-weight: bold; color: #B5B5B5; border: none;");
    }

    appLbl->setText(
        snapshot->processOwned
            ? QString("PID %1").arg(snapshot->processId)
            : "Not assigned");
    audioLbl->setText(snapshot->processOwned ? "See Audio page" : "Not assigned");
    ctrlLbl->setText(snapshot->controllerBound ? "Bound" : "Not assigned");
}

void DashboardPage::updateState(const EngineStatePayload& payload) {
    m_valSeats->setText("02");

    int activeSeats = 0;
    const hydra::hostipc::SeatSnapshot* seat1 = nullptr;
    const hydra::hostipc::SeatSnapshot* seat2 = nullptr;
    if (payload.hostSnapshot) {
        seat1 = &payload.hostSnapshot->seats[0];
        seat2 = &payload.hostSnapshot->seats[1];
        if (seat1->active) ++activeSeats;
        if (seat2->active) ++activeSeats;
    }
    m_valActive->setText(QString("%1").arg(activeSeats, 2, 10, QChar('0')));

    m_valDisplays->setText(QString("%1").arg(payload.displays.size(), 2, 10, QChar('0')));

    const size_t inputCount =
        payload.keyboards.size() + payload.mice.size() + payload.controllers.size();
    m_valInputs->setText(QString("%1").arg(inputCount, 2, 10, QChar('0')));

    m_valAudioEndpoints->setText(QString("%1").arg(payload.audioEndpoints.size(), 2, 10, QChar('0')));
    m_valAudioSessions->setText(QString("%1").arg(payload.audioSessions.size(), 2, 10, QChar('0')));

    updateSeat(seat1, m_seat1State, m_seat1App, m_seat1Audio, m_seat1Controller);
    updateSeat(seat2, m_seat2State, m_seat2App, m_seat2Audio, m_seat2Controller);
}

} // namespace hydra::ui
