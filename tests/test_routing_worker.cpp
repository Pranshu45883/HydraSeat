#include "ui/routing_controller.hpp"
#include "hydra/runtime_authority.hpp"
#include "hydra/authority_bridge.hpp"
#include "hydra/audio_session_observer.hpp"

#include <QCoreApplication>
#include <QObject>
#include <QTimer>
#include <cassert>
#include <iostream>

using namespace hydra::ui;
using namespace hydra::runtime;
using namespace hydra::windows;

class SignalCatcher : public QObject {
    Q_OBJECT
public:
    uint32_t lastPid = 0;
    RouteVerificationResult lastResult = RouteVerificationResult::Success;
    QString lastMessage;
    bool signalReceived = false;

public slots:
    void onRoutingCompleted(uint32_t pid, RouteVerificationResult result, const QString& errorMessage) {
        lastPid = pid;
        lastResult = result;
        lastMessage = errorMessage;
        signalReceived = true;
    }
};

class MockAudioRouterForWorker : public AudioRouter {
public:
    std::function<AudioRouteStatus(const ProcessIdentity&, const AudioEndpointIdentity&)> onAssign;
    std::function<AudioRouteStatus(const ProcessIdentity&)> onClear;

    AudioRouteStatus assignEndpoint(const ProcessIdentity& process, const AudioEndpointIdentity& endpoint) noexcept override {
        if (onAssign) return onAssign(process, endpoint);
        return AudioRouteStatus::Success;
    }

    AudioRouteStatus clearAssignment(const ProcessIdentity& process) noexcept override {
        if (onClear) return onClear(process);
        return AudioRouteStatus::Success;
    }
};

static std::vector<AudioSessionObservation> mockSessions;
static AudioSessionInventoryResult mockEnumerateSessions() {
    AudioSessionInventoryResult result;
    result.sessions = mockSessions;
    result.error = std::nullopt;
    return result;
}

static std::function<AudioRouteStatus(const ProcessIdentity&, const AudioEndpointIdentity&)> globalAssignHook;
static std::function<AudioRouteStatus(const ProcessIdentity&)> globalClearHook;

void testRoutingWorkerVerification() {
    std::cout << "[Test] Running RoutingWorker Verification tests..." << std::endl;

    int argc = 1;
    char* argv[] = { (char*)"test" };
    QCoreApplication app(argc, argv);

    auto controller = std::make_shared<SessionController>();
    auto bridge = std::make_shared<AuthorityBridge>(controller);
    
    // Setup initial authority
    bridge->requestUiLease(1);
    bridge->requestUiLease(2);
    
    ProcessIdentity pidA{100, 1000};
    ProcessIdentity pidB{200, 2000};
    
    controller->publishProcess(controller->acquireSeatLease(1, LeaseClass::GameProcess), pidA);
    controller->publishProcess(controller->acquireSeatLease(2, LeaseClass::GameProcess), pidB);

    RoutingWorker worker(bridge, mockEnumerateSessions, []() -> std::unique_ptr<AudioRouter> {
        auto router = std::make_unique<MockAudioRouterForWorker>();
        router->onAssign = globalAssignHook;
        router->onClear = globalClearHook;
        return router;
    });

    SignalCatcher catcher;
    QObject::connect(&worker, &RoutingWorker::routingCompleted, &catcher, &SignalCatcher::onRoutingCompleted);

    auto runWorkerAndSpin = [&](uint32_t pid, uint64_t cid, const QString& endpoint) {
        catcher.signalReceived = false;
        worker.doRoute(pid, cid, endpoint);
        while (!catcher.signalReceived) {
            QCoreApplication::processEvents();
        }
    };

    // Helper for initial state
    auto resetMockSessions = [&]() {
        mockSessions.clear();
        AudioSessionObservation sessionA;
        sessionA.processId = pidA.pid;
        sessionA.processIdentity = pidA;
        sessionA.endpointId = L"default_endpoint_A";
        sessionA.endpointStableId = L"default_stable_A";
        
        AudioSessionObservation sessionB;
        sessionB.processId = pidB.pid;
        sessionB.processIdentity = pidB;
        sessionB.endpointId = L"default_endpoint_B";
        sessionB.endpointStableId = L"default_stable_B";
        
        mockSessions.push_back(sessionA);
        mockSessions.push_back(sessionB);
    };

    // TEST 1: Route Seat A successfully. Verify Seat B endpoint remains unchanged.
    std::cout << "  -> TEST 1: Route Seat A successfully." << std::endl;
    resetMockSessions();
    globalAssignHook = [&](const ProcessIdentity& p, const AudioEndpointIdentity& ep) {
        // Mutate mockSessions to reflect success
        for (auto& s : mockSessions) {
            if (s.processId == p.pid) {
                s.endpointId = ep.endpointId;
                s.endpointStableId = ep.endpointId; // Assuming they are same for mock
            }
        }
        return AudioRouteStatus::Success;
    };
    runWorkerAndSpin(100, 1000, "new_endpoint_A");
    assert(catcher.lastResult == RouteVerificationResult::Success);

    // TEST 2: Route Seat A fails verification. Rollback Seat A. Verify Seat A returns to its exact previous endpoint.
    std::cout << "  -> TEST 2: Route Seat A fails verification. Rollback Seat A." << std::endl;
    resetMockSessions();
    globalAssignHook = [&](const ProcessIdentity& p, const AudioEndpointIdentity& ep) {
        // Mutate mockSessions partially or not at all to simulate failure
        // Wait, if it's a rollback, the first assign fails (simulated by not mutating).
        // Then the second assign (rollback) happens.
        if (ep.endpointId == L"new_endpoint_A") {
            // First routing: do NOT mutate session A, causing verification to fail.
            return AudioRouteStatus::Success; // Backend accepted, but verification fails
        } else {
            // Rollback routing: mutate back
            for (auto& s : mockSessions) {
                if (s.processId == p.pid) {
                    s.endpointId = ep.endpointId;
                    s.endpointStableId = ep.endpointId;
                }
            }
            return AudioRouteStatus::Success;
        }
    };
    runWorkerAndSpin(100, 1000, "new_endpoint_A");
    assert(catcher.lastResult == RouteVerificationResult::FailedRollbackSuccess);
    assert(mockSessions[0].endpointId == L"default_endpoint_A"); // Restored!

    // TEST 3: Route Seat A fails verification. Rollback is attempted but restoration fails.
    std::cout << "  -> TEST 3: Route Seat A fails verification. Rollback fails." << std::endl;
    resetMockSessions();
    globalAssignHook = [&](const ProcessIdentity& p, const AudioEndpointIdentity& ep) {
        // Neither routing nor rollback mutates it correctly
        return AudioRouteStatus::Success;
    };
    runWorkerAndSpin(100, 1000, "new_endpoint_A");
    assert(catcher.lastResult == RouteVerificationResult::FailedRollbackFailed);

    // TEST 4: Simulate an unexpected Seat B endpoint change during/after Seat A routing. Verify cross-seat isolation failure.
    std::cout << "  -> TEST 4: Simulate an unexpected Seat B endpoint change." << std::endl;
    resetMockSessions();
    globalAssignHook = [&](const ProcessIdentity& p, const AudioEndpointIdentity& ep) {
        for (auto& s : mockSessions) {
            if (s.processId == p.pid) {
                s.endpointId = ep.endpointId;
                s.endpointStableId = ep.endpointId;
            } else {
                // Unexpectedly change Seat B's endpoint!
                s.endpointId = L"leaked_endpoint";
                s.endpointStableId = L"leaked_endpoint";
            }
        }
        return AudioRouteStatus::Success;
    };
    runWorkerAndSpin(100, 1000, "new_endpoint_A");
    assert(catcher.lastResult == RouteVerificationResult::CrossSeatIsolationFailure);

    // TEST 5: Target PID is reused between mutation and verification.
    std::cout << "  -> TEST 5: Target PID is reused between mutation and verification." << std::endl;
    resetMockSessions();
    globalAssignHook = [&](const ProcessIdentity& p, const AudioEndpointIdentity& ep) {
        // Change the CreationIdentity of PID 100
        for (auto& s : mockSessions) {
            if (s.processId == p.pid) {
                s.endpointId = ep.endpointId;
                s.endpointStableId = ep.endpointId;
                s.processIdentity = ProcessIdentity{p.pid, 9999}; // Reused!
            }
        }
        return AudioRouteStatus::Success;
    };
    runWorkerAndSpin(100, 1000, "new_endpoint_A");
    // Fails verification and fails rollback because PID was reused.
    assert(catcher.lastResult == RouteVerificationResult::FailedRollbackFailed);

    // TEST 6: Target creationIdentity changes/disappears.
    std::cout << "  -> TEST 6: Target creationIdentity disappears." << std::endl;
    resetMockSessions();
    globalAssignHook = [&](const ProcessIdentity& p, const AudioEndpointIdentity& ep) {
        // Remove PID 100
        mockSessions.erase(mockSessions.begin());
        return AudioRouteStatus::Success;
    };
    runWorkerAndSpin(100, 1000, "new_endpoint_A");
    assert(catcher.lastResult == RouteVerificationResult::FailedRollbackFailed);

    std::cout << "[Test] RoutingWorker Verification tests passed!" << std::endl;
}

#include "test_routing_worker.moc"
