#include "ui/routing_controller.hpp"
#include "src/windows_audio_router.hpp"
#include "hydra/audio_session_observer.hpp"
#include <windows.h>
#include <objbase.h>

namespace hydra::ui {

RoutingWorker::RoutingWorker(std::shared_ptr<hydra::runtime::AuthorityBridge> bridge)
    : m_bridge(std::move(bridge)) {}

void RoutingWorker::doRoute(uint32_t pid, uint64_t creationIdentity, const QString& endpointId) {
    if (creationIdentity == 0) {
        emit routingCompleted(pid, false, "Production routing requires a valid creation identity.");
        return;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitialized = SUCCEEDED(hr);
    if (!comInitialized) {
        emit routingCompleted(pid, false, "Failed to initialize COM on routing thread.");
        return;
    }

    struct ComUninitializer {
        bool active;
        ~ComUninitializer() { if (active) CoUninitialize(); }
    } comUninit{comInitialized};

    auto sessionsResult = hydra::windows::AudioSessionObserver::enumerateSessions();
    if (!sessionsResult.isSuccess()) {
        emit routingCompleted(pid, false, "Failed to enumerate audio sessions to validate process.");
        return;
    }

    bool found = false;
    for (const auto& session : sessionsResult.sessions) {
        if (session.processId == pid) {
            if (!session.processIdentity) {
                emit routingCompleted(pid, false, "Observed process lacks a creation identity.");
                return;
            }
            if (session.processIdentity->creationIdentity != creationIdentity) {
                emit routingCompleted(pid, false, "Process identity mismatch (PID reused).");
                return;
            }
            found = true;
            break;
        }
    }

    if (!found) {
        emit routingCompleted(pid, false, "Process does not own an active audio session.");
        return;
    }

    hydra::runtime::ProcessIdentity processId{pid, creationIdentity};
    auto seatIdOpt = m_bridge->findSeatForProcess(processId);
    if (!seatIdOpt) {
        emit routingCompleted(pid, false, "Process is not bound to any active Seat.");
        return;
    }
    
    // Capture the exact previous endpoint identity for rollback
    std::optional<std::wstring> previousEndpointId;
    for (const auto& session : sessionsResult.sessions) {
        if (session.processId == pid) {
            previousEndpointId = session.endpointStableId;
            break;
        }
    }

    hydra::runtime::AudioEndpointIdentity targetEndpoint{endpointId.toStdWString(), std::nullopt};
    hydra::windows::WindowsAudioRouter router;
    
    auto status = m_bridge->routeAudio(*seatIdOpt, processId, targetEndpoint, router);
    if (status != hydra::runtime::AudioRouteStatus::Success) {
        emit routingCompleted(pid, false, "Runtime rejected audio routing authorization.");
        return;
    }

    // Verify endpoint actually moved
    auto verificationSessions = hydra::windows::AudioSessionObserver::enumerateSessions();
    if (!verificationSessions.isSuccess()) {
        emit routingCompleted(pid, false, "Failed to verify audio routing success.");
        return;
    }
    
    bool moved = false;
    for (const auto& session : verificationSessions.sessions) {
        if (session.processId == pid) {
            if (session.endpointStableId && *session.endpointStableId == targetEndpoint.stableId) {
                moved = true;
            }
            break;
        }
    }
    
    if (!moved) {
        // Rollback to exactly the previous endpoint
        if (previousEndpointId) {
            hydra::runtime::AudioEndpointIdentity fallbackEndpoint{*previousEndpointId, std::nullopt};
            m_bridge->routeAudio(*seatIdOpt, processId, fallbackEndpoint, router);
        } else {
            m_bridge->resetAudio(*seatIdOpt, processId, router);
        }
        emit routingCompleted(pid, false, "Backend rejected route. State rolled back.");
        return;
    }

    emit routingCompleted(pid, true, "");
}

void RoutingWorker::doReset(uint32_t pid, uint64_t creationIdentity) {
    if (creationIdentity == 0) {
        emit resetCompleted(pid, false, "Production routing requires a valid creation identity.");
        return;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitialized = SUCCEEDED(hr);
    if (!comInitialized) {
        emit resetCompleted(pid, false, "Failed to initialize COM on routing thread.");
        return;
    }

    struct ComUninitializer {
        bool active;
        ~ComUninitializer() { if (active) CoUninitialize(); }
    } comUninit{comInitialized};

    auto sessionsResult = hydra::windows::AudioSessionObserver::enumerateSessions();
    if (!sessionsResult.isSuccess()) {
        emit resetCompleted(pid, false, "Failed to enumerate audio sessions.");
        return;
    }

    bool found = false;
    for (const auto& session : sessionsResult.sessions) {
        if (session.processId == pid) {
            if (!session.processIdentity) {
                emit resetCompleted(pid, false, "Observed process lacks a creation identity.");
                return;
            }
            if (session.processIdentity->creationIdentity != creationIdentity) {
                emit resetCompleted(pid, false, "Process identity mismatch (PID reused).");
                return;
            }
            found = true;
            break;
        }
    }

    if (!found) {
        emit resetCompleted(pid, false, "Process does not own an active audio session.");
        return;
    }

    hydra::runtime::ProcessIdentity processId{pid, creationIdentity};
    auto seatIdOpt = m_bridge->findSeatForProcess(processId);
    if (!seatIdOpt) {
        emit resetCompleted(pid, false, "Process is not bound to any active Seat.");
        return;
    }
    
    hydra::windows::WindowsAudioRouter router;
    auto status = m_bridge->resetAudio(*seatIdOpt, processId, router);
    if (status != hydra::runtime::AudioRouteStatus::Success) {
        emit resetCompleted(pid, false, "Runtime rejected audio reset authorization.");
        return;
    }

    emit resetCompleted(pid, true, "");
}

RoutingController::RoutingController(std::shared_ptr<hydra::runtime::AuthorityBridge> bridge, QObject* parent)
    : QObject(parent) {
    m_worker = new RoutingWorker(std::move(bridge));
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

void RoutingController::requestRoute(uint32_t pid, uint64_t creationIdentity, const QString& endpointId) {
    emit triggerRoute(pid, creationIdentity, endpointId);
}

void RoutingController::requestReset(uint32_t pid, uint64_t creationIdentity) {
    emit triggerReset(pid, creationIdentity);
}

} // namespace hydra::ui
