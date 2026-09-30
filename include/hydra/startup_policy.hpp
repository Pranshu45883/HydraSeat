#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hydra::startup {

inline constexpr std::uint32_t kStartupPolicyVersion = 1u;

enum class StartupMode : std::uint8_t {
    Manual = 0,
    BackgroundIdle = 1,
    AutoActivateValidatedSession = 2,
};

struct StartupConfig {
    std::uint32_t schemaVersion{kStartupPolicyVersion};
    StartupMode mode{StartupMode::Manual};
    std::uint64_t revision{0};
    bool userApprovedRegistration{false};
    std::optional<std::string> validatedSessionId;

    bool operator==(const StartupConfig&) const = default;
};

struct StartupEnvironment {
    bool journalClean{true};
    bool safeModeClear{true};
    bool topologyMatches{true};
    bool capabilitiesReady{true};
    bool recoveryReady{true};
    bool selectedSessionStillValidated{false};
    std::uint32_t existingHostProcesses{0};
};

enum class StartupAction : std::uint8_t {
    DoNotStart = 0,
    StartBackgroundIdle = 1,
    AutoActivateValidatedSession = 2,
    ReuseExistingHost = 3,
};

enum class StartupReason : std::uint8_t {
    ManualMode = 0,
    BackgroundMode = 1,
    ValidatedSessionReady = 2,
    DuplicateHost = 3,
    UnsafeJournal = 4,
    SafeMode = 5,
    TopologyChanged = 6,
    CapabilityMissing = 7,
    RecoveryUnavailable = 8,
    SessionNotValidated = 9,
    InvalidConfig = 10,
};

struct StartupDecision {
    StartupAction action{StartupAction::DoNotStart};
    StartupReason reason{StartupReason::ManualMode};
    std::optional<std::string> sessionId;

    bool operator==(const StartupDecision&) const = default;
};

StartupDecision evaluateStartup(const StartupConfig& config,
                                const StartupEnvironment& environment) noexcept;

enum class RegistrationCode : std::uint8_t {
    Success = 0,
    InvalidConfig,
    ApprovalRequired,
    ReadFailed,
    WriteFailedRolledBack,
    RemoveFailedRolledBack,
    VerifyFailedRolledBack,
    RollbackFailed,
};

struct RegistrationDiagnostic {
    RegistrationCode code{RegistrationCode::Success};
    std::string message;
    bool succeeded() const noexcept { return code == RegistrationCode::Success; }
};

// Native adapter owns the fixed HydraSeat logon target. The config contains no
// executable path, command line, registry path, scheduled-task XML, or shell text.
class StartupRegistrationStore {
public:
    virtual ~StartupRegistrationStore() = default;
    virtual bool read(std::optional<StartupConfig>& current) noexcept = 0;
    virtual bool write(const StartupConfig& config) noexcept = 0;
    virtual bool remove() noexcept = 0;
    virtual bool verify(const std::optional<StartupConfig>& expected) noexcept = 0;
};

RegistrationDiagnostic updateRegistration(const StartupConfig& desired,
                                          StartupRegistrationStore& store);

std::string_view startupModeName(StartupMode value) noexcept;
std::string_view startupActionName(StartupAction value) noexcept;
std::string_view startupReasonName(StartupReason value) noexcept;
std::string_view registrationCodeName(RegistrationCode value) noexcept;

} // namespace hydra::startup
