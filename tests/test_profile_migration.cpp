#include "hydra/profile_migration.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::profile;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path child(const std::filesystem::path& directory,
                            std::string_view name) {
    return directory / std::filesystem::path(std::string(name));
}

bool writeBytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::map<std::string, std::string> snapshotDirectory(
    const std::filesystem::path& directory) {
    std::map<std::string, std::string> snapshot;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         iterator != end && !error; iterator.increment(error)) {
        std::error_code typeError;
        if (iterator->is_regular_file(typeError) && !typeError) {
            snapshot.emplace(iterator->path().filename().string(),
                             readBytes(iterator->path()));
        }
    }
    return snapshot;
}

std::string seatJson(SeatId id,
                     std::string_view name,
                     std::uint64_t targetHwnd,
                     std::string_view display,
                     std::string_view keyboard,
                     std::string_view mouse,
                     std::string_view controller,
                     bool addUnknownField = false) {
    std::ostringstream output;
    output << "{\"id\":" << id
           << ",\"name\":\"" << name << "\""
           << ",\"active\":true"
           << ",\"target_hwnd\":" << targetHwnd
           << ",\"displays\":[\"" << display << "\"]"
           << ",\"primary_display\":\"" << display << "\""
           << ",\"keyboards\":[\"" << keyboard << "\"]"
           << ",\"mice\":[\"" << mouse << "\"]"
           << ",\"controllers\":[\"" << controller << "\"]"
           << ",\"audio_output\":null"
           << ",\"audio_input\":null";
    if (addUnknownField) {
        output << ",\"legacy_game_binding\":{\"title\":\"old\",\"slot\":2}";
    }
    output << '}';
    return output.str();
}

std::string workspaceJson(const std::vector<std::string>& seats,
                          bool includeShareable = true,
                          bool includeManagement = true,
                          std::string_view unknownRootJson =
                              "{\"keep\":[1,true,\"x\"]}") {
    std::ostringstream output;
    output << "{\"schema_version\":2";
    if (includeManagement) output << ",\"management_seat_id\":1";
    if (includeShareable) {
        output << ",\"shareable_resources\":[{\"type\":\"controller\","
                  "\"id\":\"PAD-SHARED\"}]";
    } else {
        output << ",\"shareable_resources\":[]";
    }
    if (!unknownRootJson.empty()) {
        output << ",\"legacy_meta\":" << unknownRootJson;
    }
    output << ",\"seats\":[";
    for (std::size_t index = 0u; index < seats.size(); ++index) {
        if (index != 0u) output << ',';
        output << seats[index];
    }
    output << "]}";
    return output.str();
}

std::string validLegacy(std::string_view seatOneName = "Seat 1",
                        bool includeManagement = true,
                        bool includeUnknown = true) {
    const auto unknown = includeUnknown
                             ? std::string_view("{\"keep\":[1,true,\"x\"]}")
                             : std::string_view{};
    return workspaceJson(
        {
            seatJson(2u, "Seat 2 \\uD50C\\uB808\\uC774\\uC5B4", 305419896u,
                     "DISPLAY-2", "KEYBOARD-2", "MOUSE-2", "pad-shared", true),
            seatJson(1u, seatOneName, 0u, "DISPLAY-1", "KEYBOARD-1", "MOUSE-1",
                     "PAD-SHARED"),
        },
        true, includeManagement, unknown);
}

bool hasDiagnostic(const ProfileMigrationReport& report,
                   ProfileMigrationDiagnosticKind kind,
                   std::string_view path) {
    for (const auto& item : report.diagnostics) {
        if (item.kind == kind && item.jsonPointer == path) return true;
    }
    return false;
}

struct TemporaryRoot {
    TemporaryRoot() {
        path = std::filesystem::temp_directory_path() /
               "hydraseat-p6-mig01-profile-migration-tests";
        std::error_code error;
        (void)std::filesystem::remove_all(path, error);
        error.clear();
        (void)std::filesystem::create_directories(path, error);
        ready = !error;
    }

    ~TemporaryRoot() {
        std::error_code error;
        (void)std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
    bool ready{false};
};

void testReadOnlyPlanIsDeterministicAndSeparated() {
    const auto legacy = validLegacy();
    ProfileMigrationBundle first;
    ProfileMigrationBundle second;
    const auto firstResult = planLegacyWorkspaceMigration(legacy, first);
    const auto secondResult = planLegacyWorkspaceMigration(legacy, second);

    check(firstResult.succeeded() && secondResult.succeeded(),
          "valid legacy workspace produces a migration plan");
    check(first == second,
          "identical legacy bytes produce an identical separated migration bundle");
    check(first.seats.seats.size() == 2u &&
              first.seats.seats[0].seatId == 1u &&
              first.seats.seats[1].seatId == 2u,
          "migrated Seats are ordered deterministically by stable Seat ID");
    check(first.seats.managementSeatId == 1u,
          "explicit legacy Management Seat survives migration");
    check(first.players.players.empty() && first.games.games.empty() &&
              first.setups.setups.empty(),
          "legacy hardware migration creates separate empty Player/Game/setup stores");
    check(first.report.sourceByteCount == legacy.size() &&
              first.report.migratedSeatCount == 2u,
          "report records bounded source size and migrated Seat count");
    check(hasDiagnostic(first.report,
                        ProfileMigrationDiagnosticKind::DroppedRuntimeField,
                        "/seats/0/target_hwnd") &&
              hasDiagnostic(first.report,
                            ProfileMigrationDiagnosticKind::DroppedRuntimeField,
                            "/seats/1/target_hwnd"),
          "every legacy target_hwnd is explicitly reported as dropped runtime state");
    check(hasDiagnostic(first.report,
                        ProfileMigrationDiagnosticKind::LegacyShareableResource,
                        "/shareable_resources/0"),
          "legacy shareability is preserved in the bounded report rather than guessed into v1");
    check(hasDiagnostic(first.report,
                        ProfileMigrationDiagnosticKind::UnmappedLegacyField,
                        "/legacy_meta") &&
              hasDiagnostic(first.report,
                            ProfileMigrationDiagnosticKind::UnmappedLegacyField,
                            "/seats/0/legacy_game_binding"),
          "unknown root and Seat data are retained as deterministic diagnostics");

    const auto seatJsonBytes = encodeSeatConfigDocument(first.seats);
    check(!seatJsonBytes.empty() &&
              seatJsonBytes.find("target_hwnd") == std::string::npos &&
              seatJsonBytes.find("legacy_game_binding") == std::string::npos,
          "stable v1 Seat output contains no runtime HWND or guessed legacy game binding");
    check(encodeProfileMigrationReport(first.report) ==
              encodeProfileMigrationReport(second.report),
          "migration report encoding is deterministic");

    ProfileMigrationBundle defaulted;
    const auto defaultResult =
        planLegacyWorkspaceMigration(validLegacy("Seat 1", false), defaulted);
    check(defaultResult.succeeded() && defaulted.seats.managementSeatId == 1u &&
              hasDiagnostic(defaulted.report,
                            ProfileMigrationDiagnosticKind::AppliedLegacyDefault,
                            "/management_seat_id"),
          "missing legacy Management Seat uses and reports the deterministic Seat 1 default");
}

void testMalformedAndUnsafeLegacyStateFailsWithoutReplacingPlan() {
    ProfileMigrationBundle bundle;
    check(planLegacyWorkspaceMigration(validLegacy(), bundle).succeeded(),
          "sentinel valid migration plan is available");
    const auto sentinel = bundle;

    const auto malformed = planLegacyWorkspaceMigration("{not-json", bundle);
    check(malformed.result == ProfileMigrationResult::LegacyParseError &&
              bundle == sentinel,
          "malformed legacy JSON fails transactionally without replacing a valid plan");

    auto future = validLegacy();
    const auto versionPosition = future.find("\"schema_version\":2");
    if (versionPosition != std::string::npos) {
        future.replace(versionPosition, std::string("\"schema_version\":2").size(),
                       "\"schema_version\":3");
    }
    const auto unsupported = planLegacyWorkspaceMigration(future, bundle);
    check(unsupported.result == ProfileMigrationResult::LegacyValidationError &&
              bundle == sentinel,
          "unsupported legacy schema version fails without replacing destination state");

    const auto unshared = workspaceJson(
        {
            seatJson(1u, "Seat 1", 0u, "DISPLAY-1", "KEYBOARD-1", "MOUSE-1",
                     "PAD-SHARED"),
            seatJson(2u, "Seat 2", 0u, "DISPLAY-2", "KEYBOARD-2", "MOUSE-2",
                     "pad-shared"),
        },
        false, true, {});
    const auto unsharedResult = planLegacyWorkspaceMigration(unshared, bundle);
    check(unsharedResult.result == ProfileMigrationResult::LegacyValidationError &&
              bundle == sentinel,
          "cross-Seat duplicate resource without legacy shareable declaration fails closed");

    const auto threeSeats = workspaceJson(
        {
            seatJson(1u, "Seat 1", 0u, "D1", "K1", "M1", "P1"),
            seatJson(2u, "Seat 2", 0u, "D2", "K2", "M2", "P2"),
            seatJson(3u, "Seat 3", 0u, "D3", "K3", "M3", "P3"),
        },
        false, true, {});
    const auto thirdSeatResult = planLegacyWorkspaceMigration(threeSeats, bundle);
    check(thirdSeatResult.result == ProfileMigrationResult::LegacyValidationError &&
              bundle == sentinel,
          "legacy profile with a third Seat cannot widen the v1 product contract");

    const std::string oversizedValue(kMaximumProfileMigrationDiagnosticValueBytes + 1u,
                                     'x');
    const std::string oversizedUnknownJson = "\"" + oversizedValue + "\"";
    const auto oversizedUnknown = workspaceJson(
        {seatJson(1u, "Seat 1", 0u, "D1", "K1", "M1", "P1")},
        false, true, oversizedUnknownJson);
    const auto oversizedResult = planLegacyWorkspaceMigration(oversizedUnknown, bundle);
    check(oversizedResult.result == ProfileMigrationResult::ReportBoundsExceeded &&
              bundle == sentinel,
          "unmapped legacy data exceeding report bounds fails instead of truncating or guessing");
}

void testSuccessfulDiskMigrationPreservesExactSourceAndValidatesBundle() {
    TemporaryRoot temporary;
    check(temporary.ready, "temporary migration test root is available");
    if (!temporary.ready) return;

    const auto source = temporary.path / "workspace_config.json";
    const auto destination = temporary.path / "profile-v1";
    const auto secondDestination = temporary.path / "profile-v1-second";
    const auto legacy = validLegacy();
    check(writeBytes(source, legacy), "legacy source fixture is written");

    const auto migrated = migrateLegacyWorkspaceProfile(source, destination);
    check(migrated.succeeded(), "valid legacy profile commits a v1 bundle");
    check(readBytes(source) == legacy,
          "successful migration never changes the original legacy source bytes");
    check(readBytes(child(destination, kLegacyWorkspaceBackupFileName)) == legacy,
          "committed bundle contains an exact byte-preserving legacy backup");

    const auto firstSnapshot = snapshotDirectory(destination);
    check(firstSnapshot.size() == 6u,
          "committed migration bundle contains exactly the six declared files");
    check(firstSnapshot.contains(std::string(kMigratedSeatConfigFileName)) &&
              firstSnapshot.contains(std::string(kMigratedPlayerProfilesFileName)) &&
              firstSnapshot.contains(std::string(kMigratedGameRecordsFileName)) &&
              firstSnapshot.contains(std::string(kMigratedTwoPlayerSetupsFileName)) &&
              firstSnapshot.contains(std::string(kProfileMigrationReportFileName)) &&
              firstSnapshot.contains(std::string(kLegacyWorkspaceBackupFileName)),
          "all separated stores, report, and backup are committed together");

    SeatConfigDocument decodedSeats;
    const auto decoded = decodeSeatConfigDocument(
        readBytes(child(destination, kMigratedSeatConfigFileName)), decodedSeats);
    check(decoded.succeeded() && decodedSeats.seats.size() == 2u &&
              decodedSeats.seats[0].seatId == 1u &&
              decodedSeats.seats[1].seatId == 2u,
          "committed Seat document decodes and preserves stable ordered hardware state");
    check(readBytes(child(destination, kMigratedSeatConfigFileName))
                  .find("target_hwnd") == std::string::npos,
          "committed stable Seat document excludes transient target_hwnd");

    const auto second = migrateLegacyWorkspaceProfile(source, secondDestination);
    check(second.succeeded() &&
              snapshotDirectory(secondDestination) == firstSnapshot,
          "same legacy bytes produce byte-identical migration bundles in another destination");

    const auto conflict = migrateLegacyWorkspaceProfile(source, destination);
    check(conflict.result == ProfileMigrationResult::DestinationConflict &&
              snapshotDirectory(destination) == firstSnapshot &&
              readBytes(source) == legacy,
          "existing v1 destination is fail-closed unless replacement is explicitly authorized");
}

void testInjectedFailuresRestorePreviousBundleAndSourceBytes() {
    TemporaryRoot temporary;
    check(temporary.ready, "temporary rollback test root is available");
    if (!temporary.ready) return;

    const auto source = temporary.path / "workspace_config.json";
    const auto destination = temporary.path / "profile-v1";
    const auto originalLegacy = validLegacy("Seat 1");
    check(writeBytes(source, originalLegacy), "baseline legacy fixture is written");
    check(migrateLegacyWorkspaceProfile(source, destination).succeeded(),
          "baseline v1 bundle is committed before replacement failure tests");
    const auto previousBundle = snapshotDirectory(destination);

    const auto replacementLegacy = validLegacy("Seat One Changed");
    check(writeBytes(source, replacementLegacy), "replacement legacy fixture is written");

    const std::vector<std::pair<ProfileMigrationTestFault, ProfileMigrationResult>> faults{
        {ProfileMigrationTestFault::BeforeSeatDocumentWrite,
         ProfileMigrationResult::WriteError},
        {ProfileMigrationTestFault::CorruptSeatDocumentBeforeStagedValidation,
         ProfileMigrationResult::StagedValidationError},
        {ProfileMigrationTestFault::AfterPreviousDestinationMoved,
         ProfileMigrationResult::CommitError},
        {ProfileMigrationTestFault::BeforeCommittedValidation,
         ProfileMigrationResult::CommittedValidationError},
    };

    for (const auto& [fault, expectedResult] : faults) {
        ProfileMigrationOptions options;
        options.replaceExisting = true;
        options.testingFault = fault;
        const auto result =
            migrateLegacyWorkspaceProfile(source, destination, options);
        check(result.result == expectedResult,
              "injected migration failure returns its declared fail-closed result");
        check(readBytes(source) == replacementLegacy,
              "injected migration failure leaves original source byte-for-byte unchanged");
        check(snapshotDirectory(destination) == previousBundle,
              "injected migration failure restores the previous complete v1 bundle");
        check(!std::filesystem::exists(
                  std::filesystem::path(destination.string() + ".staging")) &&
                  !std::filesystem::exists(
                      std::filesystem::path(destination.string() + ".rollback")),
              "successful rollback leaves no owned staging or rollback directory behind");
    }

    ProfileMigrationOptions replacementOptions;
    replacementOptions.replaceExisting = true;
    const auto replaced =
        migrateLegacyWorkspaceProfile(source, destination, replacementOptions);
    const auto replacementBundle = snapshotDirectory(destination);
    check(replaced.succeeded() && replacementBundle != previousBundle,
          "authorized replacement commits the new deterministic v1 bundle");
    check(readBytes(source) == replacementLegacy,
          "authorized replacement still treats legacy source as read-only");
    check(!std::filesystem::exists(
              std::filesystem::path(destination.string() + ".rollback")),
          "successful replacement removes the rollback copy after post-commit validation");
}

} // namespace

int main() {
    testReadOnlyPlanIsDeterministicAndSeparated();
    testMalformedAndUnsafeLegacyStateFailsWithoutReplacingPlan();
    testSuccessfulDiskMigrationPreservesExactSourceAndValidatesBundle();
    testInjectedFailuresRestorePreviousBundleAndSourceBytes();

    if (failures != 0) {
        std::cerr << failures << " profile migration test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "P6-MIG-01 profile migration tests passed\n";
    return EXIT_SUCCESS;
}
