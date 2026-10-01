#pragma once

#include "hydra/runtime_state.hpp"
#include "hydra/watchdog_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::recovery {

inline constexpr std::uint32_t kRecoveryProcessAttachmentMagic = 0x31415052u; // "RPA1".
inline constexpr std::uint16_t kRecoveryProcessAttachmentVersion = 1u;
inline constexpr std::size_t kRecoveryProcessAttachmentIdentityBytes = 68u;
inline constexpr std::size_t kMaximumRecoveryProcessAttachments = 2u;

// Exact production recovery ownership. executable paths and process names are
// intentionally absent: the only process authority is PID + creation time, and
// it is additionally scoped to one Seat and one host/Seat-game activation epoch.
struct RecoveryProcessAttachmentIdentity {
    std::uint16_t schemaVersion{kRecoveryProcessAttachmentVersion};
    SeatId seatId{0};
    runtime::RuntimeSessionId hostSessionId{};
    std::uint64_t sessionGeneration{0};
    std::uint64_t seatGameGeneration{0};
    watchdog::ProcessIdentity process{};
    std::uint64_t recoveryEpoch{0};

    bool operator==(const RecoveryProcessAttachmentIdentity&) const = default;
};

struct RecoveryProcessAttachmentRegistration {
    RecoveryProcessAttachmentIdentity identity;
    watchdog::RollbackPlanManifest manifest;

    bool operator==(const RecoveryProcessAttachmentRegistration&) const = default;
};

enum class RecoveryAttachmentCode : std::uint8_t {
    Ok = 0,
    AlreadySatisfied = 1,
    InvalidIdentity = 2,
    InvalidPlan = 3,
    SeatLimitExceeded = 4,
    SeatMismatch = 5,
    SessionMismatch = 6,
    StaleSessionGeneration = 7,
    SessionGenerationMismatch = 8,
    StaleSeatGameGeneration = 9,
    SeatGameGenerationMismatch = 10,
    ProcessIdentityMismatch = 11,
    LeaseMismatch = 12,
    ConflictingRegistration = 13,
    ReplayRejected = 14,
    NotArmed = 15,
};

struct RecoveryAttachmentResult {
    RecoveryAttachmentCode code{RecoveryAttachmentCode::InvalidIdentity};
    std::optional<RecoveryProcessAttachmentRegistration> current;
    std::string diagnostic;

    bool succeeded() const noexcept {
        return code == RecoveryAttachmentCode::Ok ||
               code == RecoveryAttachmentCode::AlreadySatisfied;
    }
};

bool validateRecoveryProcessAttachmentIdentity(
    const RecoveryProcessAttachmentIdentity& identity,
    std::string* error = nullptr);
std::vector<std::byte> encodeRecoveryProcessAttachmentIdentity(
    const RecoveryProcessAttachmentIdentity& identity);
std::optional<RecoveryProcessAttachmentIdentity>
decodeRecoveryProcessAttachmentIdentity(
    std::span<const std::byte> bytes,
    std::string* error = nullptr);
bool validateRecoveryProcessAttachmentRegistration(
    const RecoveryProcessAttachmentRegistration& registration,
    std::string* error = nullptr);

// Serialized host control-path authority. It does not launch a watchdog and does
// not execute rollback. Instead it prevents a bridge from registering/disarming
// a bounded watchdog manifest for the wrong Seat/session/process epoch. At most
// two v1 Seat attachments may be armed independently at once.
class RecoveryProcessAttachmentAuthority {
public:
    RecoveryAttachmentResult registerAttachment(
        RecoveryProcessAttachmentRegistration registration);
    RecoveryAttachmentResult verifyArmed(
        const RecoveryProcessAttachmentIdentity& identity,
        const watchdog::WatchdogLease& lease) const;
    RecoveryAttachmentResult disarm(
        const RecoveryProcessAttachmentIdentity& identity,
        const watchdog::WatchdogLease& lease);

    std::vector<RecoveryProcessAttachmentRegistration> activeAttachments() const;

private:
    RecoveryAttachmentResult compareAgainstCurrent(
        const RecoveryProcessAttachmentRegistration& current,
        const RecoveryProcessAttachmentIdentity& identity,
        const watchdog::WatchdogLease& lease) const;

    std::vector<RecoveryProcessAttachmentRegistration> active_;
    std::vector<RecoveryProcessAttachmentRegistration> lastDisarmed_;
};

std::string_view recoveryAttachmentCodeName(RecoveryAttachmentCode code) noexcept;

} // namespace hydra::recovery
