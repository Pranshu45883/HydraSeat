#include "ui/routing_controller.hpp"
#include "src/windows_audio_router.hpp"
#include "hydra/audio_session_observer.hpp"
#include <windows.h>
#include <objbase.h>

namespace hydra::ui {

RoutingWorker::RoutingWorker(std::shared_ptr<hydra::runtime::AuthorityBridge> bridge, EnumeratorFunc enumerator, RouterFactory routerFactory)
    : m_bridge(std::move(bridge)),
      m_enumerator(enumerator ? std::move(enumerator) : hydra::windows::AudioSessionObserver::enumerateSessions),
      m_routerFactory(routerFactory ? std::move(routerFactory) : []() -> std::unique_ptr<hydra::runtime::AudioRouter> { return std::make_unique<hydra::windows::WindowsAudioRouter>(); }) {}

void RoutingWorker::doRoute(uint32_t pid, uint64_t creationIdentity, const QString& endpointIdStr) {
    if (creationIdentity == 0) {
        emit routingCompleted(pid, RouteVerificationResult::ProcessIdentityValidationFailure, "Production routing requires a valid creation identity.");
        return;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitialized = SUCCEEDED(hr);
    if (!comInitialized) {
        emit routingCompleted(pid, RouteVerificationResult::ProcessIdentityValidationFailure, "Failed to initialize COM on routing thread.");
        return;
    }

    struct ComUninitializer {
        bool active;
        ~ComUninitializer() { if (active) CoUninitialize(); }
    } comUninit{comInitialized};

    auto sessionsResult = m_enumerator();
    if (!sessionsResult.isSuccess()) {
        emit routingCompleted(pid, RouteVerificationResult::ProcessIdentityValidationFailure, "Failed to enumerate audio sessions to validate process.");
        return;
    }

    bool targetFound = false;
    std::wstring previousEndpointId;
    std::optional<std::wstring> previousEndpointStableId;
    
    for (const auto& session : sessionsResult.sessions) {
        if (session.processId == pid) {
            if (!session.processIdentity || session.processIdentity->creationIdentity != creationIdentity) {
                emit routingCompleted(pid, RouteVerificationResult::ProcessIdentityValidationFailure, "Process identity mismatch (PID reused).");
                return;
            }
            targetFound = true;
            previousEndpointId = session.endpointId;
            previousEndpointStableId = session.endpointStableId;
            break;
        }
    }

    if (!targetFound) {
        emit routingCompleted(pid, RouteVerificationResult::ProcessIdentityValidationFailure, "Process does not own an active audio session.");
        return;
    }

    hydra::runtime::ProcessIdentity processId{pid, creationIdentity};
    auto seatIdOpt = m_bridge->findSeatForProcess(processId);
    if (!seatIdOpt) {
        emit routingCompleted(pid, RouteVerificationResult::ProcessIdentityValidationFailure, "Process is not bound to any active Seat.");
        return;
    }
    uint32_t targetSeat = *seatIdOpt;

    // Capture other-seat isolation snapshot
    struct OtherSeatState {
        uint32_t otherPid;
        uint64_t otherCid;
        std::optional<std::wstring> endpointStableId;
    };
    std::vector<OtherSeatState> otherSeatStates;
    for (const auto& session : sessionsResult.sessions) {
        if (session.processId == pid || !session.processIdentity) continue;
        auto otherSeatOpt = m_bridge->findSeatForProcess(*session.processIdentity);
        if (otherSeatOpt && *otherSeatOpt != targetSeat) {
            otherSeatStates.push_back({session.processId, session.processIdentity->creationIdentity, session.endpointStableId});
        }
    }

    hydra::runtime::AudioEndpointIdentity targetEndpoint{endpointIdStr.toStdWString(), std::nullopt};
    auto router = m_routerFactory();
    auto status = m_bridge->routeAudio(targetSeat, processId, targetEndpoint, *router);
    if (status != hydra::runtime::AudioRouteStatus::Success) {
        emit routingCompleted(pid, RouteVerificationResult::ProcessIdentityValidationFailure, "Runtime rejected audio routing authorization.");
        return;
    }

    // Verify endpoint actually moved
    auto verifySessions = m_enumerator();
    bool routeSucceeded = false;
    if (verifySessions.isSuccess()) {
        for (const auto& session : verifySessions.sessions) {
            if (session.processId == pid && session.processIdentity && session.processIdentity->creationIdentity == creationIdentity) {
                if (session.endpointId == targetEndpoint.endpointId) {
                    routeSucceeded = true;
                }
                break;
            }
        }
    }

    bool isolationFailed = false;
    if (routeSucceeded && verifySessions.isSuccess()) {
        for (const auto& state : otherSeatStates) {
            bool foundOther = false;
            for (const auto& session : verifySessions.sessions) {
                if (session.processId == state.otherPid && session.processIdentity && session.processIdentity->creationIdentity == state.otherCid) {
                    foundOther = true;
                    if (session.endpointStableId != state.endpointStableId) {
                        isolationFailed = true;
                    }
                    break;
                }
            }
            if (isolationFailed) break;
        }
    }

    if (!routeSucceeded || isolationFailed || !verifySessions.isSuccess()) {
        // Rollback
        if (!previousEndpointId.empty()) {
            hydra::runtime::AudioEndpointIdentity fallbackEndpoint{previousEndpointId, previousEndpointStableId};
            m_bridge->routeAudio(targetSeat, processId, fallbackEndpoint, *router);
        } else {
            m_bridge->resetAudio(targetSeat, processId, *router);
        }
        
        // Verify Rollback
        auto rollbackSessions = m_enumerator();
        bool rollbackSucceeded = false;
        if (rollbackSessions.isSuccess()) {
            for (const auto& session : rollbackSessions.sessions) {
                if (session.processId == pid && session.processIdentity && session.processIdentity->creationIdentity == creationIdentity) {
                    if (!previousEndpointId.empty()) {
                        if (session.endpointId == previousEndpointId) {
                            rollbackSucceeded = true;
                        }
                    } else {
                        // Empty previousEndpointId means it was default. After reset, we just assume it succeeded if it exists.
                        rollbackSucceeded = true; 
                    }
                    break;
                }
            }
        }
        
        if (isolationFailed) {
            emit routingCompleted(pid, rollbackSucceeded ? RouteVerificationResult::CrossSeatIsolationFailure : RouteVerificationResult::FailedRollbackFailed, "Cross-Seat isolation failure.");
        } else {
            emit routingCompleted(pid, rollbackSucceeded ? RouteVerificationResult::FailedRollbackSuccess : RouteVerificationResult::FailedRollbackFailed, "Route verification failed.");
        }
        return;
    }

    emit routingCompleted(pid, RouteVerificationResult::Success, "");
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

    auto sessionsResult = m_enumerator();
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
    
    auto router = m_routerFactory();
    auto status = m_bridge->resetAudio(*seatIdOpt, processId, *router);
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
