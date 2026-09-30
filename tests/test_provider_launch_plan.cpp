#include "hydra/provider_launch_plan.hpp"
#include "hydra/instance_materialization.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::plan;
using namespace hydra::provider;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class FakeProvider final : public LauncherProviderAdapter {
public:
    ProviderDescriptor descriptorValue{
        "fake", ProviderAvailability::Available, 9u,
        {true, true, true, true, true}};

    ProviderDescriptor descriptor() const noexcept override { return descriptorValue; }
    DiscoveryResponse discoverInstalledGames() noexcept override {
        return {ProviderResult::Success, descriptorValue.metadataRevision, {}, {}};
    }
    AccountReferenceResponse listAccountReferences() noexcept override {
        return {ProviderResult::Success, descriptorValue.metadataRevision, {}, {}};
    }
    LaunchResponse buildLaunchRequest(const LaunchSelection& selection) noexcept override {
        ProviderLaunchRequest request;
        request.providerId = "fake";
        request.gameId = selection.gameId;
        request.providerAppId = selection.providerAppId;
        request.accountRef = selection.accountRef;
        request.metadataRevision = descriptorValue.metadataRevision;
        request.targetKind = LaunchTargetKind::ProviderUri;
        request.target = L"fake://launch/";
        if (selection.providerAppId) {
            for (const char ch : *selection.providerAppId) {
                request.target.push_back(static_cast<wchar_t>(ch));
            }
        }
        request.arguments = selection.instanceArguments;
        request.launchCorrelationId = "fake-" + selection.gameId;
        return {ProviderResult::Success, std::move(request), {}};
    }
    ProcessIdentificationResponse identifyProcesses(
        const ProcessIdentificationQuery&) noexcept override {
        return {ProviderResult::Success, descriptorValue.metadataRevision, {}, {}};
    }
};

profile::CompatibilityReference compatibility(std::string id = "compat-a") {
    return {std::move(id), "fixture", 3u};
}

profile::SeatConfigDocument seats() {
    profile::SeatConfigDocument document;
    document.managementSeatId = 1u;
    profile::PersistedSeatConfig first;
    first.seatId = 1u;
    first.name = L"Seat 1";
    first.displayIds = {L"display-1"};
    first.primaryDisplayId = L"display-1";
    first.keyboardIds = {L"keyboard-1"};
    first.mouseIds = {L"mouse-1"};
    first.controllerIds = {L"controller-1"};
    first.audioOutputEndpointId = L"audio-1";
    profile::PersistedSeatConfig second;
    second.seatId = 2u;
    second.name = L"Seat 2";
    second.displayIds = {L"display-2"};
    second.primaryDisplayId = L"display-2";
    second.keyboardIds = {L"keyboard-2"};
    second.mouseIds = {L"mouse-2"};
    second.controllerIds = {L"controller-2"};
    second.audioOutputEndpointId = L"audio-2";
    document.seats = {first, second};
    return document;
}

profile::PlayerProfileDocument players() {
    profile::PlayerProfileDocument document;
    document.players = {
        {"player-1", L"Player 1", "en-US", {{"fake", "account-1"}}},
        {"player-2", L"Player 2", "en-US", {{"fake", "account-2"}}},
    };
    return document;
}

profile::GameRecord game(std::string id, std::string app, std::wstring path) {
    profile::GameRecord record;
    record.gameId = std::move(id);
    record.providerId = "fake";
    record.providerAppId = std::move(app);
    record.title = L"Fixture";
    record.installRoot = L"C:\\Games";
    record.executableCandidates = {std::move(path)};
    record.compatibility = compatibility();
    record.origin = profile::GameOrigin::Discovered;
    return record;
}

profile::GameRecordDocument games() {
    profile::GameRecordDocument document;
    document.games = {
        game("game:a", "100", L"C:\\Games\\A.exe"),
        game("game:b", "200", L"C:\\Games\\B.exe"),
    };
    return document;
}

GameRuntimeRequirement requirement(std::string gameId) {
    GameRuntimeRequirement value;
    value.gameId = std::move(gameId);
    value.revision = 11u;
    value.validatedSeatCount = 2u;
    value.requirements.display = true;
    value.requirements.keyboard = true;
    value.requirements.mouse = true;
    value.requirements.controller = false;
    value.requirements.audioOutput = true;
    value.requirements.windowOwnership = true;
    value.requirements.recovery = true;
    value.capabilities = {};
    value.compatibility = compatibility();
    return value;
}

std::vector<GameRuntimeRequirement> requirements() {
    return {requirement("game:a"), requirement("game:b")};
}

profile::RuntimeSessionSelection twoDifferentGames(bool reverse = false) {
    profile::RuntimeSessionSelection selection;
    selection.bindings = {
        {1u, "player-1", "game:a", std::nullopt, 0u},
        {2u, "player-2", "game:b", std::nullopt, 0u},
    };
    if (reverse) std::swap(selection.bindings[0], selection.bindings[1]);
    return selection;
}

void testDeterministicDifferentGamePlan() {
    FakeProvider fake;
    const std::vector<ProviderAdapterBinding> providers{{"fake", &fake}};
    const auto seatDocument = seats();
    const auto playerDocument = players();
    const auto gameDocument = games();
    const profile::TwoPlayerSetupDocument setupDocument;
    const auto requirementSet = requirements();

    const auto first = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, gameDocument, setupDocument,
        twoDifferentGames(false), providers, requirementSet);
    const auto second = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, gameDocument, setupDocument,
        twoDifferentGames(true), providers, requirementSet);

    check(first.succeeded() && second.succeeded(),
          "valid different-game selections compile");
    check(first.plan == second.plan && first.plan->fingerprint == second.plan->fingerprint,
          "equivalent binding order produces identical immutable plan and hash");
    check(first.plan->seats.size() == 2u && first.plan->seats[0].seatId == 1u &&
              first.plan->seats[1].seatId == 2u,
          "compiled Seat order is canonical");
    check(first.plan->seats[0].launchRequest.accountRef == "account-1" &&
              first.plan->seats[1].launchRequest.accountRef == "account-2",
          "Player provider account references resolve into typed launch requests");
}

void testSameGameRequiresAndPinsSetup() {
    FakeProvider fake;
    const std::vector<ProviderAdapterBinding> providers{{"fake", &fake}};
    auto seatDocument = seats();
    auto playerDocument = players();
    auto gameDocument = games();
    gameDocument.games.resize(1u);
    auto requirementSet = requirements();
    requirementSet.resize(1u);

    profile::RuntimeSessionSelection missing;
    missing.bindings = {
        {1u, "player-1", "game:a", std::nullopt, 0u},
        {2u, "player-2", "game:a", std::nullopt, 1u},
    };
    profile::TwoPlayerSetupDocument noSetups;
    auto rejected = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, gameDocument, noSetups, missing, providers,
        requirementSet);
    check(!rejected.succeeded(), "same-game selection without setup fails closed");

    profile::TwoPlayerSetup setup;
    setup.setupId = "setup-a";
    setup.gameId = "game:a";
    setup.displayName = L"Two players";
    setup.compatibility = compatibility();
    setup.instances = {
        {{L"--seat=1"}, L"C:\\Games\\A1", L"C:\\Data\\A1"},
        {{L"--seat=2"}, L"C:\\Games\\A2", L"C:\\Data\\A2"},
    };
    profile::TwoPlayerSetupDocument setupDocument;
    setupDocument.setups = {setup};
    profile::RuntimeSessionSelection selected;
    selected.bindings = {
        {1u, "player-1", "game:a", "setup-a", 0u},
        {2u, "player-2", "game:a", "setup-a", 1u},
    };
    const auto compiled = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, gameDocument, setupDocument, selected, providers,
        requirementSet);
    check(compiled.succeeded(), "same-game selection with exact setup compiles");
    check(compiled.plan->seats[0].instanceRecipe->arguments ==
              std::vector<std::wstring>{L"--seat=1"} &&
              compiled.plan->seats[1].instanceRecipe->arguments ==
              std::vector<std::wstring>{L"--seat=2"},
          "instance recipes are pinned per Seat");
}

void testValidationSeatScopeIsPerGame() {
    FakeProvider fake;
    const std::vector<ProviderAdapterBinding> providers{{"fake", &fake}};
    const auto seatDocument = seats();
    const auto playerDocument = players();
    const auto gameDocument = games();
    const profile::TwoPlayerSetupDocument noSetups;

    auto independentlyValidated = requirements();
    independentlyValidated[0].validatedSeatCount = 1u;
    independentlyValidated[1].validatedSeatCount = 1u;
    const auto differentGames = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, gameDocument, noSetups,
        twoDifferentGames(), providers, independentlyValidated);
    check(differentGames.succeeded(),
          "two different Games may each use independent one-Seat physical validation authority");

    auto singleGameDocument = gameDocument;
    singleGameDocument.games.resize(1u);
    auto singleGameRequirement = requirements();
    singleGameRequirement.resize(1u);
    singleGameRequirement.front().validatedSeatCount = 1u;

    profile::TwoPlayerSetup setup;
    setup.setupId = "setup-scope";
    setup.gameId = "game:a";
    setup.displayName = L"Two players";
    setup.compatibility = compatibility();
    setup.instances = {
        {{L"--seat=1"}, L"C:\\Games\\A1", L"C:\\Data\\A1"},
        {{L"--seat=2"}, L"C:\\Games\\A2", L"C:\\Data\\A2"},
    };
    profile::TwoPlayerSetupDocument setupDocument;
    setupDocument.setups = {setup};
    profile::RuntimeSessionSelection sameGame;
    sameGame.bindings = {
        {1u, "player-1", "game:a", "setup-scope", 0u},
        {2u, "player-2", "game:a", "setup-scope", 1u},
    };

    const auto rejected = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, singleGameDocument, setupDocument,
        sameGame, providers, singleGameRequirement);
    check(!rejected.succeeded() && !rejected.issues.empty() &&
              rejected.issues.front().code == PlanIssueCode::ValidationSeatScopeExceeded,
          "one-Seat validation authority cannot compile a two-Seat same-Game plan");

    singleGameRequirement.front().validatedSeatCount = 2u;
    const auto accepted = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, singleGameDocument, setupDocument,
        sameGame, providers, singleGameRequirement);
    check(accepted.succeeded(),
          "two-Seat validation authority permits the same-Game two-Seat plan when all other checks pass");
}

void testMaterialStalenessAndHardwareBlockPlan() {
    FakeProvider fake;
    const std::vector<ProviderAdapterBinding> providers{{"fake", &fake}};
    auto seatDocument = seats();
    const auto playerDocument = players();
    const auto gameDocument = games();
    const profile::TwoPlayerSetupDocument setupDocument;
    auto requirementSet = requirements();

    requirementSet[0].compatibility = compatibility("different");
    auto stale = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, gameDocument, setupDocument,
        twoDifferentGames(), providers, requirementSet);
    check(!stale.succeeded() && stale.issues.front().code == PlanIssueCode::StaleCompatibility,
          "stale compatibility evidence prevents plan creation");

    requirementSet = requirements();
    requirementSet[0].requirements.controller = true;
    seatDocument.seats[0].controllerIds.clear();
    auto missingController = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, gameDocument, setupDocument,
        twoDifferentGames(), providers, requirementSet);
    check(!missingController.succeeded() &&
              missingController.issues.front().code == PlanIssueCode::MissingController,
          "requirement-aware hardware preflight fails before activation");

    requirementSet = requirements();
    seatDocument = seats();
    seatDocument.seats[1].displayIds = seatDocument.seats[0].displayIds;
    seatDocument.seats[1].primaryDisplayId = seatDocument.seats[0].primaryDisplayId;
    auto duplicate = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, gameDocument, setupDocument,
        twoDifferentGames(), providers, requirementSet);
    check(!duplicate.succeeded() &&
              duplicate.issues.front().code == PlanIssueCode::DuplicateExclusiveHardware,
          "cross-Seat exclusive hardware collision fails closed");
}

void testProviderAndAccountAmbiguityFailClosed() {
    FakeProvider fake;
    const std::vector<ProviderAdapterBinding> providers{{"fake", &fake}};
    const auto seatDocument = seats();
    auto playerDocument = players();
    const auto gameDocument = games();
    const profile::TwoPlayerSetupDocument setupDocument;
    const auto requirementSet = requirements();

    fake.descriptorValue.availability = ProviderAvailability::Offline;
    auto offline = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, gameDocument, setupDocument,
        twoDifferentGames(), providers, requirementSet);
    check(!offline.succeeded() &&
              offline.issues.front().code == PlanIssueCode::ProviderUnavailable,
          "offline provider cannot silently change the launch path");

    fake.descriptorValue.availability = ProviderAvailability::Available;
    playerDocument.players[0].providerAccounts.push_back({"fake", "account-other"});
    auto ambiguous = compileProviderAwareLaunchPlan(
        seatDocument, playerDocument, gameDocument, setupDocument,
        twoDifferentGames(), providers, requirementSet);
    check(!ambiguous.succeeded() &&
              ambiguous.issues.front().code == PlanIssueCode::InvalidPlayerDocument,
          "duplicate provider account references fail closed at the stable Player schema boundary");
}

void testApplicationScopedProviderBindings() {
    FakeProvider first;
    FakeProvider second;
    const std::vector<ProviderAdapterBinding> providers{
        {"fake", &first, "100"},
        {"fake", &second, "200"},
    };
    const auto compiled = compileProviderAwareLaunchPlan(
        seats(), players(), games(), {}, twoDifferentGames(), providers,
        requirements());
    check(compiled.succeeded(),
          "application-scoped adapters allow multiple definitions from one provider");

    const std::vector<ProviderAdapterBinding> ambiguous{
        {"fake", &first, "100"},
        {"fake", &second, "100"},
    };
    const auto rejected = compileProviderAwareLaunchPlan(
        seats(), players(), games(), {}, twoDifferentGames(), ambiguous,
        requirements());
    check(!rejected.succeeded() &&
              rejected.issues.front().code == PlanIssueCode::DuplicateProvider,
          "duplicate application-scoped adapters fail closed");

    const std::vector<ProviderAdapterBinding> nullScoped{
        {"fake", &first},
        {"fake", nullptr, "100"},
    };
    const auto nullRejected = compileProviderAwareLaunchPlan(
        seats(), players(), games(), {}, twoDifferentGames(), nullScoped,
        requirements());
    check(!nullRejected.succeeded() &&
              nullRejected.issues.front().code == PlanIssueCode::MissingProvider,
          "a null exact binding cannot fall back to a provider-wide adapter");
}

namespace fs = std::filesystem;
namespace mat = hydra::materialization;

struct MaterializationFixture {
    fs::path root;
    fs::path sourceRoot;
    fs::path instancesRoot;
    fs::path executable;

    MaterializationFixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() /
               fs::path("hydraseat-materialization-" + std::to_string(nonce));
        sourceRoot = root / "source";
        instancesRoot = root / "instances";
        executable = sourceRoot / "game.exe";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(sourceRoot / "defaults", ec);
        fs::create_directories(instancesRoot, ec);
        write(executable, "fixture executable\n");
        write(sourceRoot / "defaults" / "settings.ini", "setting=shared\n");
        write(sourceRoot / "defaults" / "profile.dat", "profile=shared\n");
        write(sourceRoot / "defaults" / "window.ini", "window=shared\n");
        write(sourceRoot / "defaults" / "runtime.ini", "runtime=shared\n");
    }

    ~MaterializationFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static void write(const fs::path& path, std::string_view text) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    static std::string read(const fs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>());
    }
};

profile::CompatibilityReference materialCompatibility() {
    return {"compat-material", "physical-fixture", 7u};
}

GameRuntimeRequirement materialRequirement() {
    GameRuntimeRequirement value;
    value.gameId = "game:material";
    value.revision = 21u;
    value.validatedSeatCount = 2u;
    value.compatibility = materialCompatibility();
    return value;
}

ProviderAwareLaunchPlan materialProviderPlan(const MaterializationFixture& fixture) {
    ProviderAwareLaunchPlan value;
    const auto requirementValue = materialRequirement();
    for (std::uint32_t index = 0u; index < 2u; ++index) {
        SeatProviderLaunchPlan seat;
        seat.seatId = index + 1u;
        seat.playerId = index == 0u ? "player-1" : "player-2";
        seat.gameId = requirementValue.gameId;
        seat.setupId = "setup-material";
        seat.instanceIndex = index;
        seat.requirementRevision = requirementValue.revision;
        seat.compatibility = requirementValue.compatibility;
        seat.hardwareFingerprint = 100u + index;
        seat.requirements = requirementValue.requirements;
        seat.capabilities = requirementValue.capabilities;
        seat.launchRequest.providerId = "fake";
        seat.launchRequest.gameId = requirementValue.gameId;
        seat.launchRequest.providerAppId = "material-app";
        seat.launchRequest.metadataRevision = 41u;
        seat.launchRequest.targetKind = LaunchTargetKind::Executable;
        seat.launchRequest.target = fixture.executable.wstring();
        seat.launchRequest.launchCorrelationId = "material-" + std::to_string(index + 1u);
        value.seats.push_back(std::move(seat));
    }
    value.fingerprint = recomputeProviderAwareLaunchPlanFingerprint(value);
    return value;
}

requirement::TrustedRequirementSnapshot materialTrustedSnapshot(
    const MaterializationFixture& fixture) {
    requirement::TrustedRequirementSnapshot snapshot;
    snapshot.referenceMonth = "2026-08";
    snapshot.staleAfterMonths = 1u;
    snapshot.trust = requirement::LocalEvidenceTrust::PhysicalOnly;
    requirement::TrustedGameRuntimeAuthority authority;
    authority.requirement = materialRequirement();
    authority.providerId = "fake";
    authority.providerAppId = "material-app";
    authority.providerMetadataRevision = 41u;
    authority.gameVersionUtf8 = "1.0.0";
    authority.executableSha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    authority.executableCandidates = {fixture.executable.wstring()};
    authority.evidenceResultId = "result-material";
    authority.evidenceProvenanceId = "local-physical";
    authority.evidenceProvenanceRevision = 9u;
    authority.evidenceTimestampBucket = "2026-08";
    authority.evidenceOrigin = compat::ResultOrigin::Physical;
    snapshot.authorities = {authority};
    snapshot.requirements = {authority.requirement};
    return snapshot;
}

mat::CompatibilityRecipe materialRecipe(SeatId seatId) {
    mat::CompatibilityRecipe recipe;
    recipe.seatId = seatId;
    recipe.gameId = "game:material";
    recipe.providerId = "fake";
    recipe.providerAppId = "material-app";
    recipe.providerMetadataRevision = 41u;
    recipe.requirementRevision = 21u;
    recipe.compatibility = materialCompatibility();
    recipe.steps = {
        {"runtime-files", setup::RecipeExecutionPhase::Runtime,
         setup::MutationScope::SeatWritableInstance,
         {{L"defaults/runtime.ini", L"runtime/runtime.ini", 1024u}}},
        {"pre-spawn-files", setup::RecipeExecutionPhase::PreSpawn,
         setup::MutationScope::SeatWritableInstance,
         {{L"defaults/settings.ini", L"config/settings.ini", 1024u}}},
        {"post-window-files", setup::RecipeExecutionPhase::PostWindow,
         setup::MutationScope::SeatWritableInstance,
         {{L"defaults/window.ini", L"window/window.ini", 1024u}}},
        {"startup-files", setup::RecipeExecutionPhase::Startup,
         setup::MutationScope::SeatWritableInstance,
         {{L"defaults/profile.dat", L"state/profile.dat", 1024u}}},
    };
    return recipe;
}

mat::MaterializationContext materialContext(const MaterializationFixture& fixture,
                                            std::string sessionId) {
    return {fixture.instancesRoot, std::move(sessionId)};
}

bool executeAllPhases(mat::RecipeExecutionSession& session) {
    return session.executePhase(setup::RecipeExecutionPhase::PreSpawn).succeeded() &&
           session.executePhase(setup::RecipeExecutionPhase::Startup).succeeded() &&
           session.executePhase(setup::RecipeExecutionPhase::PostWindow).succeeded() &&
           session.executePhase(setup::RecipeExecutionPhase::Runtime).succeeded();
}

void testCompatibilityRecipeCompilerIsExactAndDeterministic() {
    MaterializationFixture fixture;
    const auto providerPlan = materialProviderPlan(fixture);
    const auto trusted = materialTrustedSnapshot(fixture);
    const auto context = materialContext(fixture, "session-deterministic");
    const auto recipe = materialRecipe(1u);

    mat::InstanceMaterializationPlan first;
    mat::InstanceMaterializationPlan second;
    check(mat::compileInstanceMaterializationPlan(
              recipe, providerPlan, trusted, context, first).succeeded() &&
              mat::compileInstanceMaterializationPlan(
                  recipe, providerPlan, trusted, context, second).succeeded(),
          "exact trusted recipe compiles deterministically");
    check(first == second && first.recipeFingerprint == second.recipeFingerprint,
          "same recipe and exact authority produce the same materialization plan");
    check(first.steps.size() == 4u &&
              first.steps[0].phase == setup::RecipeExecutionPhase::PreSpawn &&
              first.steps[1].phase == setup::RecipeExecutionPhase::Startup &&
              first.steps[2].phase == setup::RecipeExecutionPhase::PostWindow &&
              first.steps[3].phase == setup::RecipeExecutionPhase::Runtime,
          "recipe compiler canonicalizes deterministic phase ordering");

    auto invalidPhase = recipe;
    invalidPhase.steps.front().phase = static_cast<setup::RecipeExecutionPhase>(99u);
    mat::InstanceMaterializationPlan sentinel = first;
    check(mat::compileInstanceMaterializationPlan(
              invalidPhase, providerPlan, trusted, context, sentinel).code ==
              mat::RecipeResult::UnsupportedPhase && sentinel == first,
          "invalid timing phase is rejected without replacing prior output");

    auto wrongGame = recipe;
    wrongGame.gameId = "game:other";
    check(mat::compileInstanceMaterializationPlan(
              wrongGame, providerPlan, trusted, context, sentinel).code ==
              mat::RecipeResult::WrongGameIdentity,
          "recipe for the wrong Game identity is rejected");

    auto staleProvider = recipe;
    staleProvider.providerMetadataRevision = 40u;
    check(mat::compileInstanceMaterializationPlan(
              staleProvider, providerPlan, trusted, context, sentinel).code ==
              mat::RecipeResult::StaleProviderRevision,
          "recipe for the wrong provider revision is rejected");

    auto staleRequirement = recipe;
    staleRequirement.requirementRevision = 20u;
    check(mat::compileInstanceMaterializationPlan(
              staleRequirement, providerPlan, trusted, context, sentinel).code ==
              mat::RecipeResult::StaleRequirementRevision,
          "recipe for the wrong requirement revision is rejected");

    auto conflict = recipe;
    conflict.steps[1].files.push_back(
        {L"defaults/profile.dat", L"config/settings.ini", 1024u});
    check(mat::compileInstanceMaterializationPlan(
              conflict, providerPlan, trusted, context, sentinel).code ==
              mat::RecipeResult::ConflictingMutation,
          "duplicate/conflicting writable destination is rejected");

    auto sharedMutation = recipe;
    sharedMutation.steps.front().scope = setup::MutationScope::SharedInstallation;
    check(mat::compileInstanceMaterializationPlan(
              sharedMutation, providerPlan, trusted, context, sentinel).code ==
              mat::RecipeResult::SharedInstallationMutationDenied,
          "shared-install destructive mutation remains fail-closed without a safe contract");

    mat::InstanceMaterializationPlan seatTwo;
    check(mat::compileInstanceMaterializationPlan(
              materialRecipe(2u), providerPlan, trusted, context, seatTwo).succeeded() &&
              seatTwo.instanceRoot != first.instanceRoot,
          "Seat A and Seat B receive distinct deterministic writable destinations");
}

void testWritableMaterializationPreservesSharedSourceAndCleansIdempotently() {
    MaterializationFixture fixture;
    const auto providerPlan = materialProviderPlan(fixture);
    const auto trusted = materialTrustedSnapshot(fixture);
    mat::InstanceMaterializationPlan plan;
    check(mat::compileInstanceMaterializationPlan(
              materialRecipe(1u), providerPlan, trusted,
              materialContext(fixture, "session-source-safe"), plan).succeeded(),
          "source-safe materialization fixture compiles");

    const auto settingsBefore = MaterializationFixture::read(
        fixture.sourceRoot / "defaults" / "settings.ini");
    const auto profileBefore = MaterializationFixture::read(
        fixture.sourceRoot / "defaults" / "profile.dat");
    const auto windowBefore = MaterializationFixture::read(
        fixture.sourceRoot / "defaults" / "window.ini");
    const auto runtimeBefore = MaterializationFixture::read(
        fixture.sourceRoot / "defaults" / "runtime.ini");

    mat::RecipeExecutionSession session(plan);
    check(executeAllPhases(session) && session.finalized(),
          "all bounded compatibility phases materialize and finalize transactionally");
    mat::InstanceState state = mat::InstanceState::Unsafe;
    check(mat::inspectInstanceMaterialization(plan, state).succeeded() &&
              state == mat::InstanceState::Current,
          "fully executed recipe has a current exact ownership manifest");
    check(MaterializationFixture::read(plan.instanceRoot / "config" / "settings.ini") ==
              settingsBefore &&
              MaterializationFixture::read(plan.instanceRoot / "state" / "profile.dat") ==
                  profileBefore &&
              MaterializationFixture::read(plan.instanceRoot / "window" / "window.ini") ==
                  windowBefore &&
              MaterializationFixture::read(plan.instanceRoot / "runtime" / "runtime.ini") ==
                  runtimeBefore,
          "only declared mutable files appear in the Seat-specific writable instance");
    check(MaterializationFixture::read(fixture.sourceRoot / "defaults" / "settings.ini") ==
              settingsBefore &&
              MaterializationFixture::read(fixture.sourceRoot / "defaults" / "profile.dat") ==
                  profileBefore &&
              MaterializationFixture::read(fixture.sourceRoot / "defaults" / "window.ini") ==
                  windowBefore &&
              MaterializationFixture::read(fixture.sourceRoot / "defaults" / "runtime.ini") ==
                  runtimeBefore,
          "shared immutable installation source remains byte-for-byte unchanged");

    check(mat::cleanupInstanceMaterialization(plan).succeeded() &&
              mat::cleanupInstanceMaterialization(plan).succeeded() &&
              !fs::exists(plan.instanceRoot),
          "owned instance cleanup is idempotent");
}

void testStagingAndCommitFailuresRollbackSafely() {
    MaterializationFixture fixture;
    const auto providerPlan = materialProviderPlan(fixture);
    const auto trusted = materialTrustedSnapshot(fixture);

    mat::InstanceMaterializationPlan stagingPlan;
    mat::compileInstanceMaterializationPlan(
        materialRecipe(1u), providerPlan, trusted,
        materialContext(fixture, "session-stage-fail"), stagingPlan);
    mat::RecipeExecutionSession stagingFailure(
        stagingPlan,
        [](mat::TransactionCheckpoint checkpoint, std::string& reason) {
            if (checkpoint == mat::TransactionCheckpoint::StagingValidated) {
                reason = "injected staging veto";
                return false;
            }
            return true;
        });
    check(stagingFailure.executePhase(setup::RecipeExecutionPhase::PreSpawn).code ==
              mat::RecipeResult::StagingFailed &&
              !fs::exists(stagingPlan.instanceRoot) &&
              !fs::exists(stagingPlan.stagingRoot),
          "staging failure removes partial staging and leaves no committed instance");

    const auto oldRecipe = materialRecipe(1u);
    mat::InstanceMaterializationPlan oldPlan;
    mat::compileInstanceMaterializationPlan(
        oldRecipe, providerPlan, trusted,
        materialContext(fixture, "session-commit-fail"), oldPlan);
    mat::RecipeExecutionSession oldSession(oldPlan);
    check(executeAllPhases(oldSession), "previous committed instance fixture is created");
    const auto oldPayload = MaterializationFixture::read(
        oldPlan.instanceRoot / "config" / "settings.ini");

    auto replacementRecipe = oldRecipe;
    replacementRecipe.steps[1].files[0].maximumBytes = 2048u;
    mat::InstanceMaterializationPlan replacementPlan;
    check(mat::compileInstanceMaterializationPlan(
              replacementRecipe, providerPlan, trusted,
              materialContext(fixture, "session-commit-fail"), replacementPlan).succeeded() &&
              replacementPlan.instanceRoot == oldPlan.instanceRoot &&
              replacementPlan.recipeFingerprint != oldPlan.recipeFingerprint,
          "replacement recipe targets the same Seat/session root but has a new exact recipe revision");

    mat::RecipeExecutionSession commitFailure(
        replacementPlan,
        [](mat::TransactionCheckpoint checkpoint, std::string& reason) {
            if (checkpoint == mat::TransactionCheckpoint::PreviousInstanceMoved) {
                reason = "injected commit veto";
                return false;
            }
            return true;
        });
    check(commitFailure.executePhase(setup::RecipeExecutionPhase::PreSpawn).code ==
              mat::RecipeResult::CommitFailed,
          "commit failure is surfaced at the atomic previous-instance handoff");
    mat::InstanceState oldState = mat::InstanceState::Unsafe;
    check(mat::inspectInstanceMaterialization(oldPlan, oldState).succeeded() &&
              oldState == mat::InstanceState::Current &&
              MaterializationFixture::read(oldPlan.instanceRoot / "config" / "settings.ini") ==
                  oldPayload,
          "commit failure preserves the previous committed instance exactly");

    mat::InstanceState staleState = mat::InstanceState::Unsafe;
    check(mat::inspectInstanceMaterialization(replacementPlan, staleState).code ==
              mat::RecipeResult::StaleInstance &&
              staleState == mat::InstanceState::Stale,
          "stale per-instance materialization is never reused as current");
    check(mat::cleanupInstanceMaterialization(replacementPlan).succeeded() &&
              mat::cleanupInstanceMaterialization(replacementPlan).succeeded() &&
              !fs::exists(replacementPlan.instanceRoot),
          "stale owned instance can be cleaned safely and cleanup remains idempotent");
}

void testPathAndReparseEscapesFailClosed() {
    MaterializationFixture fixture;
    const auto providerPlan = materialProviderPlan(fixture);
    const auto trusted = materialTrustedSnapshot(fixture);
    auto traversal = materialRecipe(1u);
    traversal.steps[1].files[0].sourceRelativePath = L"../secret.ini";
    mat::InstanceMaterializationPlan output;
    check(mat::compileInstanceMaterializationPlan(
              traversal, providerPlan, trusted,
              materialContext(fixture, "session-traversal"), output).code ==
              mat::RecipeResult::InvalidPath,
          "path traversal in mutable source is rejected before filesystem mutation");

    auto reservedDevice = materialRecipe(1u);
    reservedDevice.steps[1].files[0].destinationRelativePath = L"config/NUL.txt";
    check(mat::compileInstanceMaterializationPlan(
              reservedDevice, providerPlan, trusted,
              materialContext(fixture, "session-reserved-device"), output).code ==
              mat::RecipeResult::InvalidPath,
          "Windows reserved device paths are rejected before materialization");

    const auto outside = fixture.root / "outside";
    std::error_code ec;
    fs::create_directories(outside, ec);
    MaterializationFixture::write(outside / "secret.ini", "outside-secret\n");
    const auto link = fixture.sourceRoot / "defaults" / "escape-link";
    fs::create_directory_symlink(outside, link, ec);
    if (!ec) {
        auto symlinkRecipe = materialRecipe(1u);
        symlinkRecipe.steps = {
            {"pre-spawn-link", setup::RecipeExecutionPhase::PreSpawn,
             setup::MutationScope::SeatWritableInstance,
             {{L"defaults/escape-link/secret.ini", L"config/secret.ini", 1024u}}},
        };
        mat::InstanceMaterializationPlan symlinkPlan;
        check(mat::compileInstanceMaterializationPlan(
                  symlinkRecipe, providerPlan, trusted,
                  materialContext(fixture, "session-reparse"), symlinkPlan).succeeded(),
              "reparse fixture compiles because compilation remains filesystem-pure");
        mat::RecipeExecutionSession session(symlinkPlan);
        check(session.executePhase(setup::RecipeExecutionPhase::PreSpawn).code ==
                  mat::RecipeResult::ReparsePointRejected &&
                  !fs::exists(symlinkPlan.instanceRoot),
              "symlink/junction escape is rejected before copying outside the trusted source root");

        mat::InstanceMaterializationPlan cleanupPlan;
        check(mat::compileInstanceMaterializationPlan(
                  materialRecipe(1u), providerPlan, trusted,
                  materialContext(fixture, "session-reparse-cleanup"), cleanupPlan).succeeded(),
              "reparse cleanup fixture compiles from exact trusted authority");
        mat::RecipeExecutionSession cleanupSession(cleanupPlan);
        check(executeAllPhases(cleanupSession),
              "reparse cleanup fixture reaches an owned committed instance");
        const auto injectedLink = cleanupPlan.instanceRoot / "tampered-link";
        std::error_code injectedEc;
        fs::create_directory_symlink(outside, injectedLink, injectedEc);
        if (!injectedEc) {
            check(mat::cleanupInstanceMaterialization(cleanupPlan).code ==
                      mat::RecipeResult::ReparsePointRejected &&
                      fs::exists(outside / "secret.ini"),
                  "owned cleanup refuses a tampered reparse descendant and preserves outside data");
            fs::remove(injectedLink, injectedEc);
            check(mat::cleanupInstanceMaterialization(cleanupPlan).succeeded(),
                  "owned cleanup succeeds after the injected reparse descendant is removed");
        }
    } else {
        std::cout << "symlink/junction escape fixture unavailable on this host; runtime check retained\n";
    }
}

void testPhaseFailureReversesEarlierWorkAndRuntimeCannotJumpAhead() {
    MaterializationFixture fixture;
    const auto providerPlan = materialProviderPlan(fixture);
    const auto trusted = materialTrustedSnapshot(fixture);
    mat::InstanceMaterializationPlan plan;
    mat::compileInstanceMaterializationPlan(
        materialRecipe(1u), providerPlan, trusted,
        materialContext(fixture, "session-partial-rollback"), plan);

    int stagedCount = 0;
    mat::RecipeExecutionSession session(
        plan,
        [&stagedCount](mat::TransactionCheckpoint checkpoint, std::string& reason) {
            if (checkpoint != mat::TransactionCheckpoint::StagingValidated) return true;
            ++stagedCount;
            if (stagedCount == 2) {
                reason = "injected Startup staging failure";
                return false;
            }
            return true;
        });
    check(session.executePhase(setup::RecipeExecutionPhase::PreSpawn).succeeded() &&
              fs::exists(plan.instanceRoot),
          "PreSpawn materialization commits before later recipe phases");
    check(session.executePhase(setup::RecipeExecutionPhase::Startup).code ==
              mat::RecipeResult::StagingFailed &&
              !fs::exists(plan.instanceRoot) &&
              !fs::exists(plan.stagingRoot) &&
              !fs::exists(plan.rollbackRoot),
          "partial recipe failure reverses files already applied by earlier phases");

    mat::InstanceMaterializationPlan earlyPlan;
    mat::compileInstanceMaterializationPlan(
        materialRecipe(1u), providerPlan, trusted,
        materialContext(fixture, "session-runtime-early"), earlyPlan);
    mat::RecipeExecutionSession early(earlyPlan);
    check(early.executePhase(setup::RecipeExecutionPhase::Runtime).code ==
              mat::RecipeResult::WrongPhaseOrder &&
              !fs::exists(earlyPlan.instanceRoot),
          "Runtime phase cannot execute before required earlier phase barriers");
}

} // namespace

int main() {
    testDeterministicDifferentGamePlan();
    testSameGameRequiresAndPinsSetup();
    testValidationSeatScopeIsPerGame();
    testMaterialStalenessAndHardwareBlockPlan();
    testProviderAndAccountAmbiguityFailClosed();
    testApplicationScopedProviderBindings();
    testCompatibilityRecipeCompilerIsExactAndDeterministic();
    testWritableMaterializationPreservesSharedSourceAndCleansIdempotently();
    testStagingAndCommitFailuresRollbackSafely();
    testPathAndReparseEscapesFailClosed();
    testPhaseFailureReversesEarlierWorkAndRuntimeCannotJumpAhead();
    if (failures != 0) {
        std::cerr << failures << " provider launch plan test(s) failed\n";
        return 1;
    }
    std::cout << "provider launch plan tests passed\n";
    return 0;
}
