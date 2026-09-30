#include "hydra/runtime_authority.hpp"
#include "hydra/process_identity.hpp"
#include "hydra/audio_router.hpp"

#include <cassert>
#include <iostream>

namespace {
    class MockAudioRouter : public hydra::runtime::AudioRouter {
    public:
        hydra::runtime::AudioRouteStatus assignEndpoint(
            const hydra::runtime::ProcessIdentity& process,
            const hydra::runtime::AudioEndpointIdentity& endpoint) noexcept override {
            if (!process.valid()) return hydra::runtime::AudioRouteStatus::InvalidProcess;
            lastProcess = process;
            lastEndpoint = endpoint;
            return hydra::runtime::AudioRouteStatus::Success;
        }

        hydra::runtime::AudioRouteStatus clearAssignment(
            const hydra::runtime::ProcessIdentity& process) noexcept override {
            if (!process.valid()) return hydra::runtime::AudioRouteStatus::InvalidProcess;
            clearedProcess = process;
            return hydra::runtime::AudioRouteStatus::Success;
        }

        hydra::runtime::ProcessIdentity lastProcess;
        hydra::runtime::AudioEndpointIdentity lastEndpoint;
        hydra::runtime::ProcessIdentity clearedProcess;
    };
}

void testAudioRouter() {
    std::cout << "[Test] Running Audio Router tests..." << std::endl;

    // 1-5. Process identity
    hydra::runtime::ProcessIdentity validProcess{1234, 5678};
    assert(validProcess.valid());

    hydra::runtime::ProcessIdentity missingCreation{1234, 0};
    assert(!missingCreation.valid()); // Missing creationIdentity fails closed

    // 6-9. Seat authority
    hydra::runtime::SessionController controller;
    const auto token1 = controller.acquireSeatLease(1, hydra::runtime::LeaseClass::UiConfiguration);
    assert(token1.valid());
    
    assert(controller.publishProcess(token1, validProcess));

    hydra::runtime::AudioEndpointIdentity endpoint1{L"endpoint-A", std::nullopt};
    assert(controller.bindAudioEndpoint(token1, endpoint1));

    MockAudioRouter router;
    
    // Valid route
    assert(static_cast<int>(controller.applyAudioRoute(token1, router)) == static_cast<int>(hydra::runtime::AudioRouteStatus::Success));
    assert(router.lastProcess == validProcess);
    assert(router.lastEndpoint == endpoint1);

    // Stale activation token (stale generation)
    hydra::runtime::ActivationToken staleToken{1, token1.generation - 1, hydra::runtime::LeaseClass::UiConfiguration};
    assert(static_cast<int>(controller.applyAudioRoute(staleToken, router)) == static_cast<int>(hydra::runtime::AudioRouteStatus::InvalidProcess));

    // Missing authority (invalid token)
    hydra::runtime::ActivationToken missingToken{0, 0, hydra::runtime::LeaseClass::UiConfiguration};
    assert(static_cast<int>(controller.applyAudioRoute(missingToken, router)) == static_cast<int>(hydra::runtime::AudioRouteStatus::InvalidProcess));

    // Authority revoked
    controller.releaseSeatLease(token1);
    assert(static_cast<int>(controller.applyAudioRoute(token1, router)) == static_cast<int>(hydra::runtime::AudioRouteStatus::InvalidProcess));

    // Re-activate Seat 1 for remaining tests
    const auto newToken1 = controller.acquireSeatLease(1, hydra::runtime::LeaseClass::UiConfiguration);
    assert(controller.publishProcess(newToken1, validProcess));
    assert(controller.bindAudioEndpoint(newToken1, endpoint1));

    // 10-11. Concurrency
    const auto token2 = controller.acquireSeatLease(2, hydra::runtime::LeaseClass::UiConfiguration);
    hydra::runtime::ProcessIdentity process2{4321, 8765};
    assert(controller.publishProcess(token2, process2));
    hydra::runtime::AudioEndpointIdentity endpoint2{L"endpoint-B", std::nullopt};
    assert(controller.bindAudioEndpoint(token2, endpoint2));

    // Route for seat 2
    assert(static_cast<int>(controller.applyAudioRoute(token2, router)) == static_cast<int>(hydra::runtime::AudioRouteStatus::Success));
    assert(router.lastProcess == process2);
    assert(router.lastEndpoint == endpoint2);

    // Rollback (clear) Seat 1
    assert(static_cast<int>(controller.clearAudioRoute(newToken1, router)) == static_cast<int>(hydra::runtime::AudioRouteStatus::Success));
    assert(router.clearedProcess == validProcess);

    // Seat A cannot mutate Seat B:
    // If we pass newToken1 to applyAudioRoute, it will ONLY mutate Seat 1.
    // There is no API for token1 to mutate seat2, ensuring isolation.
    
    // Ensure seat 2 is unaffected by Seat 1's rollback
    auto snap2 = controller.snapshot(2);
    assert(snap2->audioEndpoint == endpoint2);
    assert(snap2->process == process2);
}
    std::cout << "[Test] Audio Router tests passed." << std::endl;
}
