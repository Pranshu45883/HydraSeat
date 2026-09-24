#include "ui/engine_poller.hpp"
#include <QTimer>

namespace hydra::ui {

EnginePollerWorker::EnginePollerWorker(std::shared_ptr<hydra::HardwareDetector> hardwareDetector)
    : m_hardwareDetector(std::move(hardwareDetector)) {}

void EnginePollerWorker::doPoll() {
    EngineStatePayload payload;

    // Hardware polling
    if (m_hardwareDetector) {
        payload.displays = m_hardwareDetector->detectDisplays();
        payload.keyboards = m_hardwareDetector->detectKeyboards();
        payload.mice = m_hardwareDetector->detectMice();
        payload.controllers = m_hardwareDetector->detectControllers();
        payload.hardwareError = false; // Add actual error detection if HardwareDetector starts returning errors
    } else {
        payload.hardwareError = true;
    }

    // Audio endpoints polling (blocking COM calls)
    auto endpointsResult = hydra::windows::AudioEndpointInventory::enumerateRenderEndpoints();
    if (endpointsResult.isSuccess()) {
        payload.audioEndpoints = *endpointsResult.endpoints;
        payload.audioEndpointError = false;
    } else {
        payload.audioEndpointError = true;
    }

    // Audio sessions polling (blocking COM calls)
    auto sessionsResult = hydra::windows::AudioSessionObserver::enumerateSessions();
    if (sessionsResult.isSuccess()) {
        payload.audioSessions = sessionsResult.sessions;
        payload.audioSessionError = false;
    } else {
        payload.audioSessionError = true;
    }

    emit pollCompleted(payload);
}


EnginePoller::EnginePoller(std::shared_ptr<hydra::HardwareDetector> hardwareDetector, QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<hydra::ui::EngineStatePayload>("hydra::ui::EngineStatePayload");

    m_worker = new EnginePollerWorker(std::move(hardwareDetector));
    m_worker->moveToThread(&m_workerThread);

    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    
    // Connect the trigger signal from this thread to the worker's slot in its thread
    connect(this, &EnginePoller::triggerPoll, m_worker, &EnginePollerWorker::doPoll, Qt::QueuedConnection);
    
    // Connect the worker's completion signal back to this thread
    connect(m_worker, &EnginePollerWorker::pollCompleted, this, &EnginePoller::stateUpdated, Qt::QueuedConnection);

    m_triggerTimer = new QTimer(this);
    connect(m_triggerTimer, &QTimer::timeout, this, &EnginePoller::triggerPoll);

    m_workerThread.start();
}

EnginePoller::~EnginePoller() {
    stopPolling();
    m_workerThread.quit();
    m_workerThread.wait();
}

void EnginePoller::startPolling(int intervalMs) {
    // Fire an immediate poll, then start the periodic timer
    emit triggerPoll();
    m_triggerTimer->start(intervalMs);
}

void EnginePoller::stopPolling() {
    m_triggerTimer->stop();
}

} // namespace hydra::ui
