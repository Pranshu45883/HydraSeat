#include "hydra/provider_launch_plan.hpp"
#include "hydra/instance_materialization.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace hydra::plan {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

class StableHash final {
public:
    void byte(std::uint8_t value) noexcept {
        value_ ^= static_cast<std::uint64_t>(value);
        value_ *= kFnvPrime;
    }

    void boolean(bool value) noexcept { byte(value ? 1u : 0u); }

    void u32(std::uint32_t value) noexcept {
        for (unsigned int shift = 0; shift < 32u; shift += 8u) {
            byte(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }

    void u64(std::uint64_t value) noexcept {
        for (unsigned int shift = 0; shift < 64u; shift += 8u) {
            byte(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }

    void text(std::string_view value) noexcept {
        u64(static_cast<std::uint64_t>(value.size()));
        for (const char ch : value) {
            byte(static_cast<std::uint8_t>(static_cast<unsigned char>(ch)));
        }
    }

    void wide(std::wstring_view value) noexcept {
        u64(static_cast<std::uint64_t>(value.size()));
        for (const wchar_t ch : value) u32(static_cast<std::uint32_t>(ch));
    }

    std::uint64_t value() const noexcept { return value_; }

private:
    std::uint64_t value_{kFnvOffset};
};

PlanCompileResult failure(PlanIssueCode code, SeatId seatId, std::string detail) {
    PlanCompileResult result;
    result.issues.push_back({code, seatId, std::move(detail)});
    return result;
}

const profile::PersistedSeatConfig* findSeat(const profile::SeatConfigDocument& document,
                                             SeatId seatId) noexcept {
    for (const auto& seat : document.seats) {
        if (seat.seatId == seatId) return &seat;
    }
    return nullptr;
}

const profile::PlayerProfile* findPlayer(const profile::PlayerProfileDocument& document,
                                         std::string_view playerId) noexcept {
    for (const auto& player : document.players) {
        if (player.playerId == playerId) return &player;
    }
    return nullptr;
}

const profile::GameRecord* findGame(const profile::GameRecordDocument& document,
                                    std::string_view gameId) noexcept {
    for (const auto& game : document.games) {
        if (game.gameId == gameId) return &game;
    }
    return nullptr;
}

const profile::TwoPlayerSetup* findSetup(const profile::TwoPlayerSetupDocument& document,
                                         std::string_view setupId) noexcept {
    for (const auto& setup : document.setups) {
        if (setup.setupId == setupId) return &setup;
    }
    return nullptr;
}

const GameRuntimeRequirement* findRequirement(
    std::span<const GameRuntimeRequirement> requirements,
    std::string_view gameId,
    bool& duplicate) noexcept {
    const GameRuntimeRequirement* found = nullptr;
    duplicate = false;
    for (const auto& requirement : requirements) {
        if (requirement.gameId != gameId) continue;
        if (found != nullptr) {
            duplicate = true;
            return nullptr;
        }
        found = &requirement;
    }
    return found;
}

provider::LauncherProviderAdapter* findProvider(
    std::span<const ProviderAdapterBinding> providers,
    std::string_view providerId,
    const std::optional<std::string>& providerAppId,
    bool& duplicate) noexcept {
    provider::LauncherProviderAdapter* exact = nullptr;
    bool exactMatched = false;
    provider::LauncherProviderAdapter* providerWide = nullptr;
    duplicate = false;
    for (const auto& binding : providers) {
        if (binding.providerId != providerId) continue;
        if (binding.providerAppId) {
            if (!providerAppId || *binding.providerAppId != *providerAppId) continue;
            if (exactMatched) {
                duplicate = true;
                return nullptr;
            }
            exactMatched = true;
            exact = binding.adapter;
            continue;
        }
        if (providerWide != nullptr) {
            duplicate = true;
            return nullptr;
        }
        providerWide = binding.adapter;
    }
    return exactMatched ? exact : providerWide;
}

std::optional<std::string> resolveAccount(const profile::PlayerProfile& player,
                                          std::string_view providerId,
                                          bool& ambiguous) {
    std::optional<std::string> selected;
    ambiguous = false;
    for (const auto& account : player.providerAccounts) {
        if (account.providerId != providerId) continue;
        if (selected) {
            ambiguous = true;
            return std::nullopt;
        }
        selected = account.accountRef;
    }
    return selected;
}

void hashCompatibility(StableHash& hash,
                       const std::optional<profile::CompatibilityReference>& value) noexcept {
    hash.boolean(value.has_value());
    if (!value) return;
    hash.text(value->recordId);
    hash.text(value->provenance);
    hash.u32(value->evidenceRevision);
}

void hashRecipe(StableHash& hash,
                const std::optional<profile::InstanceRecipe>& recipe) noexcept {
    hash.boolean(recipe.has_value());
    if (!recipe) return;
    hash.u64(static_cast<std::uint64_t>(recipe->arguments.size()));
    for (const auto& argument : recipe->arguments) hash.wide(argument);
    hash.boolean(recipe->workingDirectory.has_value());
    if (recipe->workingDirectory) hash.wide(*recipe->workingDirectory);
    hash.boolean(recipe->dataRoot.has_value());
    if (recipe->dataRoot) hash.wide(*recipe->dataRoot);
}

void hashRequirements(StableHash& hash, const launch::Requirements& value) noexcept {
    hash.boolean(value.display);
    hash.boolean(value.keyboard);
    hash.boolean(value.mouse);
    hash.boolean(value.controller);
    hash.boolean(value.audioOutput);
    hash.boolean(value.windowOwnership);
    hash.boolean(value.recovery);
    hash.boolean(value.highRisk);
}

void hashCapabilities(StableHash& hash, const launch::Capabilities& value) noexcept {
    hash.boolean(value.process);
    hash.boolean(value.window);
    hash.boolean(value.display);
    hash.boolean(value.input);
    hash.boolean(value.controller);
    hash.boolean(value.audio);
    hash.boolean(value.recovery);
}

template <typename Range>
void hashSortedWideRange(StableHash& hash, const Range& range) {
    std::vector<std::wstring> values(range.begin(), range.end());
    std::sort(values.begin(), values.end());
    hash.u64(static_cast<std::uint64_t>(values.size()));
    for (const auto& value : values) hash.wide(value);
}

std::uint64_t hardwareFingerprint(const profile::PersistedSeatConfig& seat) {
    StableHash hash;
    hash.u32(seat.seatId);
    hashSortedWideRange(hash, seat.displayIds);
    hash.boolean(seat.primaryDisplayId.has_value());
    if (seat.primaryDisplayId) hash.wide(*seat.primaryDisplayId);
    hashSortedWideRange(hash, seat.keyboardIds);
    hashSortedWideRange(hash, seat.mouseIds);
    hashSortedWideRange(hash, seat.controllerIds);
    hash.boolean(seat.audioOutputEndpointId.has_value());
    if (seat.audioOutputEndpointId) hash.wide(*seat.audioOutputEndpointId);
    hash.boolean(seat.audioInputEndpointId.has_value());
    if (seat.audioInputEndpointId) hash.wide(*seat.audioInputEndpointId);
    return hash.value();
}

std::optional<PlanIssue> preflightSeat(const profile::PersistedSeatConfig& seat,
                                       const GameRuntimeRequirement& requirement) {
    const auto& needs = requirement.requirements;
    const auto& caps = requirement.capabilities;
    if (needs.display && seat.displayIds.empty()) {
        return PlanIssue{PlanIssueCode::MissingDisplay, seat.seatId,
                         "selected game requires a display"};
    }
    if (seat.primaryDisplayId &&
        std::find(seat.displayIds.begin(), seat.displayIds.end(), *seat.primaryDisplayId) ==
            seat.displayIds.end()) {
        return PlanIssue{PlanIssueCode::MissingDisplay, seat.seatId,
                         "primary display is outside the Seat display group"};
    }
    if (needs.keyboard && seat.keyboardIds.empty()) {
        return PlanIssue{PlanIssueCode::MissingKeyboard, seat.seatId,
                         "selected game requires a keyboard"};
    }
    if (needs.mouse && seat.mouseIds.empty()) {
        return PlanIssue{PlanIssueCode::MissingMouse, seat.seatId,
                         "selected game requires a pointing device"};
    }
    if (needs.controller && seat.controllerIds.empty()) {
        return PlanIssue{PlanIssueCode::MissingController, seat.seatId,
                         "selected game requires a controller"};
    }
    if (needs.audioOutput && !seat.audioOutputEndpointId) {
        return PlanIssue{PlanIssueCode::MissingAudioOutput, seat.seatId,
                         "selected game requires an audio output"};
    }
    if (!caps.process || (needs.windowOwnership && !caps.window) ||
        (needs.display && !caps.display) ||
        ((needs.keyboard || needs.mouse) && !caps.input) ||
        (needs.controller && !caps.controller) ||
        (needs.audioOutput && !caps.audio) || (needs.recovery && !caps.recovery)) {
        return PlanIssue{PlanIssueCode::MissingCapability, seat.seatId,
                         "required runtime capability is unavailable"};
    }
    if (needs.highRisk && !requirement.highRiskApproved) {
        return PlanIssue{PlanIssueCode::HighRiskApprovalRequired, seat.seatId,
                         "selected compatibility path requires explicit approval"};
    }
    return std::nullopt;
}

std::optional<PlanIssue> preflightExclusiveHardware(
    const std::vector<const profile::PersistedSeatConfig*>& seats) {
    std::set<std::wstring> displays;
    std::set<std::wstring> keyboards;
    std::set<std::wstring> mice;
    std::set<std::wstring> controllers;
    std::set<std::wstring> audio;
    for (const auto* seat : seats) {
        for (const auto& id : seat->displayIds) {
            if (!displays.insert(id).second) {
                return PlanIssue{PlanIssueCode::DuplicateExclusiveHardware, seat->seatId,
                                 "display identity is assigned to multiple selected Seats"};
            }
        }
        for (const auto& id : seat->keyboardIds) {
            if (!keyboards.insert(id).second) {
                return PlanIssue{PlanIssueCode::DuplicateExclusiveHardware, seat->seatId,
                                 "keyboard identity is assigned to multiple selected Seats"};
            }
        }
        for (const auto& id : seat->mouseIds) {
            if (!mice.insert(id).second) {
                return PlanIssue{PlanIssueCode::DuplicateExclusiveHardware, seat->seatId,
                                 "mouse identity is assigned to multiple selected Seats"};
            }
        }
        for (const auto& id : seat->controllerIds) {
            if (!controllers.insert(id).second) {
                return PlanIssue{PlanIssueCode::DuplicateExclusiveHardware, seat->seatId,
                                 "controller identity is assigned to multiple selected Seats"};
            }
        }
        if (seat->audioOutputEndpointId &&
            !audio.insert(*seat->audioOutputEndpointId).second) {
            return PlanIssue{PlanIssueCode::DuplicateExclusiveHardware, seat->seatId,
                             "audio output is assigned to multiple selected Seats"};
        }
    }
    return std::nullopt;
}

std::uint64_t fingerprint(const std::vector<SeatProviderLaunchPlan>& seats) noexcept {
    StableHash hash;
    hash.u32(kProviderLaunchPlanSchemaVersion);
    hash.u64(static_cast<std::uint64_t>(seats.size()));
    for (const auto& seat : seats) {
        hash.u32(seat.seatId);
        hash.text(seat.playerId);
        hash.text(seat.gameId);
        hash.boolean(seat.setupId.has_value());
        if (seat.setupId) hash.text(*seat.setupId);
        hash.u32(seat.instanceIndex);
        hash.u64(seat.requirementRevision);
        hashCompatibility(hash, seat.compatibility);
        hashRecipe(hash, seat.instanceRecipe);
        hash.u64(seat.hardwareFingerprint);
        hashRequirements(hash, seat.requirements);
        hashCapabilities(hash, seat.capabilities);
        const auto& request = seat.launchRequest;
        hash.text(request.providerId);
        hash.text(request.gameId);
        hash.boolean(request.providerAppId.has_value());
        if (request.providerAppId) hash.text(*request.providerAppId);
        hash.boolean(request.accountRef.has_value());
        if (request.accountRef) hash.text(*request.accountRef);
        hash.u64(request.metadataRevision);
        hash.u32(static_cast<std::uint32_t>(request.targetKind));
        hash.wide(request.target);
        hash.u64(static_cast<std::uint64_t>(request.arguments.size()));
        for (const auto& argument : request.arguments) hash.wide(argument);
        hash.boolean(request.workingDirectory.has_value());
        if (request.workingDirectory) hash.wide(*request.workingDirectory);
        hash.text(request.launchCorrelationId);
    }
    return hash.value();
}

PlanCompileResult schemaFailure(const profile::SchemaDiagnostic& diagnostic,
                                PlanIssueCode code,
                                std::string_view label) {
    return failure(code, 0u, std::string(label) + ": " + diagnostic.message);
}

} // namespace

PlanCompileResult compileProviderAwareLaunchPlan(
    const profile::SeatConfigDocument& seats,
    const profile::PlayerProfileDocument& players,
    const profile::GameRecordDocument& games,
    const profile::TwoPlayerSetupDocument& setups,
    const profile::RuntimeSessionSelection& selection,
    std::span<const ProviderAdapterBinding> providers,
    std::span<const GameRuntimeRequirement> requirements) {
    try {
        auto diagnostic = profile::validateSeatConfigDocument(seats);
        if (!diagnostic.succeeded()) {
            return schemaFailure(diagnostic, PlanIssueCode::InvalidSeatDocument, "Seat document");
        }
        diagnostic = profile::validatePlayerProfileDocument(players);
        if (!diagnostic.succeeded()) {
            return schemaFailure(diagnostic, PlanIssueCode::InvalidPlayerDocument,
                                 "Player document");
        }
        diagnostic = profile::validateGameRecordDocument(games);
        if (!diagnostic.succeeded()) {
            return schemaFailure(diagnostic, PlanIssueCode::InvalidGameDocument, "Game document");
        }
        diagnostic = profile::validateTwoPlayerSetupDocument(setups);
        if (!diagnostic.succeeded()) {
            return schemaFailure(diagnostic, PlanIssueCode::InvalidSetupDocument, "Setup document");
        }
        diagnostic = profile::validateRuntimeSessionSelection(selection, seats, players, games,
                                                               setups);
        if (!diagnostic.succeeded()) {
            return schemaFailure(diagnostic, PlanIssueCode::InvalidRuntimeSelection,
                                 "Runtime selection");
        }
        if (selection.bindings.empty() || selection.bindings.size() > 2u) {
            return failure(PlanIssueCode::ActiveSeatCount, 0u,
                           "provider-aware plan requires one or two selected Seats");
        }

        std::vector<profile::RuntimeBinding> bindings = selection.bindings;
        std::sort(bindings.begin(), bindings.end(), [](const auto& left, const auto& right) {
            return left.seatId < right.seatId;
        });

        const bool sameGame = bindings.size() == 2u &&
                              bindings[0].gameId == bindings[1].gameId;
        if (sameGame) {
            if (!bindings[0].setupId || !bindings[1].setupId ||
                *bindings[0].setupId != *bindings[1].setupId) {
                return failure(PlanIssueCode::MissingTwoPlayerSetup, 0u,
                               "same-game two-Seat selection requires one shared setup" );
            }
            if (bindings[0].instanceIndex == bindings[1].instanceIndex) {
                return failure(PlanIssueCode::InvalidTwoPlayerSetup, 0u,
                               "same-game Seats must select different setup instances");
            }
        } else if (bindings.size() == 2u &&
                   (bindings[0].setupId.has_value() || bindings[1].setupId.has_value())) {
            return failure(PlanIssueCode::InvalidTwoPlayerSetup, 0u,
                           "two-player setup cannot bind two different games");
        }

        std::vector<const profile::PersistedSeatConfig*> selectedSeats;
        selectedSeats.reserve(bindings.size());
        for (const auto& binding : bindings) {
            const auto* seat = findSeat(seats, binding.seatId);
            if (seat == nullptr) {
                return failure(PlanIssueCode::MissingSeat, binding.seatId,
                               "selected Seat is missing");
            }
            if (!seat->active) {
                return failure(PlanIssueCode::InactiveSeat, binding.seatId,
                               "selected Seat is inactive");
            }
            selectedSeats.push_back(seat);
        }
        if (const auto issue = preflightExclusiveHardware(selectedSeats)) {
            PlanCompileResult result;
            result.issues.push_back(*issue);
            return result;
        }

        ProviderAwareLaunchPlan compiled;
        compiled.seats.reserve(bindings.size());
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            const auto& binding = bindings[index];
            const auto* seat = selectedSeats[index];
            const auto* player = findPlayer(players, binding.playerId);
            if (player == nullptr) {
                return failure(PlanIssueCode::MissingPlayer, binding.seatId,
                               "selected Player is missing");
            }
            const auto* game = findGame(games, binding.gameId);
            if (game == nullptr) {
                return failure(PlanIssueCode::MissingGame, binding.seatId,
                               "selected Game is missing");
            }

            bool duplicateRequirement = false;
            const auto* requirement = findRequirement(requirements, game->gameId,
                                                      duplicateRequirement);
            if (duplicateRequirement) {
                return failure(PlanIssueCode::DuplicateRequirement, binding.seatId,
                               "multiple runtime requirement records match the selected Game");
            }
            if (requirement == nullptr) {
                return failure(PlanIssueCode::MissingRequirement, binding.seatId,
                               "selected Game has no exact runtime requirement record");
            }
            const auto selectedGameSeatCount = static_cast<std::uint8_t>(std::count_if(
                bindings.begin(), bindings.end(), [&](const profile::RuntimeBinding& candidate) {
                    return candidate.gameId == binding.gameId;
                }));
            if (requirement->validatedSeatCount < 1u ||
                requirement->validatedSeatCount > 2u ||
                selectedGameSeatCount > requirement->validatedSeatCount) {
                return failure(PlanIssueCode::ValidationSeatScopeExceeded, binding.seatId,
                               "selected Game exceeds its trusted physical validation Seat scope");
            }
            if (requirement->revision == 0u || requirement->compatibility != game->compatibility) {
                return failure(PlanIssueCode::StaleCompatibility, binding.seatId,
                               "runtime requirement evidence does not match the selected Game");
            }
            if (const auto issue = preflightSeat(*seat, *requirement)) {
                PlanCompileResult result;
                result.issues.push_back(*issue);
                return result;
            }

            bool duplicateProvider = false;
            auto* adapter = findProvider(providers, game->providerId,
                                         game->providerAppId, duplicateProvider);
            if (duplicateProvider) {
                return failure(PlanIssueCode::DuplicateProvider, binding.seatId,
                               "multiple provider adapters match the selected Game");
            }
            if (adapter == nullptr) {
                return failure(PlanIssueCode::MissingProvider, binding.seatId,
                               "selected Game provider is unavailable");
            }
            const auto descriptor = adapter->descriptor();
            if (descriptor.providerId != game->providerId ||
                descriptor.availability != provider::ProviderAvailability::Available) {
                return failure(PlanIssueCode::ProviderUnavailable, binding.seatId,
                               "provider snapshot is absent, offline, or mismatched");
            }

            bool ambiguousAccount = false;
            auto account = resolveAccount(*player, game->providerId, ambiguousAccount);
            if (ambiguousAccount) {
                return failure(PlanIssueCode::AmbiguousAccountReference, binding.seatId,
                               "Player has multiple account references for this provider");
            }

            std::optional<profile::InstanceRecipe> recipe;
            if (binding.setupId) {
                const auto* setup = findSetup(setups, *binding.setupId);
                if (setup == nullptr || setup->gameId != game->gameId ||
                    setup->instances.size() != 2u || binding.instanceIndex >= 2u) {
                    return failure(PlanIssueCode::InvalidTwoPlayerSetup, binding.seatId,
                                   "selected setup does not match the Game/instance" );
                }
                if (setup->compatibility && setup->compatibility != game->compatibility) {
                    return failure(PlanIssueCode::StaleCompatibility, binding.seatId,
                                   "two-player setup compatibility evidence is stale");
                }
                recipe = setup->instances[binding.instanceIndex];
            }

            provider::LaunchSelection providerSelection;
            providerSelection.providerId = game->providerId;
            providerSelection.gameId = game->gameId;
            providerSelection.providerAppId = game->providerAppId;
            providerSelection.accountRef = std::move(account);
            providerSelection.expectedMetadataRevision = descriptor.metadataRevision;
            if (recipe) providerSelection.instanceArguments = recipe->arguments;

            provider::ProviderLaunchRequest request;
            const auto providerDiagnostic =
                provider::buildLaunchRequest(*adapter, providerSelection, request);
            if (!providerDiagnostic.succeeded()) {
                return failure(PlanIssueCode::ProviderLaunchRejected, binding.seatId,
                               std::string(provider::providerResultName(providerDiagnostic.result)) +
                                   ": " + providerDiagnostic.message);
            }

            SeatProviderLaunchPlan plannedSeat;
            plannedSeat.seatId = binding.seatId;
            plannedSeat.playerId = binding.playerId;
            plannedSeat.gameId = binding.gameId;
            plannedSeat.setupId = binding.setupId;
            plannedSeat.instanceIndex = binding.instanceIndex;
            plannedSeat.requirementRevision = requirement->revision;
            plannedSeat.compatibility = requirement->compatibility;
            plannedSeat.instanceRecipe = std::move(recipe);
            plannedSeat.hardwareFingerprint = hardwareFingerprint(*seat);
            plannedSeat.requirements = requirement->requirements;
            plannedSeat.capabilities = requirement->capabilities;
            plannedSeat.launchRequest = std::move(request);
            compiled.seats.push_back(std::move(plannedSeat));
        }

        compiled.fingerprint = fingerprint(compiled.seats);
        PlanCompileResult result;
        result.plan = std::move(compiled);
        return result;
    } catch (...) {
        return failure(PlanIssueCode::InvalidRuntimeSelection, 0u,
                       "provider-aware launch plan compilation failed unexpectedly");
    }
}

std::uint64_t recomputeProviderAwareLaunchPlanFingerprint(
    const ProviderAwareLaunchPlan& plan) noexcept {
    if (plan.schemaVersion != kProviderLaunchPlanSchemaVersion) return 0u;
    try {
        auto seats = plan.seats;
        std::sort(seats.begin(), seats.end(), [](const auto& left, const auto& right) {
            return left.seatId < right.seatId;
        });
        return fingerprint(seats);
    } catch (...) {
        return 0u;
    }
}

std::string_view planIssueCodeName(PlanIssueCode code) noexcept {
    switch (code) {
    case PlanIssueCode::InvalidSeatDocument: return "InvalidSeatDocument";
    case PlanIssueCode::InvalidPlayerDocument: return "InvalidPlayerDocument";
    case PlanIssueCode::InvalidGameDocument: return "InvalidGameDocument";
    case PlanIssueCode::InvalidSetupDocument: return "InvalidSetupDocument";
    case PlanIssueCode::InvalidRuntimeSelection: return "InvalidRuntimeSelection";
    case PlanIssueCode::ActiveSeatCount: return "ActiveSeatCount";
    case PlanIssueCode::MissingSeat: return "MissingSeat";
    case PlanIssueCode::InactiveSeat: return "InactiveSeat";
    case PlanIssueCode::MissingPlayer: return "MissingPlayer";
    case PlanIssueCode::MissingGame: return "MissingGame";
    case PlanIssueCode::MissingRequirement: return "MissingRequirement";
    case PlanIssueCode::DuplicateRequirement: return "DuplicateRequirement";
    case PlanIssueCode::StaleCompatibility: return "StaleCompatibility";
    case PlanIssueCode::MissingProvider: return "MissingProvider";
    case PlanIssueCode::DuplicateProvider: return "DuplicateProvider";
    case PlanIssueCode::ProviderUnavailable: return "ProviderUnavailable";
    case PlanIssueCode::ProviderLaunchRejected: return "ProviderLaunchRejected";
    case PlanIssueCode::AmbiguousAccountReference: return "AmbiguousAccountReference";
    case PlanIssueCode::MissingTwoPlayerSetup: return "MissingTwoPlayerSetup";
    case PlanIssueCode::InvalidTwoPlayerSetup: return "InvalidTwoPlayerSetup";
    case PlanIssueCode::MissingDisplay: return "MissingDisplay";
    case PlanIssueCode::MissingKeyboard: return "MissingKeyboard";
    case PlanIssueCode::MissingMouse: return "MissingMouse";
    case PlanIssueCode::MissingController: return "MissingController";
    case PlanIssueCode::MissingAudioOutput: return "MissingAudioOutput";
    case PlanIssueCode::MissingCapability: return "MissingCapability";
    case PlanIssueCode::HighRiskApprovalRequired: return "HighRiskApprovalRequired";
    case PlanIssueCode::DuplicateExclusiveHardware: return "DuplicateExclusiveHardware";
    case PlanIssueCode::ValidationSeatScopeExceeded: return "ValidationSeatScopeExceeded";
    }
    return "Unknown";
}

} // namespace hydra::plan

namespace hydra::materialization {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kInstanceManifestName = ".hydraseat-instance-v1";
constexpr std::string_view kInstanceManifestMagic = "HYDRASEAT_INSTANCE_V1";
constexpr std::size_t kMaximumManifestBytes = 2048u;

RecipeDiagnostic failure(RecipeResult code, std::string message) {
    return {code, std::move(message)};
}

int phaseOrdinal(setup::RecipeExecutionPhase phase) noexcept {
    switch (phase) {
    case setup::RecipeExecutionPhase::PreSpawn: return 0;
    case setup::RecipeExecutionPhase::Startup: return 1;
    case setup::RecipeExecutionPhase::PostWindow: return 2;
    case setup::RecipeExecutionPhase::Runtime: return 3;
    }
    return -1;
}

bool validScope(setup::MutationScope scope) noexcept {
    return scope == setup::MutationScope::SeatWritableInstance ||
           scope == setup::MutationScope::SharedInstallation;
}

bool validOpaqueId(std::string_view value, bool allowColon = true) noexcept {
    if (value.empty() || value.size() > profile::kMaximumIdentifierBytes) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        const bool alphaNumeric = (ch >= 'a' && ch <= 'z') ||
                                  (ch >= 'A' && ch <= 'Z') ||
                                  (ch >= '0' && ch <= '9');
        if (alphaNumeric || ch == '-' || ch == '_' || ch == '.' ||
            (allowColon && ch == ':')) {
            continue;
        }
        return false;
    }
    return true;
}

bool validSessionId(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumMaterializationSessionIdBytes) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        const bool alphaNumeric = (ch >= 'a' && ch <= 'z') ||
                                  (ch >= 'A' && ch <= 'Z') ||
                                  (ch >= '0' && ch <= '9');
        if (!(alphaNumeric || ch == '-' || ch == '_' || ch == '.')) return false;
    }
    return true;
}

class MaterialHash final {
public:
    void byte(std::uint8_t value) noexcept {
        value_ ^= static_cast<std::uint64_t>(value);
        value_ *= 1099511628211ull;
    }
    void boolean(bool value) noexcept { byte(value ? 1u : 0u); }
    void u32(std::uint32_t value) noexcept {
        for (unsigned int shift = 0u; shift < 32u; shift += 8u) {
            byte(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void u64(std::uint64_t value) noexcept {
        for (unsigned int shift = 0u; shift < 64u; shift += 8u) {
            byte(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void text(std::string_view value) noexcept {
        u64(static_cast<std::uint64_t>(value.size()));
        for (const char ch : value) {
            byte(static_cast<std::uint8_t>(static_cast<unsigned char>(ch)));
        }
    }
    void wide(std::wstring_view value) noexcept {
        u64(static_cast<std::uint64_t>(value.size()));
        for (const wchar_t ch : value) u32(static_cast<std::uint32_t>(ch));
    }
    std::uint64_t value() const noexcept { return value_ == 0u ? 1u : value_; }

private:
    std::uint64_t value_{1469598103934665603ull};
};

void hashCompatibility(MaterialHash& hash,
                       const std::optional<profile::CompatibilityReference>& value) noexcept {
    hash.boolean(value.has_value());
    if (!value) return;
    hash.text(value->recordId);
    hash.text(value->provenance);
    hash.u32(value->evidenceRevision);
}

std::string hex64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

std::wstring windowsPathKey(const fs::path& path) {
    auto value = path.lexically_normal().generic_wstring();
    for (auto& ch : value) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return value;
}

bool windowsReservedPathComponent(std::wstring_view value) {
    const auto dot = value.find(L'.');
    auto base = std::wstring(value.substr(0u, dot));
    for (auto& ch : base) {
        if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
    }
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") {
        return true;
    }
    if (base.size() == 4u && base[3] >= L'1' && base[3] <= L'9') {
        return base.substr(0u, 3u) == L"COM" || base.substr(0u, 3u) == L"LPT";
    }
    return false;
}

bool validRelativeRecipePath(std::wstring_view text, fs::path& output) {
    if (text.empty() || text.size() > profile::kMaximumPathCodeUnits ||
        text.find(L'\0') != std::wstring_view::npos ||
        text.find(L'\\') != std::wstring_view::npos) {
        return false;
    }
    for (const wchar_t ch : text) {
        if (ch < 32 || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' ||
            ch == L'<' || ch == L'>' || ch == L'|') {
            return false;
        }
    }
    fs::path candidate(text);
    if (candidate.empty() || candidate.is_absolute() || candidate.has_root_name() ||
        candidate.has_root_directory()) {
        return false;
    }
    for (const auto& component : candidate) {
        const auto part = component.generic_wstring();
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' ||
            part.back() == L' ' || windowsReservedPathComponent(part)) {
            return false;
        }
    }
    candidate = candidate.lexically_normal();
    if (candidate.empty() || candidate == fs::path(L".")) return false;
    output = std::move(candidate);
    return true;
}

bool pathPrefixConflict(const fs::path& left, const fs::path& right) {
    const auto leftKey = windowsPathKey(left);
    const auto rightKey = windowsPathKey(right);
    if (leftKey == rightKey) return true;
    const auto prefix = [](std::wstring_view shorter, std::wstring_view longer) {
        return longer.size() > shorter.size() &&
               longer.substr(0u, shorter.size()) == shorter &&
               longer[shorter.size()] == L'/';
    };
    return prefix(leftKey, rightKey) || prefix(rightKey, leftKey);
}

bool pathLengthBounded(const fs::path& path) {
    try {
        return path.wstring().size() <= profile::kMaximumPathCodeUnits;
    } catch (...) {
        return false;
    }
}

bool pathReparsePoint(const fs::path& path, bool& exists, std::string& error) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            exists = false;
            return false;
        }
        error = "failed to inspect filesystem path: " + ec.message();
        return true;
    }
    exists = status.type() != fs::file_type::not_found;
    if (!exists) return false;
    if (fs::is_symlink(status)) return true;
#ifdef _WIN32
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            exists = false;
            return false;
        }
        error = "GetFileAttributesW failed while checking a materialization path";
        return true;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) return true;
#endif
    return false;
}

bool ancestorsArePlain(const fs::path& path, std::string& error) {
    if (!path.is_absolute()) {
        error = "materialization path must be absolute";
        return false;
    }
    fs::path current = path.root_path();
    bool exists = false;
    if (!current.empty() && pathReparsePoint(current, exists, error)) {
        if (error.empty()) error = "materialization root crosses a reparse point";
        return false;
    }
    for (const auto& component : path.relative_path()) {
        current /= component;
        if (pathReparsePoint(current, exists, error)) {
            if (error.empty()) error = "materialization path crosses a symlink/junction/reparse point";
            return false;
        }
    }
    return true;
}

bool ownedTreeIsPlain(const fs::path& root, std::string& error) {
    std::vector<fs::path> pending{root};
    while (!pending.empty()) {
        const auto directory = std::move(pending.back());
        pending.pop_back();
        std::error_code ec;
        fs::directory_iterator iterator(directory, ec);
        if (ec) {
            error = "failed to enumerate owned materialization tree: " + ec.message();
            return false;
        }
        for (const auto& entry : iterator) {
            bool exists = false;
            std::string reparseError;
            if (pathReparsePoint(entry.path(), exists, reparseError)) {
                error = reparseError.empty()
                            ? "owned materialization tree contains a symlink/junction/reparse point"
                            : std::move(reparseError);
                return false;
            }
            if (!exists) continue;
            const auto status = entry.symlink_status(ec);
            if (ec) {
                error = "failed to inspect owned materialization entry: " + ec.message();
                return false;
            }
            if (fs::is_directory(status)) pending.push_back(entry.path());
        }
    }
    return true;
}

struct ParsedManifest {
    std::uint64_t instanceIdentity{0};
    std::uint64_t recipeFingerprint{0};
    std::uint64_t providerPlanFingerprint{0};
    std::uint64_t sourceIdentity{0};
    std::uint64_t providerRevision{0};
    std::uint64_t requirementRevision{0};
    std::uint64_t seatId{0};
    std::uint64_t instanceIndex{0};
    std::uint64_t phase{0};
};

enum class ManifestRead : std::uint8_t { Missing = 0, Valid, Invalid };

bool parseUnsignedLine(std::string_view line, std::string_view key, std::uint64_t& output) {
    if (line.size() <= key.size() + 1u || line.substr(0u, key.size()) != key ||
        line[key.size()] != '=') {
        return false;
    }
    const auto value = line.substr(key.size() + 1u);
    if (value.empty()) return false;
    std::uint64_t parsed = 0u;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
    output = parsed;
    return true;
}

ManifestRead readManifest(const fs::path& root, ParsedManifest& output, std::string& error) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        if (ec) error = "failed to inspect instance root: " + ec.message();
        return ec ? ManifestRead::Invalid : ManifestRead::Missing;
    }
    if (!fs::is_directory(root, ec) || ec) {
        error = "instance path exists but is not a directory";
        return ManifestRead::Invalid;
    }
    std::string reparseError;
    bool exists = false;
    if (pathReparsePoint(root, exists, reparseError)) {
        error = reparseError.empty() ? "instance root is a symlink/junction/reparse point" : reparseError;
        return ManifestRead::Invalid;
    }
    const auto manifestPath = root / fs::path(kInstanceManifestName);
    if (pathReparsePoint(manifestPath, exists, reparseError)) {
        error = reparseError.empty()
                    ? "instance ownership manifest is a symlink/junction/reparse point"
                    : std::move(reparseError);
        return ManifestRead::Invalid;
    }
    if (!fs::exists(manifestPath, ec)) {
        if (ec) error = "failed to inspect instance manifest: " + ec.message();
        else error = "instance directory has no HydraSeat ownership manifest";
        return ManifestRead::Invalid;
    }
    const auto size = fs::file_size(manifestPath, ec);
    if (ec || size == 0u || size > kMaximumManifestBytes) {
        error = "instance ownership manifest is missing, unreadable, or oversized";
        return ManifestRead::Invalid;
    }
    std::ifstream stream(manifestPath, std::ios::binary);
    if (!stream) {
        error = "failed to open instance ownership manifest";
        return ManifestRead::Invalid;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (lines.size() > 16u) {
            error = "instance ownership manifest has too many fields";
            return ManifestRead::Invalid;
        }
    }
    if (!stream.eof() || lines.size() != 10u || lines[0] != kInstanceManifestMagic) {
        error = "instance ownership manifest is malformed";
        return ManifestRead::Invalid;
    }
    ParsedManifest parsed;
    if (!parseUnsignedLine(lines[1], "identity", parsed.instanceIdentity) ||
        !parseUnsignedLine(lines[2], "recipe", parsed.recipeFingerprint) ||
        !parseUnsignedLine(lines[3], "plan", parsed.providerPlanFingerprint) ||
        !parseUnsignedLine(lines[4], "source", parsed.sourceIdentity) ||
        !parseUnsignedLine(lines[5], "provider_revision", parsed.providerRevision) ||
        !parseUnsignedLine(lines[6], "requirement_revision", parsed.requirementRevision) ||
        !parseUnsignedLine(lines[7], "seat", parsed.seatId) ||
        !parseUnsignedLine(lines[8], "instance", parsed.instanceIndex) ||
        !parseUnsignedLine(lines[9], "phase", parsed.phase) ||
        parsed.instanceIdentity == 0u || parsed.recipeFingerprint == 0u ||
        parsed.providerPlanFingerprint == 0u || parsed.sourceIdentity == 0u ||
        parsed.providerRevision == 0u || parsed.requirementRevision == 0u ||
        parsed.seatId == 0u || parsed.phase > 3u) {
        error = "instance ownership manifest contains invalid values";
        return ManifestRead::Invalid;
    }
    output = parsed;
    return ManifestRead::Valid;
}

std::string manifestText(const InstanceMaterializationPlan& plan,
                         setup::RecipeExecutionPhase phase) {
    std::string output;
    output.reserve(384u);
    output.append(kInstanceManifestMagic);
    output.push_back('\n');
    output.append("identity=").append(std::to_string(plan.instanceIdentityFingerprint)).push_back('\n');
    output.append("recipe=").append(std::to_string(plan.recipeFingerprint)).push_back('\n');
    output.append("plan=").append(std::to_string(plan.providerPlanFingerprint)).push_back('\n');
    output.append("source=").append(std::to_string(plan.sourceIdentityFingerprint)).push_back('\n');
    output.append("provider_revision=").append(std::to_string(plan.providerMetadataRevision)).push_back('\n');
    output.append("requirement_revision=").append(std::to_string(plan.requirementRevision)).push_back('\n');
    output.append("seat=").append(std::to_string(plan.seatId)).push_back('\n');
    output.append("instance=").append(std::to_string(plan.instanceIndex)).push_back('\n');
    output.append("phase=").append(std::to_string(phaseOrdinal(phase))).push_back('\n');
    return output;
}

bool writeManifest(const fs::path& root,
                   const InstanceMaterializationPlan& plan,
                   setup::RecipeExecutionPhase phase,
                   std::string& error) {
    const auto text = manifestText(plan, phase);
    const auto manifestPath = root / fs::path(kInstanceManifestName);
    bool exists = false;
    if (pathReparsePoint(manifestPath, exists, error)) {
        if (error.empty()) error = "refusing to write an ownership manifest through a reparse point";
        return false;
    }
    std::ofstream stream(manifestPath, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "failed to create instance ownership manifest";
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    if (!stream) {
        error = "failed to durably write instance ownership manifest bytes";
        return false;
    }
    return true;
}

bool manifestOwnedByPlan(const ParsedManifest& manifest,
                         const InstanceMaterializationPlan& plan) noexcept {
    return manifest.instanceIdentity == plan.instanceIdentityFingerprint &&
           manifest.seatId == static_cast<std::uint64_t>(plan.seatId) &&
           manifest.instanceIndex == static_cast<std::uint64_t>(plan.instanceIndex);
}

bool manifestCurrentForPlan(const ParsedManifest& manifest,
                            const InstanceMaterializationPlan& plan) noexcept {
    return manifestOwnedByPlan(manifest, plan) &&
           manifest.recipeFingerprint == plan.recipeFingerprint &&
           manifest.providerPlanFingerprint == plan.providerPlanFingerprint &&
           manifest.sourceIdentity == plan.sourceIdentityFingerprint &&
           manifest.providerRevision == plan.providerMetadataRevision &&
           manifest.requirementRevision == plan.requirementRevision;
}

RecipeDiagnostic removeOwnedTree(const fs::path& root,
                                 const InstanceMaterializationPlan& plan,
                                 RecipeResult errorCode) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        if (ec) return failure(errorCode, "failed to inspect owned cleanup path: " + ec.message());
        return {};
    }
    ParsedManifest manifest;
    std::string error;
    if (readManifest(root, manifest, error) != ManifestRead::Valid ||
        !manifestOwnedByPlan(manifest, plan)) {
        return failure(RecipeResult::UnsafeInstance,
                       error.empty() ? "refusing to delete an unowned materialization path" : error);
    }
    if (!ownedTreeIsPlain(root, error)) {
        return failure(RecipeResult::ReparsePointRejected,
                       error.empty() ? "refusing to delete an owned tree containing a reparse point"
                                     : error);
    }
    fs::remove_all(root, ec);
    if (ec) return failure(errorCode, "failed to remove owned materialization tree: " + ec.message());
    return {};
}

bool renameNoReplace(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    if (fs::exists(to, ec)) {
        if (ec) {
            error = "failed to inspect transaction destination: " + ec.message();
        } else {
            error = "transaction destination already exists";
        }
        return false;
    }
    ec.clear();
    fs::rename(from, to, ec);
    if (ec) {
        error = "transaction rename failed: " + ec.message();
        return false;
    }
    return true;
}

bool filesEqual(const fs::path& left, const fs::path& right, std::string& error) {
    std::ifstream first(left, std::ios::binary);
    std::ifstream second(right, std::ios::binary);
    if (!first || !second) {
        error = "failed to reopen source/staged file for verification";
        return false;
    }
    std::array<char, 64u * 1024u> firstBuffer{};
    std::array<char, 64u * 1024u> secondBuffer{};
    for (;;) {
        first.read(firstBuffer.data(), static_cast<std::streamsize>(firstBuffer.size()));
        second.read(secondBuffer.data(), static_cast<std::streamsize>(secondBuffer.size()));
        const auto firstCount = first.gcount();
        const auto secondCount = second.gcount();
        if (firstCount != secondCount) {
            error = "staged file byte count differs from immutable source";
            return false;
        }
        if (firstCount > 0 &&
            !std::equal(firstBuffer.data(),
                        firstBuffer.data() + static_cast<std::size_t>(firstCount),
                        secondBuffer.data())) {
            error = "staged file bytes differ from immutable source";
            return false;
        }
        if (firstCount == 0) break;
    }
    if ((!first.eof() && first.fail()) || (!second.eof() && second.fail())) {
        error = "source/staged verification read failed";
        return false;
    }
    return true;
}

std::uint64_t sourceIdentityFingerprint(
    const requirement::TrustedGameRuntimeAuthority& authority) {
    MaterialHash hash;
    hash.text(authority.requirement.gameId);
    hash.text(authority.providerId);
    hash.boolean(authority.providerAppId.has_value());
    if (authority.providerAppId) hash.text(*authority.providerAppId);
    hash.u64(authority.providerMetadataRevision);
    hash.u64(authority.requirement.revision);
    hashCompatibility(hash, authority.requirement.compatibility);
    hash.boolean(authority.gameVersionUtf8.has_value());
    if (authority.gameVersionUtf8) hash.text(*authority.gameVersionUtf8);
    hash.boolean(authority.executableSha256.has_value());
    if (authority.executableSha256) hash.text(*authority.executableSha256);
    hash.text(authority.evidenceResultId);
    hash.text(authority.evidenceProvenanceId);
    hash.u64(authority.evidenceProvenanceRevision);
    hash.text(authority.evidenceTimestampBucket);
    hash.u32(static_cast<std::uint32_t>(authority.evidenceOrigin));
    auto candidates = authority.executableCandidates;
    std::sort(candidates.begin(), candidates.end());
    hash.u64(static_cast<std::uint64_t>(candidates.size()));
    for (const auto& candidate : candidates) hash.wide(candidate);
    return hash.value();
}

std::uint64_t instanceIdentityFingerprint(const CompatibilityRecipe& recipe,
                                          const plan::SeatProviderLaunchPlan& seat,
                                          std::string_view sessionId) noexcept {
    MaterialHash hash;
    hash.text(sessionId);
    hash.u32(recipe.seatId);
    hash.u32(seat.instanceIndex);
    hash.text(recipe.gameId);
    hash.text(recipe.providerId);
    hash.boolean(recipe.providerAppId.has_value());
    if (recipe.providerAppId) hash.text(*recipe.providerAppId);
    return hash.value();
}

std::uint64_t compiledRecipeFingerprint(const InstanceMaterializationPlan& plan) {
    MaterialHash hash;
    hash.u32(plan.schemaVersion);
    hash.u32(plan.seatId);
    hash.u32(plan.instanceIndex);
    hash.text(plan.gameId);
    hash.text(plan.providerId);
    hash.boolean(plan.providerAppId.has_value());
    if (plan.providerAppId) hash.text(*plan.providerAppId);
    hash.u64(plan.providerMetadataRevision);
    hash.u64(plan.requirementRevision);
    hashCompatibility(hash, plan.compatibility);
    hash.u64(plan.providerPlanFingerprint);
    hash.u64(plan.sourceIdentityFingerprint);
    hash.u64(plan.instanceIdentityFingerprint);
    hash.u64(static_cast<std::uint64_t>(plan.steps.size()));
    for (const auto& step : plan.steps) {
        hash.text(step.stepId);
        hash.u32(static_cast<std::uint32_t>(step.phase));
        hash.u64(static_cast<std::uint64_t>(step.files.size()));
        for (const auto& file : step.files) {
            hash.wide(file.sourcePath.lexically_relative(plan.sourceRoot).generic_wstring());
            hash.wide(file.destinationRelativePath.generic_wstring());
            hash.u64(file.maximumBytes);
        }
    }
    return hash.value();
}

const requirement::TrustedGameRuntimeAuthority* findTrustedAuthority(
    const requirement::TrustedRequirementSnapshot& snapshot,
    std::string_view gameId) noexcept {
    const requirement::TrustedGameRuntimeAuthority* result = nullptr;
    for (const auto& authority : snapshot.authorities) {
        if (authority.requirement.gameId != gameId) continue;
        if (result != nullptr) return nullptr;
        result = &authority;
    }
    return result;
}

const plan::SeatProviderLaunchPlan* findPlanSeat(
    const plan::ProviderAwareLaunchPlan& providerPlan,
    SeatId seatId) noexcept {
    const plan::SeatProviderLaunchPlan* result = nullptr;
    for (const auto& seat : providerPlan.seats) {
        if (seat.seatId != seatId) continue;
        if (result != nullptr) return nullptr;
        result = &seat;
    }
    return result;
}

bool invokeCheckpoint(const TransactionCheckpointHook& hook,
                      TransactionCheckpoint checkpoint,
                      std::string& reason) {
    if (!hook) return true;
    try {
        if (hook(checkpoint, reason)) return true;
        if (reason.empty()) reason = "transaction checkpoint vetoed";
        return false;
    } catch (const std::exception& exception) {
        reason = std::string("transaction checkpoint threw: ") + exception.what();
        return false;
    } catch (...) {
        reason = "transaction checkpoint failed unexpectedly";
        return false;
    }
}

RecipeDiagnostic stageCumulativeFiles(const InstanceMaterializationPlan& plan,
                                      setup::RecipeExecutionPhase phase) {
    auto cleanup = removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
    if (!cleanup.succeeded()) return cleanup;

    std::error_code ec;
    fs::create_directories(plan.stagingRoot, ec);
    if (ec) return failure(RecipeResult::StagingFailed,
                           "failed to create transaction staging directory: " + ec.message());
    std::string manifestError;
    if (!writeManifest(plan.stagingRoot, plan, phase, manifestError)) {
        fs::remove_all(plan.stagingRoot, ec);
        return failure(RecipeResult::StagingFailed, std::move(manifestError));
    }

    const auto requestedPhase = phaseOrdinal(phase);
    std::uint64_t totalBytes = 0u;
    for (const auto& step : plan.steps) {
        if (phaseOrdinal(step.phase) > requestedPhase) break;
        for (const auto& file : step.files) {
            std::string pathError;
            if (!ancestorsArePlain(file.sourcePath, pathError)) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::ReparsePointRejected,
                               pathError.empty() ? "source path crosses a reparse point" : pathError);
            }
            const auto status = fs::status(file.sourcePath, ec);
            if (ec || !fs::is_regular_file(status)) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::SourceUnavailable,
                               "declared mutable source is missing or not a regular file");
            }
            const auto beforeSize = fs::file_size(file.sourcePath, ec);
            if (ec || beforeSize > file.maximumBytes ||
                beforeSize > kMaximumSingleMutableFileBytes ||
                totalBytes > kMaximumMutableBytesPerInstance - beforeSize) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::BoundsExceeded,
                               "declared mutable source exceeds recipe/file/instance bounds");
            }
            const auto beforeWriteTime = fs::last_write_time(file.sourcePath, ec);
            if (ec) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::SourceUnavailable,
                               "failed to snapshot mutable source write time");
            }
            totalBytes += beforeSize;

            const auto destination = plan.stagingRoot / file.destinationRelativePath;
            fs::create_directories(destination.parent_path(), ec);
            if (ec) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::StagingFailed,
                               "failed to create bounded staged destination directory: " + ec.message());
            }
            if (!ancestorsArePlain(destination.parent_path(), pathError)) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::ReparsePointRejected,
                               pathError.empty() ? "staged destination crosses a reparse point"
                                                 : pathError);
            }
            bool destinationExists = false;
            pathError.clear();
            if (pathReparsePoint(destination, destinationExists, pathError)) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::ReparsePointRejected,
                               pathError.empty() ? "staged destination is a reparse point" : pathError);
            }
            if (destinationExists) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::ConflictingMutation,
                               "staged destination already exists before its declared copy");
            }
            fs::copy_file(file.sourcePath, destination, fs::copy_options::none, ec);
            if (ec) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::StagingFailed,
                               "failed to copy declared mutable source into staging: " + ec.message());
            }
            pathError.clear();
            if (pathReparsePoint(destination, destinationExists, pathError) || !destinationExists) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::ReparsePointRejected,
                               pathError.empty() ? "staged copy did not remain a plain regular path"
                                                 : pathError);
            }
            std::string compareError;
            if (!filesEqual(file.sourcePath, destination, compareError)) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::StagingFailed, std::move(compareError));
            }
            const auto afterSize = fs::file_size(file.sourcePath, ec);
            if (ec || afterSize != beforeSize ||
                fs::last_write_time(file.sourcePath, ec) != beforeWriteTime || ec) {
                (void)removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
                return failure(RecipeResult::SourceUnavailable,
                               "immutable source changed while materialization was staged");
            }
        }
    }
    return {};
}

} // namespace

RecipeDiagnostic compileInstanceMaterializationPlan(
    const CompatibilityRecipe& recipe,
    const plan::ProviderAwareLaunchPlan& providerPlan,
    const requirement::TrustedRequirementSnapshot& trustedSnapshot,
    const MaterializationContext& context,
    InstanceMaterializationPlan& output) {
    try {
        if (recipe.schemaVersion != kCompatibilityRecipeSchemaVersion || recipe.seatId == 0u ||
            !validOpaqueId(recipe.gameId) || !validOpaqueId(recipe.providerId) ||
            (recipe.providerAppId && !validOpaqueId(*recipe.providerAppId)) ||
            recipe.providerMetadataRevision == 0u || recipe.requirementRevision == 0u ||
            recipe.steps.empty() || recipe.steps.size() > kMaximumCompatibilityRecipeSteps) {
            return failure(RecipeResult::InvalidRecipe,
                           "compatibility recipe has invalid bounded identity/version/step fields");
        }
        if (!context.instancesRoot.is_absolute() || !pathLengthBounded(context.instancesRoot) ||
            !validSessionId(context.sessionId)) {
            return failure(RecipeResult::InvalidPath,
                           "materialization root/session identity is invalid or unbounded");
        }
        const auto recomputed = plan::recomputeProviderAwareLaunchPlanFingerprint(providerPlan);
        if (recomputed == 0u || providerPlan.fingerprint != recomputed) {
            return failure(RecipeResult::UntrustedProviderPlan,
                           "provider plan fingerprint is absent or does not match canonical contents");
        }
        const auto trusted =
            requirement::validateProviderAwareLaunchPlanAgainstTrustedRequirements(
                providerPlan, trustedSnapshot, true);
        if (!trusted.succeeded()) {
            return failure(RecipeResult::UntrustedProviderPlan,
                           "provider plan failed exact trusted runtime requirement validation: " +
                               trusted.message);
        }
        const auto* seat = findPlanSeat(providerPlan, recipe.seatId);
        if (seat == nullptr) {
            return failure(RecipeResult::WrongGameIdentity,
                           "recipe Seat is absent or duplicated in the immutable provider plan");
        }
        if (seat->gameId != recipe.gameId || seat->launchRequest.gameId != recipe.gameId) {
            return failure(RecipeResult::WrongGameIdentity,
                           "recipe Game identity does not match the immutable provider plan");
        }
        if (seat->launchRequest.providerId != recipe.providerId ||
            seat->launchRequest.providerAppId != recipe.providerAppId) {
            return failure(RecipeResult::WrongProviderIdentity,
                           "recipe provider/application identity does not match the provider plan");
        }
        if (seat->launchRequest.metadataRevision != recipe.providerMetadataRevision) {
            return failure(RecipeResult::StaleProviderRevision,
                           "recipe provider revision is stale or belongs to another installation snapshot");
        }
        if (seat->requirementRevision != recipe.requirementRevision ||
            seat->compatibility != recipe.compatibility) {
            return failure(RecipeResult::StaleRequirementRevision,
                           "recipe requirement/compatibility revision is stale or conflicting");
        }
        const auto* authority = findTrustedAuthority(trustedSnapshot, recipe.gameId);
        if (authority == nullptr) {
            return failure(RecipeResult::UntrustedProviderPlan,
                           "trusted snapshot has no unique exact Game authority for recipe execution");
        }
        if (seat->launchRequest.targetKind != provider::LaunchTargetKind::Executable) {
            return failure(RecipeResult::UnsupportedSourceLayout,
                           "writable materialization requires an exact trusted executable source layout");
        }
        fs::path executable(seat->launchRequest.target);
        if (!executable.is_absolute() || executable.filename().empty() ||
            executable.parent_path().empty()) {
            return failure(RecipeResult::UnsupportedSourceLayout,
                           "trusted executable target has no bounded absolute source directory");
        }

        InstanceMaterializationPlan compiled;
        compiled.seatId = recipe.seatId;
        compiled.instanceIndex = seat->instanceIndex;
        compiled.gameId = recipe.gameId;
        compiled.providerId = recipe.providerId;
        compiled.providerAppId = recipe.providerAppId;
        compiled.providerMetadataRevision = recipe.providerMetadataRevision;
        compiled.requirementRevision = recipe.requirementRevision;
        compiled.compatibility = recipe.compatibility;
        compiled.providerPlanFingerprint = recomputed;
        compiled.sourceIdentityFingerprint = sourceIdentityFingerprint(*authority);
        compiled.instanceIdentityFingerprint =
            instanceIdentityFingerprint(recipe, *seat, context.sessionId);
        compiled.sourceRoot = executable.parent_path().lexically_normal();
        if (!pathLengthBounded(compiled.sourceRoot)) {
            return failure(RecipeResult::InvalidPath, "trusted executable source path exceeds bounds");
        }

        std::set<std::string> stepIds;
        std::vector<fs::path> destinations;
        std::size_t fileCount = 0u;
        std::uint64_t declaredMaximum = 0u;
        for (const auto& step : recipe.steps) {
            if (!validOpaqueId(step.stepId)) {
                return failure(RecipeResult::InvalidRecipe,
                               "recipe step identity is malformed or unbounded");
            }
            if (!stepIds.insert(step.stepId).second) {
                return failure(RecipeResult::ConflictingMutation,
                               "recipe contains duplicate step identity");
            }
            if (phaseOrdinal(step.phase) < 0) {
                return failure(RecipeResult::UnsupportedPhase,
                               "recipe declares an unsupported execution phase");
            }
            if (!validScope(step.scope)) {
                return failure(RecipeResult::InvalidRecipe,
                               "recipe declares an unknown mutation scope");
            }
            if (step.scope == setup::MutationScope::SharedInstallation) {
                return failure(RecipeResult::SharedInstallationMutationDenied,
                               "shared-install mutation has no v1 safe contract and is denied");
            }
            if (step.files.empty()) {
                return failure(RecipeResult::InvalidRecipe,
                               "materialization step must declare at least one bounded mutable file");
            }
            if (fileCount > kMaximumMutableFilesPerRecipe - step.files.size()) {
                return failure(RecipeResult::BoundsExceeded,
                               "recipe exceeds the bounded mutable-file count");
            }
            fileCount += step.files.size();

            PlannedCompatibilityStep plannedStep;
            plannedStep.stepId = step.stepId;
            plannedStep.phase = step.phase;
            for (const auto& file : step.files) {
                fs::path sourceRelative;
                fs::path destinationRelative;
                if (!validRelativeRecipePath(file.sourceRelativePath, sourceRelative) ||
                    !validRelativeRecipePath(file.destinationRelativePath, destinationRelative)) {
                    return failure(RecipeResult::InvalidPath,
                                   "recipe mutable-file path is absolute, traversing, malformed, or non-portable");
                }
                if (file.maximumBytes == 0u ||
                    file.maximumBytes > kMaximumSingleMutableFileBytes ||
                    declaredMaximum > kMaximumMutableBytesPerInstance - file.maximumBytes) {
                    return failure(RecipeResult::BoundsExceeded,
                                   "recipe mutable-file byte bounds exceed the per-file/instance maximum");
                }
                declaredMaximum += file.maximumBytes;
                for (const auto& existing : destinations) {
                    if (pathPrefixConflict(existing, destinationRelative)) {
                        return failure(RecipeResult::ConflictingMutation,
                                       "recipe contains duplicate or ancestor/child destination mutations");
                    }
                }
                destinations.push_back(destinationRelative);
                plannedStep.files.push_back({
                    (compiled.sourceRoot / sourceRelative).lexically_normal(),
                    destinationRelative,
                    file.maximumBytes,
                });
            }
            std::sort(plannedStep.files.begin(), plannedStep.files.end(),
                      [](const auto& left, const auto& right) {
                          const auto leftDestination = windowsPathKey(left.destinationRelativePath);
                          const auto rightDestination = windowsPathKey(right.destinationRelativePath);
                          if (leftDestination != rightDestination) {
                              return leftDestination < rightDestination;
                          }
                          return windowsPathKey(left.sourcePath) < windowsPathKey(right.sourcePath);
                      });
            compiled.steps.push_back(std::move(plannedStep));
        }
        std::sort(compiled.steps.begin(), compiled.steps.end(), [](const auto& left, const auto& right) {
            const auto leftPhase = phaseOrdinal(left.phase);
            const auto rightPhase = phaseOrdinal(right.phase);
            if (leftPhase != rightPhase) return leftPhase < rightPhase;
            return left.stepId < right.stepId;
        });

        const auto directoryName = std::string("seat-") + std::to_string(compiled.seatId) +
                                   "-instance-" + std::to_string(compiled.instanceIndex) + "-" +
                                   hex64(compiled.instanceIdentityFingerprint);
        compiled.instanceRoot = (context.instancesRoot / fs::path(directoryName)).lexically_normal();
        compiled.stagingRoot = fs::path(compiled.instanceRoot.wstring() + L".staging");
        compiled.rollbackRoot = fs::path(compiled.instanceRoot.wstring() + L".rollback");
        compiled.previousPhaseRoot = fs::path(compiled.instanceRoot.wstring() + L".previous");
        if (!pathLengthBounded(compiled.instanceRoot) || !pathLengthBounded(compiled.stagingRoot) ||
            !pathLengthBounded(compiled.rollbackRoot) ||
            !pathLengthBounded(compiled.previousPhaseRoot)) {
            return failure(RecipeResult::InvalidPath,
                           "deterministic per-instance materialization path exceeds bounds");
        }
        compiled.recipeFingerprint = compiledRecipeFingerprint(compiled);
        output = std::move(compiled);
        return {};
    } catch (const std::exception& exception) {
        return failure(RecipeResult::InvalidRecipe,
                       std::string("compatibility recipe compilation failed: ") + exception.what());
    } catch (...) {
        return failure(RecipeResult::InvalidRecipe,
                       "compatibility recipe compilation failed unexpectedly");
    }
}

RecipeDiagnostic inspectInstanceMaterialization(
    const InstanceMaterializationPlan& plan,
    InstanceState& state) {
    state = InstanceState::Unsafe;
    try {
        std::string pathError;
        if (!ancestorsArePlain(plan.instanceRoot.parent_path(), pathError)) {
            return failure(RecipeResult::ReparsePointRejected,
                           pathError.empty() ? "instance root parent crosses a reparse point" : pathError);
        }
        ParsedManifest manifest;
        std::string error;
        const auto result = readManifest(plan.instanceRoot, manifest, error);
        if (result == ManifestRead::Missing) {
            state = InstanceState::Missing;
            return {};
        }
        if (result != ManifestRead::Valid || !manifestOwnedByPlan(manifest, plan)) {
            state = InstanceState::Unsafe;
            return failure(RecipeResult::UnsafeInstance,
                           error.empty() ? "instance path is not owned by this Seat/session identity" : error);
        }
        if (!manifestCurrentForPlan(manifest, plan)) {
            state = InstanceState::Stale;
            return failure(RecipeResult::StaleInstance,
                           "existing Seat instance belongs to an older/conflicting recipe or source revision");
        }
        if (manifest.phase < static_cast<std::uint64_t>(phaseOrdinal(setup::RecipeExecutionPhase::Runtime))) {
            state = InstanceState::Partial;
            return failure(RecipeResult::StaleInstance,
                           "existing Seat instance contains only a partially applied recipe");
        }
        state = InstanceState::Current;
        return {RecipeResult::AlreadyCurrent, "exact Seat writable instance is already current"};
    } catch (const std::exception& exception) {
        return failure(RecipeResult::UnsafeInstance,
                       std::string("instance inspection failed: ") + exception.what());
    } catch (...) {
        return failure(RecipeResult::UnsafeInstance, "instance inspection failed unexpectedly");
    }
}

RecipeDiagnostic recoverInterruptedMaterialization(
    const InstanceMaterializationPlan& plan) {
    try {
        std::string pathError;
        if (!ancestorsArePlain(plan.instanceRoot.parent_path(), pathError)) {
            return failure(RecipeResult::ReparsePointRejected,
                           pathError.empty() ? "materialization parent crosses a reparse point" : pathError);
        }
        auto cleanup = removeOwnedTree(plan.stagingRoot, plan, RecipeResult::CleanupFailed);
        if (!cleanup.succeeded()) return cleanup;

        std::error_code ec;
        const bool rollbackExists = fs::exists(plan.rollbackRoot, ec);
        if (ec) return failure(RecipeResult::RollbackFailed,
                               "failed to inspect retained rollback instance: " + ec.message());
        const bool previousExists = fs::exists(plan.previousPhaseRoot, ec);
        if (ec) return failure(RecipeResult::RollbackFailed,
                               "failed to inspect previous-phase instance: " + ec.message());
        const bool finalExists = fs::exists(plan.instanceRoot, ec);
        if (ec) return failure(RecipeResult::RollbackFailed,
                               "failed to inspect committed instance: " + ec.message());

        if (rollbackExists) {
            ParsedManifest rollbackManifest;
            std::string error;
            if (readManifest(plan.rollbackRoot, rollbackManifest, error) != ManifestRead::Valid ||
                !manifestOwnedByPlan(rollbackManifest, plan)) {
                return failure(RecipeResult::UnsafeInstance,
                               "retained rollback path is not owned by this Seat/session identity");
            }
            bool keepCommittedFinal = false;
            if (finalExists) {
                ParsedManifest finalManifest;
                if (readManifest(plan.instanceRoot, finalManifest, error) != ManifestRead::Valid ||
                    !manifestOwnedByPlan(finalManifest, plan)) {
                    return failure(RecipeResult::UnsafeInstance,
                                   "committed path is foreign while an owned rollback exists");
                }
                keepCommittedFinal = manifestCurrentForPlan(finalManifest, plan) &&
                                     finalManifest.phase == 3u;
                if (!keepCommittedFinal) {
                    cleanup = removeOwnedTree(plan.instanceRoot, plan, RecipeResult::RollbackFailed);
                    if (!cleanup.succeeded()) return cleanup;
                }
            }
            if (keepCommittedFinal) {
                cleanup = removeOwnedTree(plan.rollbackRoot, plan, RecipeResult::CleanupFailed);
                if (!cleanup.succeeded()) return cleanup;
            } else {
                std::string renameError;
                if (!renameNoReplace(plan.rollbackRoot, plan.instanceRoot, renameError)) {
                    return failure(RecipeResult::RollbackFailed, std::move(renameError));
                }
            }
            cleanup = removeOwnedTree(plan.previousPhaseRoot, plan, RecipeResult::CleanupFailed);
            if (!cleanup.succeeded()) return cleanup;
            return {};
        }

        if (previousExists) {
            ParsedManifest previousManifest;
            std::string error;
            if (readManifest(plan.previousPhaseRoot, previousManifest, error) != ManifestRead::Valid ||
                !manifestOwnedByPlan(previousManifest, plan)) {
                return failure(RecipeResult::UnsafeInstance,
                               "previous-phase path is not owned by this Seat/session identity");
            }
            if (finalExists) {
                ParsedManifest finalManifest;
                if (readManifest(plan.instanceRoot, finalManifest, error) != ManifestRead::Valid ||
                    !manifestOwnedByPlan(finalManifest, plan)) {
                    return failure(RecipeResult::UnsafeInstance,
                                   "committed path is foreign while a previous phase exists");
                }
                cleanup = removeOwnedTree(plan.previousPhaseRoot, plan, RecipeResult::CleanupFailed);
                if (!cleanup.succeeded()) return cleanup;
            } else {
                std::string renameError;
                if (!renameNoReplace(plan.previousPhaseRoot, plan.instanceRoot, renameError)) {
                    return failure(RecipeResult::RollbackFailed, std::move(renameError));
                }
            }
        }

        InstanceState state = InstanceState::Unsafe;
        const auto inspected = inspectInstanceMaterialization(plan, state);
        if (state == InstanceState::Partial) {
            cleanup = removeOwnedTree(plan.instanceRoot, plan, RecipeResult::RollbackFailed);
            if (!cleanup.succeeded()) return cleanup;
            return {};
        }
        if (state == InstanceState::Unsafe) return inspected;
        return {};
    } catch (const std::exception& exception) {
        return failure(RecipeResult::RollbackFailed,
                       std::string("interrupted materialization recovery failed: ") + exception.what());
    } catch (...) {
        return failure(RecipeResult::RollbackFailed,
                       "interrupted materialization recovery failed unexpectedly");
    }
}

RecipeDiagnostic cleanupInstanceMaterialization(
    const InstanceMaterializationPlan& plan) {
    try {
        for (const auto& path : {plan.stagingRoot, plan.previousPhaseRoot,
                                 plan.instanceRoot, plan.rollbackRoot}) {
            const auto diagnostic = removeOwnedTree(path, plan, RecipeResult::CleanupFailed);
            if (!diagnostic.succeeded()) return diagnostic;
        }
        return {};
    } catch (const std::exception& exception) {
        return failure(RecipeResult::CleanupFailed,
                       std::string("instance cleanup failed: ") + exception.what());
    } catch (...) {
        return failure(RecipeResult::CleanupFailed, "instance cleanup failed unexpectedly");
    }
}

RecipeExecutionSession::RecipeExecutionSession(
    InstanceMaterializationPlan plan,
    TransactionCheckpointHook checkpointHook)
    : plan_(std::move(plan)), checkpointHook_(std::move(checkpointHook)) {}

RecipeDiagnostic RecipeExecutionSession::ensureRecovered() {
    if (recovered_) return {};
    auto recovered = recoverInterruptedMaterialization(plan_);
    if (!recovered.succeeded()) return recovered;
    recovered_ = true;

    InstanceState state = InstanceState::Unsafe;
    const auto inspected = inspectInstanceMaterialization(plan_, state);
    if (state == InstanceState::Unsafe) return inspected;
    if (state == InstanceState::Current) {
        reusedCurrent_ = true;
        originalExisted_ = true;
        return {};
    }
    if (state == InstanceState::Partial) {
        return failure(RecipeResult::StaleInstance,
                       "interrupted partial instance survived recovery unexpectedly");
    }
    originalExisted_ = state == InstanceState::Stale;
    return {};
}

RecipeDiagnostic RecipeExecutionSession::materializeThrough(
    setup::RecipeExecutionPhase phase) {
    const auto staged = stageCumulativeFiles(plan_, phase);
    if (!staged.succeeded()) return staged;

    std::string checkpointReason;
    if (!invokeCheckpoint(checkpointHook_, TransactionCheckpoint::StagingValidated,
                          checkpointReason)) {
        (void)removeOwnedTree(plan_.stagingRoot, plan_, RecipeResult::CleanupFailed);
        return failure(RecipeResult::StagingFailed, std::move(checkpointReason));
    }

    std::error_code ec;
    const bool finalExists = fs::exists(plan_.instanceRoot, ec);
    if (ec) {
        (void)removeOwnedTree(plan_.stagingRoot, plan_, RecipeResult::CleanupFailed);
        return failure(RecipeResult::CommitFailed,
                       "failed to inspect current instance before commit: " + ec.message());
    }

    if (!originalBackedUp_) {
        originalExisted_ = finalExists;
        if (finalExists) {
            ParsedManifest manifest;
            std::string error;
            if (readManifest(plan_.instanceRoot, manifest, error) != ManifestRead::Valid ||
                !manifestOwnedByPlan(manifest, plan_)) {
                (void)removeOwnedTree(plan_.stagingRoot, plan_, RecipeResult::CleanupFailed);
                return failure(RecipeResult::UnsafeInstance,
                               "refusing to replace a foreign/unowned instance path");
            }
            std::string renameError;
            if (!renameNoReplace(plan_.instanceRoot, plan_.rollbackRoot, renameError)) {
                (void)removeOwnedTree(plan_.stagingRoot, plan_, RecipeResult::CleanupFailed);
                return failure(RecipeResult::CommitFailed, std::move(renameError));
            }
        }
        originalBackedUp_ = true;
        if (!invokeCheckpoint(checkpointHook_, TransactionCheckpoint::PreviousInstanceMoved,
                              checkpointReason)) {
            if (originalExisted_) {
                std::string restoreError;
                if (!renameNoReplace(plan_.rollbackRoot, plan_.instanceRoot, restoreError)) {
                    return failure(RecipeResult::RollbackFailed,
                                   checkpointReason + "; previous instance restore failed: " + restoreError);
                }
            }
            originalBackedUp_ = false;
            (void)removeOwnedTree(plan_.stagingRoot, plan_, RecipeResult::CleanupFailed);
            return failure(RecipeResult::CommitFailed, std::move(checkpointReason));
        }
        std::string renameError;
        if (!renameNoReplace(plan_.stagingRoot, plan_.instanceRoot, renameError)) {
            if (originalExisted_) {
                std::string restoreError;
                if (!renameNoReplace(plan_.rollbackRoot, plan_.instanceRoot, restoreError)) {
                    return failure(RecipeResult::RollbackFailed,
                                   renameError + "; previous instance restore failed: " + restoreError);
                }
            }
            originalBackedUp_ = false;
            return failure(RecipeResult::CommitFailed, std::move(renameError));
        }
        return {};
    }

    if (!finalExists) {
        (void)removeOwnedTree(plan_.stagingRoot, plan_, RecipeResult::CleanupFailed);
        return failure(RecipeResult::CommitFailed,
                       "previous recipe phase has no committed instance to advance");
    }
    ParsedManifest manifest;
    std::string error;
    if (readManifest(plan_.instanceRoot, manifest, error) != ManifestRead::Valid ||
        !manifestOwnedByPlan(manifest, plan_)) {
        (void)removeOwnedTree(plan_.stagingRoot, plan_, RecipeResult::CleanupFailed);
        return failure(RecipeResult::UnsafeInstance,
                       "previous recipe phase instance is foreign or unowned");
    }
    std::string renameError;
    if (!renameNoReplace(plan_.instanceRoot, plan_.previousPhaseRoot, renameError)) {
        (void)removeOwnedTree(plan_.stagingRoot, plan_, RecipeResult::CleanupFailed);
        return failure(RecipeResult::CommitFailed, std::move(renameError));
    }
    if (!invokeCheckpoint(checkpointHook_, TransactionCheckpoint::PreviousInstanceMoved,
                          checkpointReason)) {
        std::string restoreError;
        if (!renameNoReplace(plan_.previousPhaseRoot, plan_.instanceRoot, restoreError)) {
            return failure(RecipeResult::RollbackFailed,
                           checkpointReason + "; previous phase restore failed: " + restoreError);
        }
        (void)removeOwnedTree(plan_.stagingRoot, plan_, RecipeResult::CleanupFailed);
        return failure(RecipeResult::CommitFailed, std::move(checkpointReason));
    }
    if (!renameNoReplace(plan_.stagingRoot, plan_.instanceRoot, renameError)) {
        std::string restoreError;
        if (!renameNoReplace(plan_.previousPhaseRoot, plan_.instanceRoot, restoreError)) {
            return failure(RecipeResult::RollbackFailed,
                           renameError + "; previous phase restore failed: " + restoreError);
        }
        return failure(RecipeResult::CommitFailed, std::move(renameError));
    }
    const auto cleanup = removeOwnedTree(plan_.previousPhaseRoot, plan_, RecipeResult::CleanupFailed);
    if (!cleanup.succeeded()) return cleanup;
    return {};
}

RecipeDiagnostic RecipeExecutionSession::reverseApplied() {
    auto cleanup = removeOwnedTree(plan_.stagingRoot, plan_, RecipeResult::RollbackFailed);
    if (!cleanup.succeeded()) return cleanup;

    std::error_code ec;
    const bool previousExists = fs::exists(plan_.previousPhaseRoot, ec);
    if (ec) return failure(RecipeResult::RollbackFailed,
                           "failed to inspect previous phase during rollback: " + ec.message());
    if (previousExists) {
        const bool finalExists = fs::exists(plan_.instanceRoot, ec);
        if (ec) return failure(RecipeResult::RollbackFailed,
                               "failed to inspect current phase during rollback: " + ec.message());
        if (finalExists) {
            cleanup = removeOwnedTree(plan_.instanceRoot, plan_, RecipeResult::RollbackFailed);
            if (!cleanup.succeeded()) return cleanup;
        }
        std::string restoreError;
        if (!renameNoReplace(plan_.previousPhaseRoot, plan_.instanceRoot, restoreError)) {
            return failure(RecipeResult::RollbackFailed, std::move(restoreError));
        }
    }

    if (!originalBackedUp_) return {};
    const bool finalExists = fs::exists(plan_.instanceRoot, ec);
    if (ec) return failure(RecipeResult::RollbackFailed,
                           "failed to inspect current instance during full rollback: " + ec.message());
    if (finalExists) {
        cleanup = removeOwnedTree(plan_.instanceRoot, plan_, RecipeResult::RollbackFailed);
        if (!cleanup.succeeded()) return cleanup;
    }
    if (originalExisted_) {
        const bool rollbackExists = fs::exists(plan_.rollbackRoot, ec);
        if (ec || !rollbackExists) {
            return failure(RecipeResult::RollbackFailed,
                           "retained previous committed instance is unavailable for rollback");
        }
        std::string restoreError;
        if (!renameNoReplace(plan_.rollbackRoot, plan_.instanceRoot, restoreError)) {
            return failure(RecipeResult::RollbackFailed, std::move(restoreError));
        }
    } else {
        cleanup = removeOwnedTree(plan_.rollbackRoot, plan_, RecipeResult::RollbackFailed);
        if (!cleanup.succeeded()) return cleanup;
    }
    originalBackedUp_ = false;
    return {};
}

RecipeDiagnostic RecipeExecutionSession::executePhase(setup::RecipeExecutionPhase phase) {
    if (finalized_) {
        return failure(RecipeResult::AlreadyFinalized,
                       "materialization transaction is already finalized");
    }
    if (failed_) {
        return failure(RecipeResult::RollbackFailed,
                       "materialization transaction already failed and was reversed");
    }
    const auto ordinal = phaseOrdinal(phase);
    if (ordinal < 0) {
        return failure(RecipeResult::UnsupportedPhase, "unsupported recipe execution phase");
    }
    if (ordinal != completedPhase_ + 1) {
        return failure(RecipeResult::WrongPhaseOrder,
                       "recipe phases must execute exactly PreSpawn -> Startup -> PostWindow -> Runtime");
    }
    const auto recovered = ensureRecovered();
    if (!recovered.succeeded()) {
        failed_ = true;
        return recovered;
    }

    bool phaseHasFiles = false;
    for (const auto& step : plan_.steps) {
        if (phaseOrdinal(step.phase) == ordinal && !step.files.empty()) {
            phaseHasFiles = true;
            break;
        }
    }
    if (!reusedCurrent_ && phaseHasFiles) {
        const auto applied = materializeThrough(phase);
        if (!applied.succeeded()) {
            const auto reversed = reverseApplied();
            failed_ = true;
            if (!reversed.succeeded()) {
                return failure(RecipeResult::RollbackFailed,
                               applied.message + "; rollback also failed: " + reversed.message);
            }
            return applied;
        }
    }
    completedPhase_ = ordinal;

    if (phase == setup::RecipeExecutionPhase::Runtime) {
        if (!reusedCurrent_) {
            std::error_code ec;
            if (!fs::exists(plan_.instanceRoot, ec) || ec) {
                const auto reversed = reverseApplied();
                failed_ = true;
                if (!reversed.succeeded()) return reversed;
                return failure(RecipeResult::CommitFailed,
                               "recipe reached Runtime without a committed writable instance");
            }
            std::string manifestError;
            if (!writeManifest(plan_.instanceRoot, plan_, setup::RecipeExecutionPhase::Runtime,
                               manifestError)) {
                const auto reversed = reverseApplied();
                failed_ = true;
                if (!reversed.succeeded()) return reversed;
                return failure(RecipeResult::CommitFailed, std::move(manifestError));
            }
        }
        if (!reusedCurrent_ && originalBackedUp_ && originalExisted_) {
            const auto cleanup = removeOwnedTree(plan_.rollbackRoot, plan_, RecipeResult::CleanupFailed);
            if (!cleanup.succeeded()) {
                const auto reversed = reverseApplied();
                failed_ = true;
                if (!reversed.succeeded()) return reversed;
                return cleanup;
            }
        }
        originalBackedUp_ = false;
        finalized_ = true;
    }
    return reusedCurrent_ ?
        RecipeDiagnostic{RecipeResult::AlreadyCurrent, "exact Seat instance reused without mutation"} :
        RecipeDiagnostic{};
}

RecipeDiagnostic RecipeExecutionSession::rollback() {
    if (finalized_) {
        return failure(RecipeResult::AlreadyFinalized,
                       "finalized materialization has already released its rollback snapshot");
    }
    if (reusedCurrent_) {
        failed_ = true;
        return {};
    }
    const auto reversed = reverseApplied();
    if (reversed.succeeded()) failed_ = true;
    return reversed;
}

std::string_view recipeResultName(RecipeResult result) noexcept {
    switch (result) {
    case RecipeResult::Success: return "Success";
    case RecipeResult::AlreadyCurrent: return "AlreadyCurrent";
    case RecipeResult::InvalidRecipe: return "InvalidRecipe";
    case RecipeResult::UnsupportedPhase: return "UnsupportedPhase";
    case RecipeResult::WrongPhaseOrder: return "WrongPhaseOrder";
    case RecipeResult::WrongGameIdentity: return "WrongGameIdentity";
    case RecipeResult::WrongProviderIdentity: return "WrongProviderIdentity";
    case RecipeResult::StaleProviderRevision: return "StaleProviderRevision";
    case RecipeResult::StaleRequirementRevision: return "StaleRequirementRevision";
    case RecipeResult::UntrustedProviderPlan: return "UntrustedProviderPlan";
    case RecipeResult::InvalidPath: return "InvalidPath";
    case RecipeResult::BoundsExceeded: return "BoundsExceeded";
    case RecipeResult::ConflictingMutation: return "ConflictingMutation";
    case RecipeResult::SharedInstallationMutationDenied: return "SharedInstallationMutationDenied";
    case RecipeResult::UnsupportedSourceLayout: return "UnsupportedSourceLayout";
    case RecipeResult::SourceUnavailable: return "SourceUnavailable";
    case RecipeResult::ReparsePointRejected: return "ReparsePointRejected";
    case RecipeResult::StaleInstance: return "StaleInstance";
    case RecipeResult::UnsafeInstance: return "UnsafeInstance";
    case RecipeResult::StagingFailed: return "StagingFailed";
    case RecipeResult::CommitFailed: return "CommitFailed";
    case RecipeResult::RollbackFailed: return "RollbackFailed";
    case RecipeResult::CleanupFailed: return "CleanupFailed";
    case RecipeResult::AlreadyFinalized: return "AlreadyFinalized";
    }
    return "Unknown";
}

std::string_view instanceStateName(InstanceState state) noexcept {
    switch (state) {
    case InstanceState::Missing: return "Missing";
    case InstanceState::Current: return "Current";
    case InstanceState::Partial: return "Partial";
    case InstanceState::Stale: return "Stale";
    case InstanceState::Unsafe: return "Unsafe";
    }
    return "Unknown";
}

} // namespace hydra::materialization
