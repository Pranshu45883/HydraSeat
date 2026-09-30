#include "hydra/hidhide_session_recovery.hpp"

#include "phase3_hardware_evidence_fixture.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace hydra;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class FakePlatform final : public HidHideSessionPlatform {
public:
    HidHideSessionSnapshot state;
    bool supportsMutation{true};
    unsigned reads{0};
    unsigned writes{0};
    unsigned sessionAdds{0};
    unsigned sessionClears{0};

    bool readState(HidHideSessionSnapshot& snapshot,
                   std::string& error) noexcept override {
        ++reads;
        snapshot = state;
        error.clear();
        return true;
    }

    bool writeState(const HidHideSessionSnapshot& snapshot,
                    std::string& error) noexcept override {
        ++writes;
        state = snapshot;
        error.clear();
        return true;
    }

    bool addSessionBlacklist(std::span<const std::wstring>,
                             std::string& error) noexcept override {
        ++sessionAdds;
        error.clear();
        return true;
    }

    bool clearSessionBlacklist(std::string& error) noexcept override {
        ++sessionClears;
        error.clear();
        return true;
    }

    bool mutationSupported() const noexcept override { return supportsMutation; }
    bool sessionBlacklistSupported() const noexcept override { return true; }
};

std::filesystem::path tempRoot(std::string_view name) {
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    auto path = std::filesystem::temp_directory_path() /
        (std::string("hydra-hidhide-") + std::string(name) + "-" +
         std::to_string(stamp));
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    return path;
}

HidHideSessionSnapshot beforeState() {
    HidHideSessionSnapshot state;
    state.active = false;
    state.inverseWhitelist = false;
    state.blockedDeviceInstanceIds = {L"HID\\PERSISTENT\\DEVICE"};
    state.allowedApplications = {
        L"\\Device\\HarddiskVolume3\\Existing\\tool.exe"};
    return state;
}

HidHideSessionSnapshot appliedState() {
    auto state = beforeState();
    state.active = true;
    state.allowedApplications.push_back(
        L"\\Device\\HarddiskVolume3\\HydraSeat\\hydra_host.exe");
    return state;
}

HidHideSessionRecoveryRecord record(std::uint64_t resourceId = 0x42,
                                    std::uint64_t generation = 7) {
    return {resourceId, generation, beforeState(), appliedState()};
}

HidHideSessionRequest guardedRequest(
    const Phase3HardwareAcceptanceEvidence& evidence,
    std::uint64_t generation = 7) {
    HidHideSessionRequest request;
    request.deviceInstanceIds = test::SyntheticPhase3EvidenceFixture::requestedDeviceInstanceIds();
    request.allowedApplications = {
        L"\\Device\\HarddiskVolume3\\HydraSeat\\hydra_host.exe"};
    request.replacementPathVerified = true;
    request.recoveryReady = true;
    request.spareRecoveryInputPresent = true;
    request.physicalAcceptanceEvidence = evidence;
    request.nativeMutationApproved = true;
    request.expiryMilliseconds = 30'000;
    request.generation = generation;
    return request;
}

watchdog::RollbackPlanManifest manifest(
    const watchdog::RollbackActionDescriptor& action) {
    watchdog::RollbackPlanManifest value;
    value.lease.sessionId[0] = 1;
    value.lease.generation = action.generation;
    value.lease.timeoutMilliseconds = 1'000;
    value.rollbackTimeoutMilliseconds = 2'000;
    value.actions = {action};
    return value;
}

void testCodecAndValidation() {
    const auto original = record();
    std::string error;
    check(validateHidHideSessionRecoveryRecord(original, &error),
          "valid HidHide recovery record passes validation");
    const auto bytes = encodeHidHideSessionRecoveryRecord(original);
    check(!bytes.empty() && bytes.size() <= kHidHideSnapshotMaxFileBytes,
          "valid recovery record encodes within the bounded file size");
    const auto decoded = decodeHidHideSessionRecoveryRecord(bytes, &error);
    check(decoded.has_value() && *decoded == original,
          "HidHide recovery record round-trips exactly");

    auto corrupt = bytes;
    corrupt.back() ^= std::byte{0x01};
    check(!decodeHidHideSessionRecoveryRecord(corrupt, &error).has_value(),
          "corrupt recovery bytes fail checksum validation");

    auto persistentMutation = original;
    persistentMutation.applied.blockedDeviceInstanceIds.push_back(
        L"HID\\HYDRASEAT\\MUST_NOT_PERSIST");
    check(!validateHidHideSessionRecoveryRecord(persistentMutation, &error),
          "recovery record rejects HydraSeat persistent-blacklist mutation");

    auto duplicate = original;
    const auto duplicateApplication = duplicate.applied.allowedApplications.front();
    duplicate.applied.allowedApplications.push_back(duplicateApplication);
    check(!validateHidHideSessionRecoveryRecord(duplicate, &error),
          "recovery record rejects duplicate bounded identities");
}

void testDurableStore() {
    const auto root = tempRoot("store");
    HidHideSessionSnapshotStore store(root);
    std::string error;
    const auto original = record(0x1234, 9);
    check(store.write(original, &error),
          "bounded recovery record is written below the recovery root");
    const auto loaded = store.load(0x1234);
    check(loaded.status == HidHideSnapshotReadStatus::Success &&
              loaded.record.has_value() && *loaded.record == original,
          "durable recovery store loads the exact resource record");
    check(store.load(0x9999).status == HidHideSnapshotReadStatus::Missing,
          "unknown numeric resource ID cannot select an arbitrary file");
    check(store.remove(0x1234, &error) &&
              store.load(0x1234).status == HidHideSnapshotReadStatus::Missing,
          "durable recovery resource removes idempotently after verified cleanup");
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

void testGuardedOrderingAndCleanup() {
    const auto root = tempRoot("guarded");
    auto platform = std::make_shared<FakePlatform>();
    platform->state = beforeState();
    GuardedHidHideSession guarded(platform, root, 0x55, 31, 4, 2'000);
    test::SyntheticPhase3EvidenceFixture evidence;

    const auto prepared = guarded.prepare(guardedRequest(evidence.evidence(), 13), 1'000);
    check(prepared.succeeded() && prepared.phase == HidHideSessionPhase::Prepared &&
              guarded.rollbackAction().has_value(),
          "guarded session persists a recovery plan before exposing activation");
    HidHideSessionSnapshotStore store(root);
    const auto durable = store.load(0x55);
    check(durable.status == HidHideSnapshotReadStatus::Success &&
              durable.record.has_value() && durable.record->generation == 13,
          "guarded prepare writes the exact generation-bound durable snapshot first");

    const auto unarmed = guarded.activate(1'100);
    check(unarmed.code == HidHideSessionResultCode::RecoveryNotArmed &&
              platform->writes == 0 && platform->sessionAdds == 0,
          "native mutation is impossible before the exact rollback action is armed");

    auto wrongAction = *guarded.rollbackAction();
    ++wrongAction.actionId;
    std::string error;
    check(!guarded.confirmRecoveryArmed(wrongAction, &error) &&
              !guarded.recoveryArmed(),
          "different watchdog recovery identity cannot authorize activation");
    const auto exactAction = *guarded.rollbackAction();
    check(guarded.confirmRecoveryArmed(exactAction, &error) &&
              guarded.recoveryArmed(),
          "exact durable rollback action authorizes guarded activation");

    const auto activated = guarded.activate(1'200);
    check(activated.succeeded() && activated.phase == HidHideSessionPhase::Active &&
              platform->writes == 1 && platform->sessionAdds == 1,
          "guarded activation runs only after durable recovery is armed");
    const auto rolledBack = guarded.rollback();
    check(rolledBack.succeeded() && rolledBack.phase == HidHideSessionPhase::Idle &&
              equivalentHidHideSessionSnapshots(platform->state, beforeState()) &&
              platform->writes == 2 && platform->sessionClears == 1,
          "normal guarded rollback clears session ownership and restores persistent state");
    check(store.load(0x55).status == HidHideSnapshotReadStatus::Missing &&
              !guarded.rollbackAction().has_value() && !guarded.recoveryArmed(),
          "verified clean rollback removes the stale durable recovery resource");

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

void testRollbackRegistryRestore() {
    const auto root = tempRoot("rollback");
    HidHideSessionSnapshotStore store(root);
    std::string error;
    const auto original = record(0x42, 7);
    check(store.write(original, &error), "rollback fixture persists exact snapshot");

    auto platform = std::make_shared<FakePlatform>();
    platform->state = original.applied;
    HidHideSessionRollbackExecutor executor(root, platform);
    const auto action = makeHidHideSessionRollbackAction(10, 3, 1'000, 7, 0x42);
    watchdog::RollbackRegistry registry;
    check(registry.registerPlan(manifest(action), &error),
          "watchdog registry accepts bounded HidHide restore action");
    const auto summary = registry.execute(executor);
    check(summary.allSatisfied && !summary.recoveryRequired &&
              summary.outcomes.size() == 1 &&
              summary.outcomes.front().result == watchdog::RollbackActionResult::Success,
          "watchdog registry executes HidHide restore as a satisfied action");
    check(equivalentHidHideSessionSnapshots(platform->state, original.before) &&
              platform->writes == 1,
          "restore executor returns exact persistent state to the pre-session snapshot");

    watchdog::RollbackRegistry already;
    check(already.registerPlan(manifest(action), &error),
          "idempotent restore fixture rearms independently");
    const auto repeated = already.execute(executor);
    check(repeated.allSatisfied && repeated.outcomes.front().result ==
              watchdog::RollbackActionResult::AlreadySatisfied &&
              platform->writes == 1,
          "already-restored persistent state is verified without another write");

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

void testExternalDriftAndIdentityMismatchFailClosed() {
    const auto root = tempRoot("drift");
    HidHideSessionSnapshotStore store(root);
    std::string error;
    const auto original = record(0x77, 11);
    check(store.write(original, &error), "drift fixture persists recovery record");

    auto platform = std::make_shared<FakePlatform>();
    platform->state = original.applied;
    platform->state.allowedApplications.push_back(
        L"\\Device\\HarddiskVolume3\\Other\\changed.exe");
    HidHideSessionRollbackExecutor executor(root, platform);
    const auto action = makeHidHideSessionRollbackAction(20, 4, 1'000, 11, 0x77);
    const auto drift = executor.restoreSnapshotState(action, 1'000);
    check(drift.result == watchdog::RollbackActionResult::IdentityMismatch &&
              platform->writes == 0,
          "external persistent HidHide drift is never overwritten by stale recovery state");

    auto wrongGeneration = action;
    wrongGeneration.generation = 12;
    platform->state = original.applied;
    const auto identity = executor.restoreSnapshotState(wrongGeneration, 1'000);
    check(identity.result == watchdog::RollbackActionResult::IdentityMismatch &&
              platform->writes == 0,
          "snapshot generation mismatch fails closed before mutation");

    auto missing = action;
    missing.resourceId = 0x88;
    const auto missingResult = executor.restoreSnapshotState(missing, 1'000);
    check(missingResult.result == watchdog::RollbackActionResult::Failed &&
              platform->writes == 0,
          "missing durable snapshot is recovery failure rather than assumed success");

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

} // namespace

int main() {
    testCodecAndValidation();
    testDurableStore();
    testGuardedOrderingAndCleanup();
    testRollbackRegistryRestore();
    testExternalDriftAndIdentityMismatchFailClosed();

    if (failures != 0) {
        std::cerr << failures << " HidHide session recovery test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "HidHide session recovery tests passed.\n";
    return EXIT_SUCCESS;
}
