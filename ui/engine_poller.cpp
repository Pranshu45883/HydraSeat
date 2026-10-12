#include "ui/engine_poller.hpp"
#include "ui/host_bootstrap.hpp"

#include <QString>

#include <algorithm>
#include <objbase.h>
#include <utility>
#include <windows.h>

namespace hydra::ui {


namespace {

std::wstring fromUtf8(const std::string& value) {
    return QString::fromUtf8(
               value.data(), static_cast<qsizetype>(value.size()))
        .toStdWString();
}

void applyHostHardwareInventory(
    EngineStatePayload& payload,
    const hydra::hostipc::HardwareInventory& inventory) {
    payload.displays.clear();
    payload.keyboards.clear();
    payload.mice.clear();
    payload.controllers.clear();

    for (const auto& record : inventory.devices) {
        hydra::DeviceInfo device;
        device.id = fromUtf8(record.stableIdUtf8);
        device.name = fromUtf8(record.displayNameUtf8);
        if (device.id.empty()) continue;

        switch (record.kind) {
        case hydra::hostipc::HardwareDeviceKind::Display:
            device.type = hydra::DeviceType::Display;
            payload.displays.push_back(std::move(device));
            break;
        case hydra::hostipc::HardwareDeviceKind::Keyboard:
            device.type = hydra::DeviceType::Keyboard;
            payload.keyboards.push_back(std::move(device));
            break;
        case hydra::hostipc::HardwareDeviceKind::Mouse:
            device.type = hydra::DeviceType::Mouse;
            payload.mice.push_back(std::move(device));
            break;
        case hydra::hostipc::HardwareDeviceKind::Controller:
            device.type = hydra::DeviceType::Controller;
            payload.controllers.push_back(std::move(device));
            break;
        }
    }
}

void deduplicateAudioSessionsByProcess(EngineStatePayload& payload) {
    std::vector<hydra::windows::AudioSessionObservation> filtered;
    for (const auto& session : payload.audioSessions) {
        // Process identity is required for stable UI matching and for any
        // mutation. Sessions whose PID cannot be creation-time validated are
        // not useful as application records and are intentionally omitted.
        if (!session.processIdentity ||
            !session.processIdentity->valid()) {
            continue;
        }

        const auto existing = std::find_if(
            filtered.begin(), filtered.end(),
            [&](const auto& candidate) {
                return candidate.processIdentity &&
                       *candidate.processIdentity ==
                           *session.processIdentity;
            });
        if (existing == filtered.end()) {
            filtered.push_back(session);
            continue;
        }

        // A process can expose sessions on multiple render endpoints. Present
        // one application row/card, preferring an active session over an
        // inactive/expired one. Host ownership is evaluated by the UI against
        // the authoritative snapshot; do not delete unassigned applications.
        if (existing->state !=
                hydra::windows::AudioSessionState::Active &&
            session.state ==
                hydra::windows::AudioSessionState::Active) {
            *existing = session;
        }
    }

    payload.audioSessions = std::move(filtered);
}

} // namespace

void EnginePollerWorker::doPoll() {
    EngineStatePayload payload;


    std::string hostError;
    if (!m_hostClient.connected()) {
        if (!connectCanonicalHost(
                m_hostClient,
                hydra::hostipc::ClientRole::ReadOnly,
                &hostError)) {
            payload.hostError = std::move(hostError);
        }
    }
    if (m_hostClient.connected()) {
        auto snapshot = m_hostClient.getSnapshot(
            hydra::hostipc::kDefaultHostPipeTimeoutMs,
            &hostError);
        if (snapshot) {
            // A valid authority snapshot proves the canonical host connection
            // itself is alive. Inventory failures are subsystem failures and
            // must not make the status bar claim that the host disappeared.
            payload.hostConnected = true;
            payload.hostSnapshot = std::move(snapshot);

            std::string inventoryError;
            const auto inventory = m_hostClient.getHardwareInventory(
                hydra::hostipc::kDefaultHostPipeTimeoutMs,
                &inventoryError);
            if (!inventory) {
                payload.hardwareError = true;
                payload.hostError = inventoryError.empty()
                    ? "canonical host hardware inventory is unavailable"
                    : std::move(inventoryError);
            } else {
                payload.hardwareError = false;
                applyHostHardwareInventory(payload, *inventory);

                for (std::uint32_t seatId = 1;
                     seatId <= hydra::hostipc::kHostSeatCount;
                     ++seatId) {
                    std::string hardwareError;
                    payload.seatHardware[seatId - 1u] =
                        m_hostClient.getSeatHardware(
                            seatId,
                            hydra::hostipc::kDefaultHostPipeTimeoutMs,
                            &hardwareError);
                    if (!payload.seatHardware[seatId - 1u] &&
                        payload.hostError.empty()) {
                        payload.hostError = std::move(hardwareError);
                    }
                }
            }
        } else {
            payload.hardwareError = true;
            payload.hostError = std::move(hostError);
            m_hostClient.close();
        }
    }

    // Controller discovery is independent from Core Audio/COM. Do it before
    // entering the audio apartment so a transient COM failure cannot make
    // controllers disappear from an otherwise healthy hardware snapshot.
    payload.controllerInventory = m_controllerInventory.scan();
    payload.controllerInventoryError =
        !payload.controllerInventory.authoritative;

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(hr);
    if (!comInitialized) {
        payload.audioEndpointError = true;
        payload.audioSessionError = true;
        emit pollCompleted(payload);
        return;
    }

    struct ComUninitializer {
        bool active;
        ~ComUninitializer() {
            if (active) CoUninitialize();
        }
    } comUninit{comInitialized};


    const auto endpointsResult =
        hydra::windows::AudioEndpointInventory::enumerateRenderEndpoints();
    if (endpointsResult.isSuccess()) {
        payload.audioEndpoints = *endpointsResult.endpoints;
    } else {
        payload.audioEndpointError = true;
    }

    const auto sessionsResult =
        hydra::windows::AudioSessionObserver::enumerateSessions();
    if (sessionsResult.isSuccess() && sessionsResult.isComplete) {
        payload.audioSessions = sessionsResult.sessions;
        deduplicateAudioSessionsByProcess(payload);
    } else {
        // A partial Core Audio walk is not authoritative absence. Publishing a
        // truncated list makes running Seat applications blink out of the UI.
        payload.audioSessionError = true;
        payload.audioSessions.clear();
    }

    emit pollCompleted(payload);
}

EnginePoller::EnginePoller(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<hydra::ui::EngineStatePayload>(
        "hydra::ui::EngineStatePayload");

    m_worker = new EnginePollerWorker();
    m_worker->moveToThread(&m_workerThread);

    connect(
        &m_workerThread,
        &QThread::finished,
        m_worker,
        &QObject::deleteLater);
    connect(
        this,
        &EnginePoller::triggerPoll,
        m_worker,
        &EnginePollerWorker::doPoll,
        Qt::QueuedConnection);
    connect(
        m_worker,
        &EnginePollerWorker::pollCompleted,
        this,
        &EnginePoller::onPollCompleted,
        Qt::QueuedConnection);

    m_triggerTimer = new QTimer(this);
    connect(
        m_triggerTimer,
        &QTimer::timeout,
        this,
        &EnginePoller::requestPoll);

    m_workerThread.start();
}

EnginePoller::~EnginePoller() {
    stopPolling();
    m_workerThread.quit();
    m_workerThread.wait();
}

void EnginePoller::startPolling(int intervalMs) {
    m_triggerTimer->start(intervalMs);
    requestPoll();
}

void EnginePoller::stopPolling() {
    m_triggerTimer->stop();
}

void EnginePoller::requestPoll() {
    // Hardware, audio-session and controller discovery can legitimately take
    // longer than the configured refresh interval during device churn. Queueing
    // another poll for every timer tick would make stale snapshots arrive late
    // and can visually flap a healthy host between old/new states.
    if (m_pollInFlight || !m_workerThread.isRunning()) return;
    m_pollInFlight = true;
    emit triggerPoll();
}

void EnginePoller::onPollCompleted(EngineStatePayload payload) {
    m_pollInFlight = false;
    emit stateUpdated(std::move(payload));
}

} // namespace hydra::ui
