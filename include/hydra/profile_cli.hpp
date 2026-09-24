#pragma once

#include "hydra/profile_schema.hpp"
#include "hydra/provider_launch_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::cli {

inline constexpr std::uint32_t kPlanSnapshotVersion = 1u;
inline constexpr std::size_t kMaximumPlanSnapshotBytes = 4u * 1024u * 1024u;

enum class OutputFormat : std::uint8_t {
    Human = 0,
    Json = 1,
};

enum class CliResult : std::uint8_t {
    Success = 0,
    InvalidInput = 1,
    UnsupportedVersion = 2,
    BoundsExceeded = 3,
    ParseError = 4,
};

struct CliDiagnostic {
    CliResult result{CliResult::Success};
    std::string message;

    bool succeeded() const noexcept { return result == CliResult::Success; }
};

struct PlanSeatSnapshot {
    SeatId seatId{0};
    std::string playerId;
    std::string gameId;
    std::optional<std::string> setupId;
    std::uint32_t instanceIndex{0};
    std::uint64_t requirementRevision{0};
    std::uint64_t hardwareFingerprint{0};
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::uint64_t providerMetadataRevision{0};
    provider::LaunchTargetKind targetKind{provider::LaunchTargetKind::Executable};
    std::wstring target;
    std::vector<std::wstring> arguments;
    std::optional<std::wstring> workingDirectory;
    bool accountReferenceSelected{false};
    bool protectedOrExperimental{false};

    bool operator==(const PlanSeatSnapshot&) const = default;
};

struct PlanSnapshot {
    std::uint32_t version{kPlanSnapshotVersion};
    std::uint64_t fingerprint{0};
    std::vector<PlanSeatSnapshot> seats;

    bool operator==(const PlanSnapshot&) const = default;
};

// Diagnostic snapshot deliberately records only whether an account reference was
// selected; its opaque value is never copied into CLI/export state.
CliDiagnostic makePlanSnapshot(const plan::ProviderAwareLaunchPlan& plan,
                               PlanSnapshot& output);
CliDiagnostic encodePlanSnapshot(const PlanSnapshot& snapshot, std::string& output);
CliDiagnostic decodePlanSnapshot(std::string_view bytes, PlanSnapshot& output);

CliDiagnostic renderGames(const profile::GameRecordDocument& document,
                          OutputFormat format,
                          std::string& output);
CliDiagnostic renderPlayers(const profile::PlayerProfileDocument& document,
                            OutputFormat format,
                            std::string& output);
CliDiagnostic renderSetups(const profile::TwoPlayerSetupDocument& document,
                           OutputFormat format,
                           std::string& output);
CliDiagnostic renderPlan(const PlanSnapshot& snapshot,
                         OutputFormat format,
                         std::string& output);

std::string_view cliResultName(CliResult result) noexcept;
std::string_view outputFormatName(OutputFormat format) noexcept;

} // namespace hydra::cli
