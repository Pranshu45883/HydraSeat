#include "hydra/production_compatibility_activation.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace hydra;
using namespace hydra::materialization;
using namespace hydra::production;
using hydra::launch::ResourceKind;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Fixture {
    fs::path root;
    fs::path sourceRoot;
    fs::path instancesRoot;

    Fixture() {
        static std::uint64_t serial = 0u;
        root = fs::temp_directory_path() /
               ("HydraSeatCompatActivation-" + std::to_string(++serial));
        sourceRoot = root / "source";
        instancesRoot = root / "instances";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(sourceRoot / "defaults", ec);
        fs::create_directories(instancesRoot, ec);
        write(sourceRoot / "defaults" / "pre.ini", "pre-v1\n");
        write(sourceRoot / "defaults" / "startup.ini", "startup-v1\n");
        write(sourceRoot / "defaults" / "window.ini", "window-v1\n");
        write(sourceRoot / "defaults" / "runtime.ini", "runtime-v1\n");
    }

    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static void write(const fs::path& path, std::string_view value) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    }

    static std::string read(const fs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
};

InstanceMaterializationPlan makePlan(Fixture& fixture,
                                     SeatId seatId,
                                     std::string sessionId,
                                     std::uint32_t instanceIndex = 0u) {
    InstanceMaterializationPlan plan;
    plan.seatId = seatId;
    plan.instanceIndex = instanceIndex;
    plan.gameId = "game:fixture";
    plan.providerId = "provider-fixture";
    plan.providerAppId = "app-fixture";
    plan.providerMetadataRevision = 41u;
    plan.requirementRevision = 21u;
    plan.setupId = "activation-test-setup";
    plan.localDecisionId = "local-materialization-decision";
    plan.localDecisionRevision = 5u;
    plan.sessionId = sessionId;
    plan.sessionGeneration = 17u;
    plan.seatGameGeneration = 100u + seatId;
    plan.activationFingerprint = 0x9000000000000000ull + seatId;
    plan.providerPlanFingerprint = 0x1122334455667788ull + seatId;
    plan.sourceIdentityFingerprint = 0x8877665544332211ull + seatId;
    plan.recipeFingerprint = 0x1234000000000000ull + seatId;
    plan.sourceRoot = fixture.sourceRoot;

    CompatibilityActivationIdentity fingerprintInput;
    fingerprintInput.seatId = seatId;
    fingerprintInput.sessionId = std::move(sessionId);
    fingerprintInput.instanceIndex = instanceIndex;
    fingerprintInput.gameId = plan.gameId;
    fingerprintInput.providerId = plan.providerId;
    fingerprintInput.providerAppId = plan.providerAppId;
    plan.instanceIdentityFingerprint =
        compatibilityInstanceIdentityFingerprint(fingerprintInput);

    const auto suffix = std::to_string(seatId) + "-" +
                        std::to_string(plan.instanceIdentityFingerprint);
    plan.instanceRoot = fixture.instancesRoot / ("seat-instance-" + suffix);
    plan.stagingRoot = fs::path(plan.instanceRoot.wstring() + L".staging");
    plan.rollbackRoot = fs::path(plan.instanceRoot.wstring() + L".rollback");
    plan.previousPhaseRoot = fs::path(plan.instanceRoot.wstring() + L".previous");
    plan.steps = {
        {"pre", setup::RecipeExecutionPhase::PreSpawn,
         {{fixture.sourceRoot / "defaults" / "pre.ini", "config/pre.ini", 1024u}}},
        {"startup", setup::RecipeExecutionPhase::Startup,
         {{fixture.sourceRoot / "defaults" / "startup.ini", "config/startup.ini", 1024u}}},
        {"post-window", setup::RecipeExecutionPhase::PostWindow,
         {{fixture.sourceRoot / "defaults" / "window.ini", "config/window.ini", 1024u}}},
        {"runtime", setup::RecipeExecutionPhase::Runtime,
         {{fixture.sourceRoot / "defaults" / "runtime.ini", "config/runtime.ini", 1024u}}},
    };
    return plan;
}

CompatibilityActivationIdentity identityFor(const InstanceMaterializationPlan& plan,
                                              std::string sessionId) {
    return makeCompatibilityActivationIdentity(plan, std::move(sessionId));
}

launch::SeatActivationPlan launchPlanFor(const InstanceMaterializationPlan& plan) {
    launch::SeatActivationPlan launchPlan;
    launchPlan.seatId = plan.seatId;
    launchPlan.seat.seatId = plan.seatId;
    launchPlan.seat.active = true;
    launchPlan.target.gameId = plan.gameId;
    launchPlan.target.process.seatId = plan.seatId;
    launchPlan.target.process.executablePath = L"C:\\HydraSeat\\fixture.exe";
    launchPlan.resources = {
        ResourceKind::Recovery,
        ResourceKind::Process,
        ResourceKind::Window,
        ResourceKind::Display,
        ResourceKind::Input,
        ResourceKind::Controller,
        ResourceKind::Audio,
    };
    launchPlan.fingerprint = 0x9000000000000000ull + plan.seatId;
    return launchPlan;
}

runtime::SeatGameBinding bindingFor(const InstanceMaterializationPlan& plan) {
    return {"player-fixture", plan.gameId};
}

plan::ProviderAwareLaunchPlan providerPlanFor(
    const InstanceMaterializationPlan& materializationPlan,
    std::uint64_t metadataRevision) {
    plan::ProviderAwareLaunchPlan providerPlan;
    plan::SeatProviderLaunchPlan currentSeat;
    currentSeat.seatId = materializationPlan.seatId;
    currentSeat.playerId = "player-fixture";
    currentSeat.gameId = materializationPlan.gameId;
    currentSeat.setupId = materializationPlan.setupId;
    currentSeat.instanceIndex = materializationPlan.instanceIndex;
    currentSeat.requirementRevision = materializationPlan.requirementRevision;
    currentSeat.launchRequest.providerId = materializationPlan.providerId;
    currentSeat.launchRequest.gameId = materializationPlan.gameId;
    currentSeat.launchRequest.providerAppId = materializationPlan.providerAppId;
    currentSeat.launchRequest.metadataRevision = metadataRevision;
    currentSeat.launchRequest.targetKind = provider::LaunchTargetKind::Executable;
    currentSeat.launchRequest.target = L"C:\\HydraSeat\\fixture.exe";
    currentSeat.launchRequest.launchCorrelationId = "fixture-current";

    plan::SeatProviderLaunchPlan otherSeat = currentSeat;
    otherSeat.seatId = materializationPlan.seatId == 1u ? 2u : 1u;
    otherSeat.playerId = "player-other";
    otherSeat.gameId = "game:other-seat";
    otherSeat.instanceIndex = materializationPlan.instanceIndex == 0u ? 1u : 0u;
    otherSeat.launchRequest.gameId = otherSeat.gameId;
    otherSeat.launchRequest.launchCorrelationId = "fixture-other";

    providerPlan.seats = {currentSeat, otherSeat};
    providerPlan.fingerprint = plan::recomputeProviderAwareLaunchPlanFingerprint(providerPlan);
    return providerPlan;
}

bool prepareAdapter(ProductionCompatibilityActivation& adapter,
                    const InstanceMaterializationPlan& plan,
                    std::string& error) {
    return adapter.prepare(launchPlanFor(plan), bindingFor(plan), error);
}

void testPhaseOrderRepeatedAndFinalizedExecution() {
    Fixture fixture;
    const std::string sessionId = "session-phase-order";
    const auto plan = makePlan(fixture, 1u, sessionId);
    ProductionCompatibilityActivation adapter(plan, identityFor(plan, sessionId));
    std::string error;

    check(prepareAdapter(adapter, plan, error),
          "exact adapter identity prepares without mutation");
    check(!adapter.startup(error) &&
              adapter.diagnostic().code == CompatibilityActivationCode::WrongPhaseOrder &&
              !fs::exists(plan.instanceRoot),
          "skipped Startup is rejected before new materialization");
    check(adapter.preSpawn(error), "PreSpawn executes at the first lifecycle boundary");
    check(adapter.preSpawn(error) &&
              adapter.diagnostic().code == CompatibilityActivationCode::AlreadySatisfied,
          "repeated successful PreSpawn is idempotent without replaying mutation");
    check(adapter.startup(error) && adapter.postWindow(error) && adapter.runtime(error) &&
              adapter.finalized(),
          "Startup/PostWindow/Runtime advance exactly and finalize the transaction");
    check(!adapter.runtime(error) &&
              adapter.diagnostic().code == CompatibilityActivationCode::AlreadyFinalized,
          "duplicate Runtime after finalization is rejected");
    check(!prepareAdapter(adapter, plan, error),
          "finalized compatibility lifecycle cannot be rebound for duplicate execution");
    check(Fixture::read(fixture.sourceRoot / "defaults" / "pre.ini") == "pre-v1\n",
          "compatibility activation never mutates the original install source");
    check(adapter.rollback(error) && adapter.verifySafe(error) &&
              !fs::exists(plan.instanceRoot),
          "finalized Seat writable instance is cleaned and verified safe on lifecycle stop");
}

void testFreshProviderAuthorityBindingRejectsStaleMaterialization() {
    Fixture fixture;
    const std::string sessionId = "session-fresh-provider-bind";
    auto materializationPlan = makePlan(fixture, 1u, sessionId);
    auto currentProviderPlan = providerPlanFor(
        materializationPlan, materializationPlan.providerMetadataRevision);
    materializationPlan.providerPlanFingerprint = currentProviderPlan.fingerprint;

    CompatibilityActivationIdentity currentIdentity;
    const auto bound = bindCompatibilityActivationIdentity(
        materializationPlan, currentProviderPlan, sessionId, currentIdentity);
    ProductionCompatibilityActivation current(
        materializationPlan, currentIdentity);
    std::string error;
    check(bound.succeeded() && prepareAdapter(current, materializationPlan, error),
          "fresh trusted provider plan binds an exact activation identity");

    auto advancedProviderPlan = providerPlanFor(
        materializationPlan, materializationPlan.providerMetadataRevision + 1u);
    CompatibilityActivationIdentity advancedIdentity;
    const auto rebound = bindCompatibilityActivationIdentity(
        materializationPlan, advancedProviderPlan, sessionId, advancedIdentity);
    ProductionCompatibilityActivation staleMaterialization(
        materializationPlan, advancedIdentity);
    check(rebound.succeeded() &&
              !prepareAdapter(staleMaterialization, materializationPlan, error) &&
              staleMaterialization.diagnostic().code ==
                  CompatibilityActivationCode::StaleProviderRevision &&
              !fs::exists(materializationPlan.instanceRoot),
          "fresh provider authority makes a prior materialization revision fail before PreSpawn");
}

void testExactIdentityRejectsStaleReuseBeforeMutation() {
    Fixture fixture;
    const std::string sessionId = "session-exact-identity";
    const auto plan = makePlan(fixture, 1u, sessionId);
    const auto launchPlan = launchPlanFor(plan);
    const auto binding = bindingFor(plan);
    std::string error;

    auto wrongSeatIdentity = identityFor(plan, sessionId);
    wrongSeatIdentity.seatId = 2u;
    ProductionCompatibilityActivation wrongSeat(plan, wrongSeatIdentity);
    check(!wrongSeat.prepare(launchPlan, binding, error) &&
              wrongSeat.diagnostic().code == CompatibilityActivationCode::WrongSeat,
          "wrong Seat identity fails before mutation");

    auto staleSessionIdentity = identityFor(plan, "session-other");
    ProductionCompatibilityActivation staleSession(plan, staleSessionIdentity);
    check(!staleSession.prepare(launchPlan, binding, error) &&
              staleSession.diagnostic().code == CompatibilityActivationCode::StaleSession,
          "stale session ID is detected by recomputing the session-bound instance identity");

    auto staleDecisionIdentity = identityFor(plan, sessionId);
    ++staleDecisionIdentity.localDecisionRevision;
    ProductionCompatibilityActivation staleDecision(plan, staleDecisionIdentity);
    check(!staleDecision.prepare(launchPlan, binding, error) &&
              staleDecision.diagnostic().code ==
                  CompatibilityActivationCode::StaleSetupDecision,
          "stale local setup/materialization decision fails before mutation");

    auto staleSessionGenerationIdentity = identityFor(plan, sessionId);
    ++staleSessionGenerationIdentity.sessionGeneration;
    ProductionCompatibilityActivation staleSessionGeneration(
        plan, staleSessionGenerationIdentity);
    check(!staleSessionGeneration.prepare(launchPlan, binding, error) &&
              staleSessionGeneration.diagnostic().code ==
                  CompatibilityActivationCode::StaleSession,
          "stale Host session generation fails before mutation");

    auto staleSeatGameGenerationIdentity = identityFor(plan, sessionId);
    ++staleSeatGameGenerationIdentity.seatGameGeneration;
    ProductionCompatibilityActivation staleSeatGameGeneration(
        plan, staleSeatGameGenerationIdentity);
    check(!staleSeatGameGeneration.prepare(launchPlan, binding, error) &&
              staleSeatGameGeneration.diagnostic().code ==
                  CompatibilityActivationCode::StaleSeatGameGeneration,
          "stale Seat-game generation fails before mutation");

    auto staleActivationIdentity = identityFor(plan, sessionId);
    ++staleActivationIdentity.activationFingerprint;
    ProductionCompatibilityActivation staleActivation(plan, staleActivationIdentity);
    check(!staleActivation.prepare(launchPlan, binding, error) &&
              staleActivation.diagnostic().code ==
                  CompatibilityActivationCode::StaleActivationPlan,
          "stale immutable activation fingerprint fails before mutation");

    auto staleProviderIdentity = identityFor(plan, sessionId);
    --staleProviderIdentity.providerMetadataRevision;
    ProductionCompatibilityActivation staleProvider(plan, staleProviderIdentity);
    check(!staleProvider.prepare(launchPlan, binding, error) &&
              staleProvider.diagnostic().code == CompatibilityActivationCode::StaleProviderRevision,
          "stale provider revision fails before mutation");

    auto staleRequirementIdentity = identityFor(plan, sessionId);
    --staleRequirementIdentity.requirementRevision;
    ProductionCompatibilityActivation staleRequirement(plan, staleRequirementIdentity);
    check(!staleRequirement.prepare(launchPlan, binding, error) &&
              staleRequirement.diagnostic().code == CompatibilityActivationCode::StaleRequirementRevision,
          "stale requirement revision fails before mutation");

    auto wrongGameIdentity = identityFor(plan, sessionId);
    wrongGameIdentity.gameId = "game:other";
    ProductionCompatibilityActivation wrongGame(plan, wrongGameIdentity);
    check(!wrongGame.prepare(launchPlan, binding, error) &&
              wrongGame.diagnostic().code == CompatibilityActivationCode::WrongGame,
          "wrong Game identity fails before mutation");

    auto stalePlanIdentity = identityFor(plan, sessionId);
    ++stalePlanIdentity.providerPlanFingerprint;
    ProductionCompatibilityActivation stalePlan(plan, stalePlanIdentity);
    check(!stalePlan.prepare(launchPlan, binding, error) &&
              stalePlan.diagnostic().code == CompatibilityActivationCode::StaleProviderPlan,
          "stale provider-plan fingerprint fails before mutation");

    auto staleSourceIdentity = identityFor(plan, sessionId);
    ++staleSourceIdentity.sourceIdentityFingerprint;
    ProductionCompatibilityActivation staleSource(plan, staleSourceIdentity);
    check(!staleSource.prepare(launchPlan, binding, error) &&
              staleSource.diagnostic().code == CompatibilityActivationCode::StaleSourceIdentity,
          "stale exact source identity fails before mutation");

    auto staleInstanceIdentity = identityFor(plan, sessionId);
    ++staleInstanceIdentity.instanceIdentityFingerprint;
    ProductionCompatibilityActivation staleInstance(plan, staleInstanceIdentity);
    check(!staleInstance.prepare(launchPlan, binding, error) &&
              staleInstance.diagnostic().code == CompatibilityActivationCode::StaleInstanceIdentity,
          "stale instance identity fingerprint fails before mutation");

    auto staleRecipeIdentity = identityFor(plan, sessionId);
    ++staleRecipeIdentity.recipeFingerprint;
    ProductionCompatibilityActivation staleRecipe(plan, staleRecipeIdentity);
    check(!staleRecipe.prepare(launchPlan, binding, error) &&
              staleRecipe.diagnostic().code == CompatibilityActivationCode::StaleRecipe,
          "stale recipe fingerprint fails before mutation");

    check(!fs::exists(plan.instanceRoot),
          "all stale/wrong identity rejections occur before product-owned instance mutation");
}

void testEachRecipePhaseFailureReversesToBaseline() {
    for (int failCheckpoint = 1; failCheckpoint <= 4; ++failCheckpoint) {
        Fixture fixture;
        const std::string sessionId = "session-phase-failure-" +
                                      std::to_string(failCheckpoint);
        const auto plan = makePlan(fixture, 1u, sessionId);
        int stagingValidated = 0;
        ProductionCompatibilityActivation adapter(
            plan, identityFor(plan, sessionId),
            [&](TransactionCheckpoint checkpoint, std::string& reason) {
                if (checkpoint != TransactionCheckpoint::StagingValidated) return true;
                ++stagingValidated;
                if (stagingValidated == failCheckpoint) {
                    reason = "injected lifecycle materialization phase failure";
                    return false;
                }
                return true;
            });
        std::string error;
        check(prepareAdapter(adapter, plan, error),
              "phase-failure adapter prepares exact identity");

        bool succeeded = adapter.preSpawn(error);
        if (succeeded) succeeded = adapter.startup(error);
        if (succeeded) succeeded = adapter.postWindow(error);
        if (succeeded) succeeded = adapter.runtime(error);
        check(!succeeded &&
                  adapter.diagnostic().code == CompatibilityActivationCode::MaterializationFailure,
              "PreSpawn/Startup/PostWindow/Runtime transaction failure is surfaced");
        check(!fs::exists(plan.instanceRoot) && !fs::exists(plan.stagingRoot) &&
                  !fs::exists(plan.rollbackRoot) && !fs::exists(plan.previousPhaseRoot),
              "failed recipe phase reverses all materialization paths to the missing baseline");
        check(adapter.rollback(error) && adapter.verifySafe(error),
              "post-failure lifecycle rollback remains idempotent and verified safe");
    }
}

void testInterruptedPreviousMaterializationIsRecovered() {
    Fixture fixture;
    const std::string sessionId = "session-interrupted";
    const auto plan = makePlan(fixture, 1u, sessionId);
    RecipeExecutionSession previous(plan);
    check(previous.executePhase(setup::RecipeExecutionPhase::PreSpawn).succeeded(),
          "interrupted fixture creates a partial owned instance");
    std::error_code ec;
    fs::rename(plan.instanceRoot, plan.previousPhaseRoot, ec);
    check(!ec, "interrupted fixture moves partial instance to previous-phase path");

    ProductionCompatibilityActivation adapter(plan, identityFor(plan, sessionId));
    std::string error;
    check(prepareAdapter(adapter, plan, error) && adapter.preSpawn(error),
          "new activation recovers interrupted previous materialization before PreSpawn");
    check(fs::exists(plan.instanceRoot) && !fs::exists(plan.previousPhaseRoot) &&
              !fs::exists(plan.stagingRoot),
          "interrupted debris is consumed and a fresh bounded PreSpawn instance is committed");
    check(adapter.rollback(error) && adapter.verifySafe(error) &&
              !fs::exists(plan.instanceRoot),
          "recovered activation still rolls back to the original missing baseline");
}

void testUnsafePreexistingInstanceRetainsRecoveryOwnership() {
    Fixture fixture;
    const std::string sessionId = "session-unsafe-baseline";
    const auto plan = makePlan(fixture, 1u, sessionId);
    std::error_code ec;
    fs::create_directories(plan.instanceRoot, ec);
    Fixture::write(plan.instanceRoot / "foreign.txt", "not-owned\n");

    ProductionCompatibilityActivation adapter(plan, identityFor(plan, sessionId));
    std::string error;
    check(prepareAdapter(adapter, plan, error),
          "unsafe-baseline fixture binds exact activation identity without mutation");
    check(!adapter.preSpawn(error) && adapter.recoveryRequired() &&
              adapter.diagnostic().code == CompatibilityActivationCode::RecoveryRequired,
          "foreign/unverifiable preexisting instance blocks PreSpawn as RecoveryRequired");
    check(!adapter.rollback(error) && adapter.recoveryRequired() &&
              fs::exists(plan.instanceRoot / "foreign.txt"),
          "generic rollback cannot erase RecoveryRequired when no verified baseline existed");
}

void testRollbackVerificationFailureRequiresRecovery() {
    Fixture fixture;
    const std::string sessionId = "session-recovery-required";
    const auto plan = makePlan(fixture, 1u, sessionId);
    ProductionCompatibilityActivation adapter(plan, identityFor(plan, sessionId));
    std::string error;
    check(prepareAdapter(adapter, plan, error) && adapter.preSpawn(error),
          "rollback verification fixture reaches PreSpawn");

    Fixture::write(plan.instanceRoot / ".hydraseat-instance-v1", "tampered\n");
    check(!adapter.rollback(error) && adapter.recoveryRequired() &&
              adapter.diagnostic().code == CompatibilityActivationCode::RecoveryRequired,
          "unverifiable rollback is never converted to ordinary success");
    check(!adapter.verifySafe(error),
          "RecoveryRequired compatibility ownership remains explicitly unsafe");
}

void testOneSeatRollbackDoesNotMutateOtherSeatInstance() {
    Fixture fixture;
    const std::string sessionOne = "session-seat-one";
    const std::string sessionTwo = "session-seat-two";
    const auto planOne = makePlan(fixture, 1u, sessionOne, 0u);
    const auto planTwo = makePlan(fixture, 2u, sessionTwo, 1u);
    ProductionCompatibilityActivation seatOne(planOne, identityFor(planOne, sessionOne));
    ProductionCompatibilityActivation seatTwo(planTwo, identityFor(planTwo, sessionTwo));
    std::string error;

    check(prepareAdapter(seatOne, planOne, error) && seatOne.preSpawn(error) &&
              seatOne.startup(error),
          "Seat 1 writable instance advances independently");
    check(prepareAdapter(seatTwo, planTwo, error) && seatTwo.preSpawn(error) &&
              seatTwo.startup(error),
          "Seat 2 writable instance advances independently");
    const auto seatOneBytes = Fixture::read(planOne.instanceRoot / "config" / "startup.ini");

    check(seatTwo.rollback(error) && seatTwo.verifySafe(error) &&
              !fs::exists(planTwo.instanceRoot),
          "Seat 2 rollback removes only its exact owned instance");
    check(fs::exists(planOne.instanceRoot) &&
              Fixture::read(planOne.instanceRoot / "config" / "startup.ini") == seatOneBytes,
          "Seat 2 rollback leaves Seat 1 writable instance byte-for-byte unchanged");
    check(seatOne.rollback(error) && seatOne.verifySafe(error),
          "Seat 1 remains independently rollback-capable");
}

void testMissingWindowLifecycleBoundaryFailsClosed() {
    Fixture fixture;
    const std::string sessionId = "session-missing-window";
    const auto plan = makePlan(fixture, 1u, sessionId);
    auto activation = launchPlanFor(plan);
    activation.resources = {ResourceKind::Recovery, ResourceKind::Process,
                            ResourceKind::Display, ResourceKind::Audio};
    ProductionCompatibilityActivation adapter(plan, identityFor(plan, sessionId));
    std::string error;
    check(!adapter.prepare(activation, bindingFor(plan), error) &&
              adapter.diagnostic().code == CompatibilityActivationCode::MissingLifecycleBoundary &&
              !fs::exists(plan.instanceRoot),
          "adapter refuses to fake PostWindow when authoritative Window lifecycle is absent");
}

} // namespace

int main() {
    testPhaseOrderRepeatedAndFinalizedExecution();
    testFreshProviderAuthorityBindingRejectsStaleMaterialization();
    testExactIdentityRejectsStaleReuseBeforeMutation();
    testEachRecipePhaseFailureReversesToBaseline();
    testInterruptedPreviousMaterializationIsRecovered();
    testUnsafePreexistingInstanceRetainsRecoveryOwnership();
    testRollbackVerificationFailureRequiresRecovery();
    testOneSeatRollbackDoesNotMutateOtherSeatInstance();
    testMissingWindowLifecycleBoundaryFailsClosed();

    if (failures != 0) {
        std::cerr << failures << " production compatibility activation test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Production compatibility activation tests passed.\n";
    return EXIT_SUCCESS;
}
