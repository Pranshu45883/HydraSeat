#include "hydra/gate_c_recovery.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace hydra::gatec;
using namespace hydra::recovery;
using namespace hydra::watchdog;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

SessionId session(std::uint8_t seed = 0x20) {
    SessionId value{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<std::uint8_t>(seed + index + 1);
    }
    return value;
}

GateCRecoveryTarget target(std::uint32_t id, std::uint32_t pid,
                           std::uint64_t created) {
    GateCRecoveryTarget value;
    value.actionId = id;
    value.activationOrdinal = id;
    value.generation = 7;
    value.process = {pid, created};
    return value;
}

class MemoryStorage final : public CrashJournalStorage {
public:
    JournalReadResult read(JournalStorageSlot slot,
                           std::size_t maxBytes) override {
        const auto found = files.find(slot);
        if (found == files.end()) return {JournalReadStatus::Missing, {}, 0};
        if (found->second.size() > maxBytes) {
            return {JournalReadStatus::TooLarge, {}, 0};
        }
        return {JournalReadStatus::Success, found->second, 0};
    }

    bool durableWrite(JournalStorageSlot slot,
                      std::span<const std::byte> bytes,
                      std::uint32_t* systemError) override {
        if (failCurrentTempOnce && slot == JournalStorageSlot::CurrentTemp) {
            failCurrentTempOnce = false;
            if (systemError != nullptr) *systemError = 112;
            return false;
        }
        files[slot] = std::vector<std::byte>(bytes.begin(), bytes.end());
        if (systemError != nullptr) *systemError = 0;
        return true;
    }

    bool atomicReplace(JournalStorageSlot from, JournalStorageSlot to,
                       std::uint32_t* systemError) override {
        const auto found = files.find(from);
        if (found == files.end()) return false;
        files[to] = std::move(found->second);
        files.erase(found);
        if (systemError != nullptr) *systemError = 0;
        return true;
    }

    bool remove(JournalStorageSlot slot,
                std::uint32_t* systemError) override {
        files.erase(slot);
        if (systemError != nullptr) *systemError = 0;
        return true;
    }

    std::map<JournalStorageSlot, std::vector<std::byte>> files;
    bool failCurrentTempOnce{false};
};

void testPlanIsNarrowAndDeterministic() {
    const std::vector targets{
        target(1, 1001, 0x10101010ull),
        target(2, 1002, 0x20202020ull),
    };
    std::string error;
    const auto first = makeGateCRecoveryPlan(
        session(), 7, 2'000, 5'000, targets, &error);
    const auto second = makeGateCRecoveryPlan(
        session(), 7, 2'000, 5'000, targets, &error);
    check(first.has_value() && second == first,
          "Gate C recovery plan is deterministic");
    check(first->actions.size() == 2,
          "Gate C recovery plan covers every controlled target");
    for (std::size_t index = 0; index < first->actions.size(); ++index) {
        const auto& action = first->actions[index];
        check(action.kind == RollbackActionKind::TerminateOwnedProcess &&
                  action.resourceId == 0 &&
                  action.process == targets[index].process,
              "Gate C recovery plan contains only exact process termination");
    }

    auto duplicate = targets;
    duplicate[1].actionId = duplicate[0].actionId;
    check(!makeGateCRecoveryPlan(session(), 7, 2'000, 5'000,
                                 duplicate, &error),
          "duplicate rollback identities fail closed");
    auto invalid = targets;
    invalid[0].process.creationTime100ns = 0;
    check(!makeGateCRecoveryPlan(session(), 7, 2'000, 5'000,
                                 invalid, &error),
          "missing process creation identity fails closed");
    auto duplicateProcess = targets;
    duplicateProcess[1].process = duplicateProcess[0].process;
    check(!makeGateCRecoveryPlan(session(), 7, 2'000, 5'000,
                                 duplicateProcess, &error),
          "one exact process identity cannot be registered twice");
}

void testJournalLifecycle() {
    const std::vector targets{
        target(1, 2001, 0x30303030ull),
        target(2, 2002, 0x40404040ull),
    };
    std::string error;
    const auto manifest = makeGateCRecoveryPlan(
        session(0x40), 11, 2'000, 5'000, targets, &error);
    check(manifest.has_value(), "journal fixture plan is valid");

    MemoryStorage storage;
    CrashJournalStore store(storage);
    GateCRecoveryJournal journal(store, *manifest, 11);
    check(journal.begin({}, &error), "recovery journal begins durably");
    for (const auto& action : manifest->actions) {
        check(journal.prepareAction(action.actionId, &error),
              "rollback action preparation persists");
        check(journal.markActionApplied(action.actionId, &error),
              "rollback action activation persists");
        check(journal.markActionVerified(action.actionId, &error),
              "rollback action verification persists");
    }
    check(journal.commitActivation(&error),
          "fully guarded Gate C activation commits");
    check(journal.state() != nullptr &&
              journal.state()->phase == CrashJournalPhase::Active,
          "journal reaches active only after all actions verify");

    check(journal.beginRollback(&error), "rollback boundary persists first");
    for (auto it = manifest->actions.rbegin();
         it != manifest->actions.rend(); ++it) {
        check(journal.markActionRolledBack(it->actionId, &error),
              "reverse rollback action persists");
    }
    check(journal.verifyRollback(&error),
          "verified process teardown persists");
    check(journal.markCleanStop(&error), "clean stop persists last");
    check(store.assessStartupAndEnterSafeMode().state ==
              StartupRecoveryState::Clean,
          "clean Gate C rollback permits normal next startup");
}

void testDurableFailureDoesNotAdvanceVolatileState() {
    const std::vector targets{target(1, 2501, 0x45454545ull)};
    std::string error;
    const auto manifest = makeGateCRecoveryPlan(
        session(0x50), 12, 2'000, 5'000, targets, &error);
    check(manifest.has_value(), "durable-failure fixture plan is valid");

    MemoryStorage storage;
    CrashJournalStore store(storage);
    GateCRecoveryJournal journal(store, *manifest, 12);
    check(journal.begin({}, &error), "durable-failure fixture journal begins");
    const auto before = *journal.state();
    storage.failCurrentTempOnce = true;
    check(!journal.prepareAction(1, &error),
          "durable transition failure is reported to the caller");
    check(journal.state() != nullptr && *journal.state() == before,
          "failed durable transition cannot advance volatile recovery state");
    const auto marker = store.loadSafeMode(&error);
    check(marker.has_value() &&
              marker->reason == SafeModeReason::JournalWriteFailure,
          "durable transition failure enters journal-write-failure safe mode");
}

void testUnresolvedCleanupBecomesRecoveryRequired() {
    const std::vector targets{target(1, 3001, 0x50505050ull)};
    std::string error;
    const auto manifest = makeGateCRecoveryPlan(
        session(0x60), 13, 2'000, 5'000, targets, &error);
    check(manifest.has_value(), "recovery-required fixture plan is valid");

    MemoryStorage storage;
    CrashJournalStore store(storage);
    GateCRecoveryJournal journal(store, *manifest, 13);
    check(journal.begin({}, &error), "failure fixture journal begins");
    check(journal.prepareAction(1, &error), "failure fixture prepares action");
    check(journal.markActionApplied(1, &error), "failure fixture applies action");
    check(journal.markActionVerified(1, &error), "failure fixture verifies action");
    check(journal.commitActivation(&error), "failure fixture commits activation");
    check(journal.markRecoveryRequired(&error),
          "unresolved cleanup records RecoveryRequired");
    const auto assessment = store.assessStartupAndEnterSafeMode();
    check(assessment.state == StartupRecoveryState::RecoveryRequired &&
              assessment.safeMode.has_value() &&
              assessment.safeMode->reason == SafeModeReason::RecoveryRequired,
          "RecoveryRequired persists into safe-mode startup assessment");
}

} // namespace

int main() {
    testPlanIsNarrowAndDeterministic();
    testJournalLifecycle();
    testDurableFailureDoesNotAdvanceVolatileState();
    testUnresolvedCleanupBecomesRecoveryRequired();
    std::cout << "Gate C recovery core tests passed.\n";
    return EXIT_SUCCESS;
}
