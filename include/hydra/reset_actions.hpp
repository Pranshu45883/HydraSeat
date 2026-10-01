#pragma once

#include "hydra/crash_journal.hpp"
#include "hydra/rollback_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::reset {

inline constexpr std::uint32_t kRuntimeResetRegistrationMagic = 0x31525248u; // "HRR1".
inline constexpr std::uint16_t kRuntimeResetRegistrationLegacyVersion = 1;
inline constexpr std::uint16_t kRuntimeResetRegistrationVersion = 2;
inline constexpr std::size_t kRuntimeResetRegistrationLegacyHeaderBytes = 64;
inline constexpr std::size_t kRuntimeResetRegistrationHeaderBytes = 68;
inline constexpr std::size_t kRuntimeResetRegistrationMaxBytes =
    kRuntimeResetRegistrationHeaderBytes +
    recovery::kRecoveryProcessAttachmentIdentityBytes +
    watchdog::kWatchdogMaxFrameBytes;
inline constexpr std::uint32_t kResetOwnerActionId = 0xfffffff0u;
inline constexpr std::uint32_t kResetOwnerTimeoutMs = 2'000u;

// Version 2 registration is recovery authority written before risky activation.
// It contains the exact owner process identity, the same bounded rollback manifest
// armed in the watchdog, and the full RecoveryProcessAttachmentIdentity. Its
// persisted digest covers all three. Legacy version 1 remains decode-only evidence:
// it may be inspected for diagnosis/clean-metadata cleanup but cannot authorize
// process mutation. The crash journal remains evidence-only; mutating reset requires
// an exact attachment-bound registration correlated to the journal.
struct RuntimeResetRegistration {
    watchdog::ProcessIdentity ownerProcess{};
    watchdog::RollbackPlanManifest manifest;
    std::optional<recovery::RecoveryProcessAttachmentIdentity> attachment;

    bool operator==(const RuntimeResetRegistration&) const = default;
};

enum class RuntimeRegistrationReadStatus : std::uint16_t {
    Success = 1,
    Missing = 2,
    TooLarge = 3,
    Corrupt = 4,
    Failed = 5
};

struct RuntimeRegistrationReadResult {
    RuntimeRegistrationReadStatus status{RuntimeRegistrationReadStatus::Failed};
    std::optional<RuntimeResetRegistration> registration;
    std::uint32_t systemError{0};
    std::string diagnostic;
};

bool validateRuntimeResetRegistration(
    const RuntimeResetRegistration& registration,
    std::string* error = nullptr);
std::vector<std::byte> encodeRuntimeResetRegistration(
    const RuntimeResetRegistration& registration);
std::optional<RuntimeResetRegistration> decodeRuntimeResetRegistration(
    std::span<const std::byte> bytes,
    std::string* error = nullptr);

// Single-writer control-plane storage. Writes are temp+atomic-replace and are
// durable on Windows. The registration never contains arbitrary paths/commands.
class RuntimeResetRegistrationStore {
public:
    explicit RuntimeResetRegistrationStore(std::filesystem::path rootDirectory)
        : m_rootDirectory(std::move(rootDirectory)) {}

    bool write(const RuntimeResetRegistration& registration,
               std::string* error = nullptr);
    RuntimeRegistrationReadResult load() const;
    bool remove(std::string* error = nullptr);

    const std::filesystem::path& rootDirectory() const noexcept {
        return m_rootDirectory;
    }

private:
    std::filesystem::path currentPath() const;
    std::filesystem::path temporaryPath() const;
    bool ensureRoot(std::uint32_t* systemError) const;

    std::filesystem::path m_rootDirectory;
};

enum class ResetState : std::uint16_t {
    Clean = 1,
    Recoverable = 2,
    RecoveryRequired = 3
};

struct ResetInspection {
    ResetState state{ResetState::RecoveryRequired};
    std::optional<recovery::CrashJournalState> journal;
    std::optional<recovery::SafeModeMarker> safeMode;
    std::optional<RuntimeResetRegistration> registration;
    std::string diagnostic;
};

ResetInspection inspectResetState(
    recovery::CrashJournalStore& journalStore,
    RuntimeResetRegistrationStore& registrationStore);

struct ResetExecutionReport {
    bool success{false};
    bool noOp{false};
    bool ownerSatisfied{false};
    bool rollbackSatisfied{false};
    bool journalClean{false};
    bool safeModeCleared{false};
    bool registrationCleared{false};
    std::optional<watchdog::RollbackActionOutcome> ownerOutcome;
    watchdog::RollbackExecutionSummary rollback;
    ResetInspection before;
    ResetInspection after;
    std::string diagnostic;
};

// Executes only a journal-correlated registration. When expectedSession is set,
// a different session fails closed. The runtime owner is stopped by exact PID +
// creation time before the registered rollback actions are retried idempotently.
ResetExecutionReport executeVerifiedReset(
    recovery::CrashJournalStore& journalStore,
    RuntimeResetRegistrationStore& registrationStore,
    watchdog::RollbackExecutor& executor,
    std::optional<watchdog::SessionId> expectedSession = std::nullopt);

bool enableManualSafeMode(
    recovery::CrashJournalStore& journalStore,
    std::string* error = nullptr);

// Manual zero-identity safe mode may be removed when no runtime journal exists.
// Any session-bound marker requires a verified clean journal identity.
bool disableManualSafeMode(
    recovery::CrashJournalStorage& storage,
    recovery::CrashJournalStore& journalStore,
    std::string* error = nullptr);

std::string sessionIdHex(const watchdog::SessionId& sessionId);
std::optional<watchdog::SessionId> parseSessionIdHex(
    std::string_view text,
    std::string* error = nullptr);
std::string_view resetStateName(ResetState state) noexcept;
std::string_view runtimeRegistrationReadStatusName(
    RuntimeRegistrationReadStatus status) noexcept;

} // namespace hydra::reset
