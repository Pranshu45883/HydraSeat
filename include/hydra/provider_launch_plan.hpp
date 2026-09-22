#pragma once

#include "hydra/profile_schema.hpp"
#include "hydra/provider_adapter.hpp"
#include "hydra/two_seat_launch.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hydra::plan {

inline constexpr std::uint32_t kProviderLaunchPlanSchemaVersion = 1u;

struct GameRuntimeRequirement {
    std::string gameId;
    std::uint64_t revision{0};
    std::uint8_t validatedSeatCount{0};
    launch::Requirements requirements;
    launch::Capabilities capabilities;
    bool highRiskApproved{false};
    std::optional<profile::CompatibilityReference> compatibility;

    bool operator==(const GameRuntimeRequirement&) const = default;
};

struct ProviderAdapterBinding {
    std::string providerId;
    provider::LauncherProviderAdapter* adapter{nullptr};
    // Empty binds one provider-wide adapter. A value scopes the adapter to one
    // exact application, allowing multiple independent manual EXE definitions.
    std::optional<std::string> providerAppId;

    ProviderAdapterBinding() = default;
    ProviderAdapterBinding(
        std::string provider,
        provider::LauncherProviderAdapter* providerAdapter,
        std::optional<std::string> application = std::nullopt)
        : providerId(std::move(provider)),
          adapter(providerAdapter),
          providerAppId(std::move(application)) {}
};

struct SeatProviderLaunchPlan {
    SeatId seatId{0};
    std::string playerId;
    std::string gameId;
    std::optional<std::string> setupId;
    std::uint32_t instanceIndex{0};
    std::uint64_t requirementRevision{0};
    std::optional<profile::CompatibilityReference> compatibility;
    std::optional<profile::InstanceRecipe> instanceRecipe;
    std::uint64_t hardwareFingerprint{0};
    launch::Requirements requirements;
    launch::Capabilities capabilities;
    provider::ProviderLaunchRequest launchRequest;

    bool operator==(const SeatProviderLaunchPlan&) const = default;
};

struct ProviderAwareLaunchPlan {
    std::uint32_t schemaVersion{kProviderLaunchPlanSchemaVersion};
    std::uint64_t fingerprint{0};
    std::vector<SeatProviderLaunchPlan> seats;

    bool operator==(const ProviderAwareLaunchPlan&) const = default;
};

enum class PlanIssueCode : std::uint8_t {
    InvalidSeatDocument = 0,
    InvalidPlayerDocument = 1,
    InvalidGameDocument = 2,
    InvalidSetupDocument = 3,
    InvalidRuntimeSelection = 4,
    ActiveSeatCount = 5,
    MissingSeat = 6,
    InactiveSeat = 7,
    MissingPlayer = 8,
    MissingGame = 9,
    MissingRequirement = 10,
    DuplicateRequirement = 11,
    StaleCompatibility = 12,
    MissingProvider = 13,
    DuplicateProvider = 14,
    ProviderUnavailable = 15,
    ProviderLaunchRejected = 16,
    AmbiguousAccountReference = 17,
    MissingTwoPlayerSetup = 18,
    InvalidTwoPlayerSetup = 19,
    MissingDisplay = 20,
    MissingKeyboard = 21,
    MissingMouse = 22,
    MissingController = 23,
    MissingAudioOutput = 24,
    MissingCapability = 25,
    HighRiskApprovalRequired = 26,
    DuplicateExclusiveHardware = 27,
    ValidationSeatScopeExceeded = 28,
};

struct PlanIssue {
    PlanIssueCode code{PlanIssueCode::InvalidRuntimeSelection};
    SeatId seatId{0};
    std::string detail;

    bool operator==(const PlanIssue&) const = default;
};

struct PlanCompileResult {
    std::optional<ProviderAwareLaunchPlan> plan;
    std::vector<PlanIssue> issues;

    bool succeeded() const noexcept { return plan.has_value(); }
};

// Pure compiler: adapters may read only their already-refreshed in-memory snapshots.
// No process launch, filesystem mutation, registry mutation, provider authentication,
// or recovery/device mutation is performed here.
PlanCompileResult compileProviderAwareLaunchPlan(
    const profile::SeatConfigDocument& seats,
    const profile::PlayerProfileDocument& players,
    const profile::GameRecordDocument& games,
    const profile::TwoPlayerSetupDocument& setups,
    const profile::RuntimeSessionSelection& selection,
    std::span<const ProviderAdapterBinding> providers,
    std::span<const GameRuntimeRequirement> requirements);

// Recomputes the immutable compiler fingerprint from canonical plan contents.
// Runtime-side consumers use this instead of trusting a caller-provided number.
std::uint64_t recomputeProviderAwareLaunchPlanFingerprint(
    const ProviderAwareLaunchPlan& plan) noexcept;

std::string_view planIssueCodeName(PlanIssueCode code) noexcept;

} // namespace hydra::plan
