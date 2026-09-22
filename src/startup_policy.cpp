#include "hydra/startup_policy.hpp"

#include <string>
#include <utility>

namespace hydra::startup {
namespace {

bool validMode(StartupMode mode) noexcept {
    return mode == StartupMode::Manual || mode == StartupMode::BackgroundIdle ||
           mode == StartupMode::AutoActivateValidatedSession;
}

bool validId(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128u) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
              ch == ':')) return false;
    }
    return true;
}

bool validConfig(const StartupConfig& config) noexcept {
    if (config.schemaVersion != kStartupPolicyVersion || !validMode(config.mode) ||
        config.revision == 0u) return false;
    if (config.mode == StartupMode::AutoActivateValidatedSession) {
        return config.userApprovedRegistration && config.validatedSessionId &&
               validId(*config.validatedSessionId);
    }
    return !config.validatedSessionId.has_value();
}

RegistrationDiagnostic fail(RegistrationCode code, std::string message) {
    return {code, std::move(message)};
}

bool restore(const std::optional<StartupConfig>& previous, StartupRegistrationStore& store) {
    if (previous) {
        if (!store.write(*previous)) return false;
    } else if (!store.remove()) {
        return false;
    }
    return store.verify(previous);
}

} // namespace

StartupDecision evaluateStartup(const StartupConfig& config,
                                const StartupEnvironment& environment) noexcept {
    if (!validConfig(config) ||
        (config.mode != StartupMode::Manual && !config.userApprovedRegistration)) {
        return {StartupAction::DoNotStart, StartupReason::InvalidConfig, {}};
    }
    if (config.mode == StartupMode::Manual) {
        return {StartupAction::DoNotStart, StartupReason::ManualMode, {}};
    }
    if (environment.existingHostProcesses > 1u) {
        return {StartupAction::DoNotStart, StartupReason::DuplicateHost, {}};
    }
    if (environment.existingHostProcesses == 1u) {
        return {StartupAction::ReuseExistingHost, StartupReason::DuplicateHost, {}};
    }
    if (config.mode == StartupMode::BackgroundIdle) {
        return {StartupAction::StartBackgroundIdle, StartupReason::BackgroundMode, {}};
    }

    if (!environment.journalClean) {
        return {StartupAction::StartBackgroundIdle, StartupReason::UnsafeJournal, {}};
    }
    if (!environment.safeModeClear) {
        return {StartupAction::StartBackgroundIdle, StartupReason::SafeMode, {}};
    }
    if (!environment.topologyMatches) {
        return {StartupAction::StartBackgroundIdle, StartupReason::TopologyChanged, {}};
    }
    if (!environment.capabilitiesReady) {
        return {StartupAction::StartBackgroundIdle, StartupReason::CapabilityMissing, {}};
    }
    if (!environment.recoveryReady) {
        return {StartupAction::StartBackgroundIdle, StartupReason::RecoveryUnavailable, {}};
    }
    if (!environment.selectedSessionStillValidated) {
        return {StartupAction::StartBackgroundIdle, StartupReason::SessionNotValidated, {}};
    }
    return {StartupAction::AutoActivateValidatedSession, StartupReason::ValidatedSessionReady,
            config.validatedSessionId};
}

RegistrationDiagnostic updateRegistration(const StartupConfig& desired,
                                          StartupRegistrationStore& store) {
    if (!validConfig(desired)) return fail(RegistrationCode::InvalidConfig, "startup config is invalid");
    if (desired.mode != StartupMode::Manual && !desired.userApprovedRegistration) {
        return fail(RegistrationCode::ApprovalRequired,
                    "background startup registration requires explicit user approval");
    }

    std::optional<StartupConfig> previous;
    if (!store.read(previous)) return fail(RegistrationCode::ReadFailed, "startup registration state read failed");

    const bool changed = desired.mode == StartupMode::Manual ? store.remove() : store.write(desired);
    if (!changed) {
        const bool restored = restore(previous, store);
        return restored
                   ? fail(desired.mode == StartupMode::Manual ? RegistrationCode::RemoveFailedRolledBack
                                                              : RegistrationCode::WriteFailedRolledBack,
                          "startup registration mutation failed and previous state was restored")
                   : fail(RegistrationCode::RollbackFailed,
                          "startup registration mutation and rollback both failed");
    }

    const std::optional<StartupConfig> expected =
        desired.mode == StartupMode::Manual ? std::nullopt : std::optional<StartupConfig>{desired};
    if (!store.verify(expected)) {
        return restore(previous, store)
                   ? fail(RegistrationCode::VerifyFailedRolledBack,
                          "startup registration verification failed and previous state was restored")
                   : fail(RegistrationCode::RollbackFailed,
                          "startup registration verification and rollback failed");
    }
    return {};
}

std::string_view startupModeName(StartupMode value) noexcept {
    switch (value) {
        case StartupMode::Manual: return "Manual";
        case StartupMode::BackgroundIdle: return "BackgroundIdle";
        case StartupMode::AutoActivateValidatedSession: return "AutoActivateValidatedSession";
    }
    return "Unknown";
}
std::string_view startupActionName(StartupAction value) noexcept {
    switch (value) {
        case StartupAction::DoNotStart: return "DoNotStart";
        case StartupAction::StartBackgroundIdle: return "StartBackgroundIdle";
        case StartupAction::AutoActivateValidatedSession: return "AutoActivateValidatedSession";
        case StartupAction::ReuseExistingHost: return "ReuseExistingHost";
    }
    return "Unknown";
}
std::string_view startupReasonName(StartupReason value) noexcept {
    switch (value) {
        case StartupReason::ManualMode: return "ManualMode";
        case StartupReason::BackgroundMode: return "BackgroundMode";
        case StartupReason::ValidatedSessionReady: return "ValidatedSessionReady";
        case StartupReason::DuplicateHost: return "DuplicateHost";
        case StartupReason::UnsafeJournal: return "UnsafeJournal";
        case StartupReason::SafeMode: return "SafeMode";
        case StartupReason::TopologyChanged: return "TopologyChanged";
        case StartupReason::CapabilityMissing: return "CapabilityMissing";
        case StartupReason::RecoveryUnavailable: return "RecoveryUnavailable";
        case StartupReason::SessionNotValidated: return "SessionNotValidated";
        case StartupReason::InvalidConfig: return "InvalidConfig";
    }
    return "Unknown";
}
std::string_view registrationCodeName(RegistrationCode value) noexcept {
    switch (value) {
        case RegistrationCode::Success: return "Success";
        case RegistrationCode::InvalidConfig: return "InvalidConfig";
        case RegistrationCode::ApprovalRequired: return "ApprovalRequired";
        case RegistrationCode::ReadFailed: return "ReadFailed";
        case RegistrationCode::WriteFailedRolledBack: return "WriteFailedRolledBack";
        case RegistrationCode::RemoveFailedRolledBack: return "RemoveFailedRolledBack";
        case RegistrationCode::VerifyFailedRolledBack: return "VerifyFailedRolledBack";
        case RegistrationCode::RollbackFailed: return "RollbackFailed";
    }
    return "Unknown";
}

} // namespace hydra::startup
