#include "ui/engine_poller.hpp"

#include <objbase.h>
#include <windows.h>

namespace hydra::ui {

EnginePollerWorker::EnginePollerWorker(std::shared_ptr<hydra::HardwareDetector> hardwareDetector)
    : m_hardwareDetector(std::move(hardwareDetector)) {}

void EnginePollerWorker::doPoll() {
    EngineStatePayload payload;

    std::string hostError;
    if (!m_hostClient.connected()) {
        if (!m_hostClient.connect(
                hydra::hostipc::ClientRole::ReadOnly,
                hydra::hostipc::kDefaultHostPipeTimeoutMs,
                &hostError)) {
            payload.hostError = std::move(hostError);
        }
    }
    if (m_hostClient.connected()) {
        auto snapshot = m_hostClient.getSnapshot(
            hydra::hostipc::kDefaultHostPipeTimeoutMs,
            &hostError);
        if (snapshot) {
            payload.hostConnected = true;
            payload.hostSnapshot = std::move(snapshot);
        } else {
            payload.hostError = std::move(hostError);
            m_hostClient.close();
        }
    }

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(hr);
    if (!comInitialized) {
        payload.hardwareError = true;
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

    if (m_hardwareDetector) {
        payload.displays = m_hardwareDetector->detectDisplays();
        payload.keyboards = m_hardwareDetector->detectKeyboards();
        payload.mice = m_hardwareDetector->detectMice();
        payload.controllers = m_hardwareDetector->detectControllers();
        payload.hardwareError = false;
    } else {
        payload.hardwareError = true;
    }

    const auto endpointsResult =
        hydra::windows::AudioEndpointInventory::enumerateRenderEndpoints();
    if (endpointsResult.isSuccess()) {
        payload.audioEndpoints = *endpointsResult.endpoints;
    } else {
        payload.audioEndpointError = true;
    }

    const auto sessionsResult =
        hydra::windows::AudioSessionObserver::enumerateSessions();
    if (sessionsResult.isSuccess()) {
        payload.audioSessions = sessionsResult.sessions;
    } else {
        payload.audioSessionError = true;
    }

    payload.controllerInventory = m_controllerInventory.scan();

    emit pollCompleted(payload);
}

EnginePoller::EnginePoller(
    std::shared_ptr<hydra::HardwareDetector> hardwareDetector,
    QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<hydra::ui::EngineStatePayload>(
        "hydra::ui::EngineStatePayload");

    m_worker = new EnginePollerWorker(std::move(hardwareDetector));
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
        &EnginePoller::stateUpdated,
        Qt::QueuedConnection);

    m_triggerTimer = new QTimer(this);
    connect(
        m_triggerTimer,
        &QTimer::timeout,
        this,
        &EnginePoller::triggerPoll);

    m_workerThread.start();
}

EnginePoller::~EnginePoller() {
    stopPolling();
    m_workerThread.quit();
    m_workerThread.wait();
}

void EnginePoller::startPolling(int intervalMs) {
    emit triggerPoll();
    m_triggerTimer->start(intervalMs);
}

void EnginePoller::stopPolling() {
    m_triggerTimer->stop();
}

} // namespace hydra::ui
