#pragma once

#include "hydra/profile_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::profile {

inline constexpr std::uint32_t kProfileMigrationReportVersion = 1u;
inline constexpr std::uint32_t kLegacyWorkspaceSchemaVersion = 2u;
inline constexpr std::size_t kMaximumProfileMigrationDiagnostics = 256u;
inline constexpr std::size_t kMaximumProfileMigrationDiagnosticValueBytes = 16u * 1024u;
inline constexpr std::size_t kMaximumProfileMigrationReportBytes = 512u * 1024u;

inline constexpr std::string_view kMigratedSeatConfigFileName = "seat_config.v1.json";
inline constexpr std::string_view kMigratedPlayerProfilesFileName = "players.v1.json";
inline constexpr std::string_view kMigratedGameRecordsFileName = "games.v1.json";
inline constexpr std::string_view kMigratedTwoPlayerSetupsFileName =
    "two_player_setups.v1.json";
inline constexpr std::string_view kProfileMigrationReportFileName =
    "migration_report.v1.json";
inline constexpr std::string_view kLegacyWorkspaceBackupFileName =
    "workspace_config.legacy-v2.backup.json";

enum class ProfileMigrationResult : std::uint8_t {
    Success = 0,
    InvalidArgument = 1,
    SourceReadError = 2,
    SourceTooLarge = 3,
    LegacyParseError = 4,
    LegacyValidationError = 5,
    ReportBoundsExceeded = 6,
    DestinationConflict = 7,
    StagingError = 8,
    WriteError = 9,
    StagedValidationError = 10,
    CommitError = 11,
    RollbackError = 12,
    CommittedValidationError = 13,
};

enum class ProfileMigrationDiagnosticKind : std::uint8_t {
    DroppedRuntimeField = 0,
    UnmappedLegacyField = 1,
    LegacyShareableResource = 2,
    AppliedLegacyDefault = 3,
};

// Deterministic fault points are exposed only so packet tests can prove that
// every write/validation/commit failure preserves the source and restores any
// previously committed v1 bundle. Production callers leave this as None.
enum class ProfileMigrationTestFault : std::uint8_t {
    None = 0,
    BeforeBackupWrite = 1,
    BeforeSeatDocumentWrite = 2,
    CorruptSeatDocumentBeforeStagedValidation = 3,
    AfterPreviousDestinationMoved = 4,
    BeforeCommittedValidation = 5,
};

struct ProfileMigrationDiagnostic {
    ProfileMigrationDiagnosticKind kind{ProfileMigrationDiagnosticKind::UnmappedLegacyField};
    std::string jsonPointer;
    // Canonical bounded JSON for the unmapped legacy value. The exact complete
    // source also remains available in the byte-preserving backup.
    std::string valueJson;
    std::string note;

    bool operator==(const ProfileMigrationDiagnostic&) const = default;
};

struct ProfileMigrationReport {
    std::uint32_t reportVersion{kProfileMigrationReportVersion};
    std::uint32_t legacySchemaVersion{kLegacyWorkspaceSchemaVersion};
    std::uint32_t targetSchemaVersion{kProfileSchemaVersion};
    std::uint64_t sourceByteCount{0};
    // Non-cryptographic deterministic fingerprint used only for diagnostics.
    // Backup validation compares the complete source bytes directly.
    std::uint64_t sourceFnv1a64{0};
    std::uint32_t migratedSeatCount{0};
    std::vector<ProfileMigrationDiagnostic> diagnostics;

    bool operator==(const ProfileMigrationReport&) const = default;
};

struct ProfileMigrationBundle {
    SeatConfigDocument seats;
    PlayerProfileDocument players;
    GameRecordDocument games;
    TwoPlayerSetupDocument setups;
    ProfileMigrationReport report;

    bool operator==(const ProfileMigrationBundle&) const = default;
};

struct ProfileMigrationOptions {
    // Replacing an existing v1 bundle is opt-in. When enabled, the old bundle
    // is moved aside and restored on every commit/post-commit validation error.
    bool replaceExisting{false};
    ProfileMigrationTestFault testingFault{ProfileMigrationTestFault::None};
};

struct ProfileMigrationOutcome {
    ProfileMigrationResult result{ProfileMigrationResult::Success};
    std::string message;
    ProfileMigrationReport report;

    bool succeeded() const noexcept { return result == ProfileMigrationResult::Success; }
};

// Pure read-only planning. On failure, `bundle` is unchanged. Legacy runtime
// HWND identity is deliberately discarded from the v1 Seat document and
// retained only as a bounded report entry plus the eventual exact backup.
ProfileMigrationOutcome planLegacyWorkspaceMigration(
    std::string_view legacyWorkspaceJson,
    ProfileMigrationBundle& bundle);

// Transactional disk migration. The source file is opened read-only and is
// never renamed, truncated, or rewritten. Files are written below a sibling
// staging directory, decoded/validated there, then committed by directory
// rename. Callers must not concurrently migrate to the same destination.
ProfileMigrationOutcome migrateLegacyWorkspaceProfile(
    const std::filesystem::path& legacyWorkspaceFile,
    const std::filesystem::path& destinationDirectory,
    const ProfileMigrationOptions& options = {});

std::string encodeProfileMigrationReport(const ProfileMigrationReport& report,
                                         std::string* error = nullptr);

std::string_view profileMigrationResultName(ProfileMigrationResult result) noexcept;
std::string_view profileMigrationDiagnosticKindName(
    ProfileMigrationDiagnosticKind kind) noexcept;

} // namespace hydra::profile
