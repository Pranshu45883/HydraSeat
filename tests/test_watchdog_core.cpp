#include "hydra/rollback_registry.hpp"
#include "hydra/startup_policy.hpp"
#include "hydra/watchdog_protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace hydra::watchdog;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

RollbackPlanManifest validManifest() {
    RollbackPlanManifest manifest;
    manifest.lease.sessionId[0] = 0x42u;
    manifest.lease.generation = 3u;
    manifest.lease.timeoutMilliseconds = 1000u;
    manifest.rollbackTimeoutMilliseconds = 2000u;

    RollbackActionDescriptor action;
    action.actionId = 1u;
    action.kind = RollbackActionKind::TerminateOwnedProcess;
    action.activationOrdinal = 1u;
    action.timeoutMilliseconds = 500u;
    action.generation = 3u;
    action.process.processId = 1234u;
    action.process.creationTime100ns = 5678u;
    manifest.actions.push_back(action);
    return manifest;
}

class FakeExecutor final : public RollbackExecutor {
public:
    int calls{0};

    RollbackActionOutcome terminateOwnedProcess(
        const RollbackActionDescriptor& action,
        std::uint32_t) override {
        ++calls;
        return {action.actionId, action.kind, RollbackActionResult::Success, 0u};
    }

    RollbackActionOutcome closeOwnedSession(
        const RollbackActionDescriptor& action,
        std::uint32_t) override {
        return {action.actionId, action.kind, RollbackActionResult::Success, 0u};
    }

    RollbackActionOutcome clearOptionalBackendState(
        const RollbackActionDescriptor& action,
        std::uint32_t) override {
        return {action.actionId, action.kind, RollbackActionResult::Success, 0u};
    }

    RollbackActionOutcome releaseOverlayState(
        const RollbackActionDescriptor& action,
        std::uint32_t) override {
        return {action.actionId, action.kind, RollbackActionResult::Success, 0u};
    }

    RollbackActionOutcome restoreSnapshotState(
        const RollbackActionDescriptor& action,
        std::uint32_t) override {
        return {action.actionId, action.kind, RollbackActionResult::Success, 0u};
    }

    RollbackActionOutcome writeSafeModeResult(
        const RollbackActionDescriptor& action,
        std::uint32_t) override {
        return {action.actionId, action.kind, RollbackActionResult::Success, 0u};
    }
};

void testProtocolRoundTrip() {
    const auto manifest = validManifest();
    std::string error;
    check(validateRollbackPlan(manifest, &error),
          "valid rollback manifest passes validation");

    const auto bytes = encodeRegisterPlan(7u, manifest);
    check(!bytes.empty(), "register-plan frame encodes");

    const auto frame = decodeWatchdogFrame(bytes);
    check(static_cast<bool>(frame), "watchdog frame decodes");
    if (!frame) return;

    RollbackPlanManifest decoded;
    check(decodeRegisterPlan(*frame.frame, decoded, &error),
          "register-plan payload decodes");
    check(decoded == manifest, "register-plan round trip preserves manifest");

    WatchdogLease renewed;
    const auto renewal = encodeLeaseRenewal(8u, manifest.lease);
    const auto renewalFrame = decodeWatchdogFrame(renewal);
    check(renewalFrame && decodeLeaseRenewal(*renewalFrame.frame, renewed, &error),
          "lease renewal round trip succeeds");
    check(renewed == manifest.lease, "lease renewal preserves identity");
}

void testFailClosedValidation() {
    auto manifest = validManifest();
    manifest.lease.sessionId = {};
    std::string error;
    check(!validateRollbackPlan(manifest, &error),
          "zero session identity is rejected");

    manifest = validManifest();
    manifest.actions.front().process.creationTime100ns = 0u;
    check(!validateRollbackPlan(manifest, &error),
          "PID without creation identity is rejected");
}

void testRollbackIdempotency() {
    const auto manifest = validManifest();
    RollbackRegistry registry;
    std::string error;
    check(registry.registerPlan(manifest, &error),
          "rollback registry arms valid plan");

    FakeExecutor executor;
    const auto first = registry.execute(executor);
    check(first.allSatisfied && !first.recoveryRequired && executor.calls == 1,
          "first rollback executes exact action once");

    const auto second = registry.execute(executor);
    check(second.allSatisfied && !second.recoveryRequired && executor.calls == 1,
          "repeated rollback is idempotent");
    check(second.outcomes.size() == 1u &&
              second.outcomes.front().result == RollbackActionResult::AlreadySatisfied,
          "repeated rollback reports already-satisfied action");
}

} // namespace

int main() {
    testProtocolRoundTrip();
    testFailClosedValidation();
    testRollbackIdempotency();

    if (failures != 0) {
        std::cerr << failures << " watchdog core test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Watchdog core tests passed.\n";
    return EXIT_SUCCESS;
}
