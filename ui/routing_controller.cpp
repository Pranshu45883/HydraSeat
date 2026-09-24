#include "ui/routing_controller.hpp"
#include "src/windows_audio_router.hpp"
#include "hydra/audio_session_observer.hpp"

namespace hydra::ui {

RoutingWorker::RoutingWorker(std::shared_ptr<hydra::runtime::SessionController> sessionController)
    : m_sessionController(std::move(sessionController)) {}

void RoutingWorker::doRoute(uint32_t pid, const QString& endpointId) {
    // 1. Resolve full ProcessIdentity from Windows (needed to prevent PID reuse races)
    auto sessionsResult = hydra::windows::AudioSessionObserver::enumerateSessions();
    if (!sessionsResult.isSuccess()) {
        emit routingCompleted(pid, false, "Failed to enumerate audio sessions to validate process.");
        return;
    }

    uint64_t creationId = 0;
    bool found = false;
    for (const auto& session : sessionsResult.sessions) {
        if (session.processId == pid) {
            if (session.processIdentity) {
                creationId = session.processIdentity->creationIdentity;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        emit routingCompleted(pid, false, "Process does not own an active audio session.");
        return;
    }

    hydra::runtime::ProcessIdentity process{pid, creationId};
    hydra::runtime::AudioEndpointIdentity endpoint{endpointId.toStdWString(), std::nullopt};

    // 2. Perform the actual route via the production WindowsAudioRouter
    hydra::windows::WindowsAudioRouter router;
    
    // Check if process belongs to a seat. If so, we should ideally go through SessionController
    // but the UI currently lacks the ActivationToken. Since the UI is the top-level orchestrator,
    // we route it manually. (To update SessionController state, we'd need a UI-level token map).
    // For this initial implementation, we directly route the validated identity.
    auto status = router.assignEndpoint(process, endpoint);

    if (status == hydra::runtime::AudioRouteStatus::Success) {
        emit routingCompleted(pid, true, "");
    } else {
        emit routingCompleted(pid, false, "Windows Audio Router rejected the assignment.");
    }
}

void RoutingWorker::doReset(uint32_t pid) {
    auto sessionsResult = hydra::windows::AudioSessionObserver::enumerateSessions();
    if (!sessionsResult.isSuccess()) {
        emit resetCompleted(pid, false, "Failed to enumerate audio sessions.");
        return;
    }

    uint64_t creationId = 0;
    bool found = false;
    for (const auto& session : sessionsResult.sessions) {
        if (session.processId == pid) {
            if (session.processIdentity) {
                creationId = session.processIdentity->creationIdentity;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        emit resetCompleted(pid, false, "Process does not own an active audio session.");
        return;
    }

    hydra::runtime::ProcessIdentity process{pid, creationId};
    hydra::windows::WindowsAudioRouter router;
    auto status = router.clearAssignment(process);

    if (status == hydra::runtime::AudioRouteStatus::Success) {
        emit resetCompleted(pid, true, "");
    } else {
        emit resetCompleted(pid, false, "Windows Audio Router failed to clear assignment.");
    }
}

RoutingController::RoutingController(std::shared_ptr<hydra::runtime::SessionController> sessionController, QObject* parent)
    : QObject(parent) {
    m_worker = new RoutingWorker(std::move(sessionController));
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(this, &RoutingController::triggerRoute, m_worker, &RoutingWorker::doRoute, Qt::QueuedConnection);
    connect(this, &RoutingController::triggerReset, m_worker, &RoutingWorker::doReset, Qt::QueuedConnection);

    connect(m_worker, &RoutingWorker::routingCompleted, this, &RoutingController::routingCompleted, Qt::QueuedConnection);
    connect(m_worker, &RoutingWorker::resetCompleted, this, &RoutingController::resetCompleted, Qt::QueuedConnection);

    m_workerThread.start();
}

RoutingController::~RoutingController() {
    m_workerThread.quit();
    m_workerThread.wait();
}

void RoutingController::requestRoute(uint32_t pid, const QString& endpointId) {
    emit triggerRoute(pid, endpointId);
}

void RoutingController::requestReset(uint32_t pid) {
    emit triggerReset(pid);
}

} // namespace hydra::ui
