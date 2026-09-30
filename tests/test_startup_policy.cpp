#include "hydra/startup_policy.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>

namespace {
using namespace hydra::startup;
int failures = 0;
void check(bool condition, const char* message) { if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; } }

StartupConfig config(StartupMode mode) {
    StartupConfig value;
    value.mode = mode;
    value.revision = 7u;
    value.userApprovedRegistration = mode != StartupMode::Manual;
    if (mode == StartupMode::AutoActivateValidatedSession) value.validatedSessionId = "session-validated-1";
    return value;
}

class Store final : public StartupRegistrationStore {
public:
    std::optional<StartupConfig> current;
    bool readOk{true}, writeOk{true}, removeOk{true}, verifyOk{true};
    int reads{0}, writes{0}, removes{0}, verifies{0};
    bool read(std::optional<StartupConfig>& out) noexcept override { ++reads; if (!readOk) return false; out = current; return true; }
    bool write(const StartupConfig& value) noexcept override { ++writes; if (!writeOk) return false; current = value; return true; }
    bool remove() noexcept override { ++removes; if (!removeOk) return false; current.reset(); return true; }
    bool verify(const std::optional<StartupConfig>& expected) noexcept override { ++verifies; return verifyOk && current == expected; }
};

void testModesAndSafeFallbacks() {
    StartupEnvironment environment;
    check(evaluateStartup(config(StartupMode::Manual), environment).action == StartupAction::DoNotStart,
          "Manual never creates a background start action");
    check(evaluateStartup(config(StartupMode::BackgroundIdle), environment).action == StartupAction::StartBackgroundIdle,
          "BackgroundIdle starts only idle host state");
    environment.selectedSessionStillValidated = true;
    auto automatic = evaluateStartup(config(StartupMode::AutoActivateValidatedSession), environment);
    check(automatic.action == StartupAction::AutoActivateValidatedSession && automatic.sessionId == "session-validated-1",
          "validated auto mode may activate only its explicit selected session");

    environment.journalClean = false;
    check(evaluateStartup(config(StartupMode::AutoActivateValidatedSession), environment).action == StartupAction::StartBackgroundIdle,
          "unsafe journal falls back to Idle rather than activation");
    environment.journalClean = true; environment.safeModeClear = false;
    check(evaluateStartup(config(StartupMode::AutoActivateValidatedSession), environment).reason == StartupReason::SafeMode,
          "safe mode prevents auto activation");
    environment.safeModeClear = true; environment.topologyMatches = false;
    check(evaluateStartup(config(StartupMode::AutoActivateValidatedSession), environment).reason == StartupReason::TopologyChanged,
          "changed topology prevents auto activation");
    environment.topologyMatches = true; environment.recoveryReady = false;
    check(evaluateStartup(config(StartupMode::AutoActivateValidatedSession), environment).reason == StartupReason::RecoveryUnavailable,
          "missing recovery prevents auto activation");
}

void testDuplicateHostAndInvalidAutoConfig() {
    auto environment = StartupEnvironment{};
    environment.existingHostProcesses = 1u;
    check(evaluateStartup(config(StartupMode::BackgroundIdle), environment).action == StartupAction::ReuseExistingHost,
          "exactly one existing host is reused instead of creating a duplicate");
    environment.existingHostProcesses = 2u;
    const auto ambiguousHosts = evaluateStartup(config(StartupMode::BackgroundIdle), environment);
    check(ambiguousHosts.action == StartupAction::DoNotStart &&
              ambiguousHosts.reason == StartupReason::DuplicateHost,
          "multiple existing hosts fail closed instead of selecting an ambiguous control authority");
    auto invalid = config(StartupMode::AutoActivateValidatedSession);
    invalid.validatedSessionId.reset();
    check(evaluateStartup(invalid, StartupEnvironment{}).reason == StartupReason::InvalidConfig,
          "auto activation without an exact validated session fails closed");
}

void testRegistrationApprovalDisableAndRollback() {
    Store store;
    auto background = config(StartupMode::BackgroundIdle);
    check(updateRegistration(background, store).succeeded() && store.current == background,
          "user-approved background registration writes and verifies typed config");

    auto manual = config(StartupMode::Manual);
    check(updateRegistration(manual, store).succeeded() && !store.current,
          "Manual mode removes registration for disable/uninstall cleanup");

    background.userApprovedRegistration = false;
    const int writesBefore = store.writes;
    check(updateRegistration(background, store).code == RegistrationCode::ApprovalRequired && store.writes == writesBefore,
          "background registration cannot be created without explicit approval");

    store.current = config(StartupMode::BackgroundIdle);
    const auto previous = store.current;
    store.verifyOk = false;
    auto automatic = config(StartupMode::AutoActivateValidatedSession);
    auto failed = updateRegistration(automatic, store);
    check(failed.code == RegistrationCode::RollbackFailed,
          "verification plus unverifiable rollback is surfaced as RollbackFailed");
    store.verifyOk = true;
    store.current = previous;
    store.writeOk = false;
    failed = updateRegistration(automatic, store);
    check(failed.code == RegistrationCode::RollbackFailed,
          "write failure with failed restoration is explicit rollback failure");
}

} // namespace
int main() {
    testModesAndSafeFallbacks();
    testDuplicateHostAndInvalidAutoConfig();
    testRegistrationApprovalDisableAndRollback();
    if (failures) { std::cerr << failures << " startup policy test(s) failed.\n"; return EXIT_FAILURE; }
    std::cout << "Startup policy tests passed.\n";
    return EXIT_SUCCESS;
}
