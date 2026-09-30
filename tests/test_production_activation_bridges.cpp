#include "hydra/production_activation_bridges.hpp"
#include "hydra/gate_c_external_profile.hpp"
#include "phase3_hardware_evidence_fixture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::launch;
using namespace hydra::production;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

runtime::RuntimeSessionId sessionId(std::uint8_t seed) {
    runtime::RuntimeSessionId id;
    for (std::size_t index = 0; index < id.bytes.size(); ++index)
        id.bytes[index] = static_cast<std::uint8_t>(seed + index);
    return id;
}

ProductionActivationEpoch epochFor(SeatId seatId,
                                   std::uint64_t fingerprint,
                                   std::uint64_t sessionGeneration = 5u,
                                   std::uint64_t seatGameGeneration = 7u) {
    ProductionActivationEpoch epoch;
    epoch.seatId = seatId;
    epoch.sessionId = sessionId(static_cast<std::uint8_t>(0x20u + seatId));
    epoch.sessionGeneration = sessionGeneration;
    epoch.seatGameGeneration = seatGameGeneration;
    epoch.activationFingerprint = fingerprint;
    return epoch;
}

class TestActivationContext final : public IProductionActivationContext {
public:
    explicit TestActivationContext(ProductionActivationEpoch epoch) {
        snapshot_.epoch = std::move(epoch);
        snapshot_.stage = ProductionActivationContextStage::PreProcess;
    }

    ProductionActivationContextSnapshot snapshot() const override {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    bool validatesEpoch(const ProductionActivationEpoch& epoch) const noexcept override {
        std::lock_guard lock(mutex_);
        return !invalidated_ && snapshot_.epoch == epoch;
    }

    bool validatesCurrentProcess(
        const ProductionProcessActivatedContext& process) const noexcept override {
        std::lock_guard lock(mutex_);
        return !invalidated_ &&
               snapshot_.stage == ProductionActivationContextStage::ProcessActive &&
               snapshot_.process && process.valid() &&
               snapshot_.process->epoch == process.epoch &&
               snapshot_.process->authoritativeProcess == process.authoritativeProcess &&
               snapshot_.process->handoffGeneration == process.handoffGeneration &&
               snapshot_.process->handoffState == process.handoffState;
    }

    void publish(process::ProcessIdentity identity,
                 std::uint64_t handoffGeneration = 0u,
                 process::ProcessHandoffState handoffState =
                     process::ProcessHandoffState::RootActive) {
        std::lock_guard lock(mutex_);
        ProductionProcessActivatedContext process;
        process.epoch = snapshot_.epoch;
        process.authoritativeProcess = std::move(identity);
        process.handoffGeneration = handoffGeneration;
        process.treeSequence = handoffGeneration + 10u;
        process.handoffState = handoffState;
        snapshot_.stage = ProductionActivationContextStage::ProcessActive;
        snapshot_.process = process;
        snapshot_.handoffState = handoffState;
        snapshot_.handoffGeneration = handoffGeneration;
        snapshot_.treeSequence = process.treeSequence;
    }

    void setStage(ProductionActivationContextStage stage,
                  std::optional<process::ProcessHandoffState> handoff = std::nullopt) {
        std::lock_guard lock(mutex_);
        snapshot_.stage = stage;
        snapshot_.process.reset();
        snapshot_.handoffState = handoff;
    }

    void invalidate() {
        std::lock_guard lock(mutex_);
        invalidated_ = true;
        snapshot_.stage = ProductionActivationContextStage::ProcessInvalidated;
        snapshot_.process.reset();
        snapshot_.handoffState.reset();
        snapshot_.handoffGeneration = 0;
        snapshot_.treeSequence = 0;
    }

private:
    mutable std::mutex mutex_;
    ProductionActivationContextSnapshot snapshot_;
    bool invalidated_{false};
};

SeatConfig seat(SeatId id) {
    SeatConfig value;
    value.seatId = id;
    value.name = L"Bridge Seat " + std::to_wstring(id);
    if (id == 1u) {
        value.keyboardIds = {L"Keyboard:HID\\VID_1111&PID_0001\\K1"};
        value.mouseIds = {L"Mouse:HID\\VID_1111&PID_0002\\M1"};
    } else {
        value.keyboardIds = {L"Keyboard:HID\\VID_2222&PID_0001\\K2"};
        value.mouseIds = {L"Mouse:HID\\VID_2222&PID_0002\\M2"};
    }
    value.active = true;
    return value;
}

SeatActivationPlan inputPlan(SeatId seatId, std::uint64_t fingerprint,
                             std::string gameId = "bridge-game") {
    SeatActivationPlan plan;
    plan.seatId = seatId;
    plan.seat = seat(seatId);
    plan.target.gameId = std::move(gameId);
    plan.target.process.seatId = seatId;
    plan.target.process.executablePath = L"C:\\Games\\bridge-game.exe";
    plan.target.requirements.keyboard = true;
    plan.target.requirements.mouse = true;
    plan.target.requirements.recovery = true;
    plan.target.capabilities.process = true;
    plan.target.capabilities.input = true;
    plan.target.capabilities.recovery = true;
    plan.resources = {ResourceKind::Recovery, ResourceKind::Process,
                      ResourceKind::Input};
    plan.fingerprint = fingerprint;
    return plan;
}

SeatActivationPlan recoveryOnlyPlan(SeatId seatId, std::uint64_t fingerprint) {
    auto plan = inputPlan(seatId, fingerprint, "recovery-only-game");
    plan.target.requirements.keyboard = false;
    plan.target.requirements.mouse = false;
    plan.target.capabilities.input = false;
    plan.resources = {ResourceKind::Recovery, ResourceKind::Process};
    return plan;
}

ProductionGateCProfile profileFor(std::string gameId) {
    ProductionGateCProfile profile;
    profile.gameId = std::move(gameId);
    profile.requiredApiMask = HYDRA_GATE_C_SHIM_POLLING_API_MASK |
                              HYDRA_GATE_C_SHIM_CURSOR_FOCUS_API_MASK |
                              HYDRA_GATE_C_SHIM_RAW_INPUT_API_MASK;
    profile.allowedProcessExecutablePaths = {L"C:\\Games\\bridge-game.exe"};
    profile.bridgeLibraryPath = L"C:\\HydraSeat\\hydra_production_gate_c_bridge.dll";
    profile.runtimeAttachApproved = true;
    profile.physicalCloakingRequired = true;
    profile.nativeHidHideMutationApproved = true;
    profile.spareRecoveryInputPresent = true;
    profile.hidHideExpiryMilliseconds = 30'000u;
    profile.hidHideAllowedApplications = {
        L"\\Device\\HarddiskVolume3\\HydraSeat\\hydra_host.exe",
        L"\\Device\\HarddiskVolume3\\HydraSeat\\hydra_production_gate_c_bridge.dll",
    };
    return profile;
}

process::ProcessIdentity processIdentity(std::uint32_t pid,
                                         std::uint64_t creation) {
    process::ProcessIdentity identity;
    identity.processId = pid;
    identity.creationTime100ns = creation;
    identity.executablePath = L"C:\\Games\\bridge-game.exe";
    return identity;
}

class FakeRecoveryRuntime final : public IProductionRecoveryLeaseRuntime {
public:
    bool arm(const recovery::RecoveryProcessAttachmentRegistration& registration,
             std::string& error) override {
        std::lock_guard lock(mutex);
        if (const auto found = active.find(registration.identity.seatId);
            found != active.end()) {
            if (found->second != registration) {
                error = "fake conflicting registration";
                return false;
            }
            error.clear();
            return true;
        }
        if (failArm) {
            error = "injected recovery arm failure";
            return false;
        }
        active[registration.identity.seatId] = registration;
        ++armCalls;
        error.clear();
        return true;
    }

    bool verifyArmed(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::string& error) override {
        std::lock_guard lock(mutex);
        ++verifyCalls;
        const auto found = active.find(registration.identity.seatId);
        if (failVerify || found == active.end() || found->second != registration) {
            error = "injected/missing exact recovery verification";
            return false;
        }
        error.clear();
        return true;
    }

    bool markActionActive(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::uint32_t actionId,
        std::string& error) override {
        std::lock_guard lock(mutex);
        const auto found = active.find(registration.identity.seatId);
        if (found == active.end() || found->second != registration) {
            error = "fake recovery action has stale registration";
            return false;
        }
        activeActions[registration.identity.seatId].push_back(actionId);
        error.clear();
        return true;
    }

    bool commit(const recovery::RecoveryProcessAttachmentRegistration& registration,
                std::string& error) override {
        std::lock_guard lock(mutex);
        if (failCommit || !active.contains(registration.identity.seatId)) {
            error = "injected recovery commit failure";
            return false;
        }
        committed[registration.identity.seatId] = true;
        error.clear();
        return true;
    }

    bool disarm(const recovery::RecoveryProcessAttachmentRegistration& registration,
                std::span<const std::uint32_t> verifiedRolledBackActionIds,
                std::string& error) override {
        std::lock_guard lock(mutex);
        const auto found = active.find(registration.identity.seatId);
        if (found == active.end()) {
            error.clear();
            return true;
        }
        if (found->second != registration) {
            error = "fake stale disarm rejected";
            return false;
        }
        if (failDisarm) {
            error = "injected recovery disarm failure";
            return false;
        }
        verifiedRollback[registration.identity.seatId] =
            std::vector<std::uint32_t>(verifiedRolledBackActionIds.begin(),
                                       verifiedRolledBackActionIds.end());
        active.erase(found);
        ++disarmCalls;
        error.clear();
        return true;
    }

    bool verifyDisarmed(
        const recovery::RecoveryProcessAttachmentRegistration& registration,
        std::string& error) override {
        std::lock_guard lock(mutex);
        if (active.contains(registration.identity.seatId)) {
            error = "fake registration still active";
            return false;
        }
        error.clear();
        return true;
    }

    std::optional<recovery::RecoveryProcessAttachmentRegistration> registration(
        SeatId seatId) const {
        std::lock_guard lock(mutex);
        const auto found = active.find(seatId);
        return found == active.end() ? std::nullopt
                                     : std::optional{found->second};
    }

    mutable std::mutex mutex;
    std::map<SeatId, recovery::RecoveryProcessAttachmentRegistration> active;
    std::map<SeatId, std::vector<std::uint32_t>> activeActions;
    std::map<SeatId, std::vector<std::uint32_t>> verifiedRollback;
    std::map<SeatId, bool> committed;
    std::atomic<bool> failArm{false};
    std::atomic<bool> failVerify{false};
    std::atomic<bool> failCommit{false};
    std::atomic<bool> failDisarm{false};
    std::uint32_t armCalls{0};
    std::uint32_t verifyCalls{0};
    std::uint32_t disarmCalls{0};
};

class FakeGateCRuntime final : public IProductionGateCSessionRuntime {
public:
    bool start(const ProductionGateCSessionRequest& request,
               std::string& error) override {
        std::lock_guard lock(mutex);
        ++startCalls;
        if (failStart) {
            error = "injected Gate-C start failure";
            return false;
        }
        if (const auto found = active.find(request.epoch.seatId); found != active.end() &&
            !(found->second.process.authoritativeProcess.sameInstance(
                  request.process.authoritativeProcess) &&
              found->second.process.handoffGeneration == request.process.handoffGeneration)) {
            error = "fake Gate-C conflicting exact Seat session";
            return false;
        }
        active[request.epoch.seatId] = request;
        error.clear();
        return true;
    }

    bool verify(const ProductionGateCSessionRequest& request,
                ProductionGateCSessionStatus& status,
                std::string& error) override {
        std::lock_guard lock(mutex);
        ++verifyCalls;
        const auto found = active.find(request.epoch.seatId);
        if (failVerify || !transportPresent || found == active.end() ||
            !found->second.process.authoritativeProcess.sameInstance(
                request.process.authoritativeProcess) ||
            found->second.process.handoffGeneration != request.process.handoffGeneration) {
            error = "fake Gate-C receiver verification failed";
            status = {};
            return false;
        }
        status.active = true;
        status.receiverVerified = receiverVerified;
        status.assignedDevicesPresent = devicesPresent;
        status.process = request.process.authoritativeProcess;
        status.handoffGeneration = request.process.handoffGeneration;
        status.receiverSequence = ++sequence;
        error.clear();
        return receiverVerified && devicesPresent;
    }

    bool inputMetricsSnapshot(const ProductionGateCSessionRequest& request,
                              InputMetricsSnapshot& snapshot,
                              std::string& error) override {
        std::lock_guard lock(mutex);
        ++metricsCalls;
        const auto found = active.find(request.epoch.seatId);
        if (failMetrics || found == active.end() ||
            !found->second.process.authoritativeProcess.sameInstance(
                request.process.authoritativeProcess) ||
            found->second.process.handoffGeneration != request.process.handoffGeneration) {
            error = "fake Gate-C metrics snapshot rejected stale/missing exact session";
            snapshot = {};
            return false;
        }
        snapshot = metricsSnapshot;
        error.clear();
        return true;
    }

    bool stop(const ProductionGateCSessionRequest& request,
              std::string& error) noexcept override {
        std::lock_guard lock(mutex);
        ++stopCalls;
        const auto found = active.find(request.epoch.seatId);
        if (found == active.end()) {
            error.clear();
            return true;
        }
        if (!found->second.process.authoritativeProcess.sameInstance(
                request.process.authoritativeProcess) ||
            found->second.process.handoffGeneration != request.process.handoffGeneration) {
            error = "fake Gate-C stale stop rejected";
            return false;
        }
        if (failStop) {
            error = "injected Gate-C rollback verification failure";
            return false;
        }
        active.erase(found);
        error.clear();
        return true;
    }

    bool hasActive(SeatId seatId) const {
        std::lock_guard lock(mutex);
        return active.contains(seatId);
    }

    mutable std::mutex mutex;
    std::map<SeatId, ProductionGateCSessionRequest> active;
    std::atomic<bool> failStart{false};
    std::atomic<bool> failVerify{false};
    std::atomic<bool> failMetrics{false};
    std::atomic<bool> failStop{false};
    std::atomic<bool> receiverVerified{true};
    std::atomic<bool> devicesPresent{true};
    std::atomic<bool> transportPresent{true};
    InputMetricsSnapshot metricsSnapshot;
    std::uint64_t sequence{0};
    std::uint32_t startCalls{0};
    std::uint32_t verifyCalls{0};
    std::uint32_t metricsCalls{0};
    std::uint32_t stopCalls{0};
};

class FakeHidHidePlatform final : public HidHideSessionPlatform {
public:
    bool readState(HidHideSessionSnapshot& output,
                   std::string& error) noexcept override {
        std::lock_guard lock(mutex);
        if (failRead) {
            error = "injected HidHide read failure";
            return false;
        }
        output = state;
        error.clear();
        return true;
    }

    bool writeState(const HidHideSessionSnapshot& input,
                    std::string& error) noexcept override {
        std::lock_guard lock(mutex);
        if (failWrite) {
            error = "injected HidHide write failure";
            return false;
        }
        state = input;
        error.clear();
        return true;
    }

    bool addSessionBlacklist(std::span<const std::wstring> ids,
                             std::string& error) noexcept override {
        std::lock_guard lock(mutex);
        if (failAdd) {
            error = "injected HidHide session blacklist failure";
            return false;
        }
        for (const auto& id : ids) {
            if (std::find(sessionDevices.begin(), sessionDevices.end(), id) ==
                sessionDevices.end()) {
                sessionDevices.push_back(id);
            }
        }
        error.clear();
        return true;
    }

    bool clearSessionBlacklist(std::string& error) noexcept override {
        std::lock_guard lock(mutex);
        if (failClear) {
            error = "injected HidHide rollback failure";
            return false;
        }
        sessionDevices.clear();
        error.clear();
        return true;
    }

    bool mutationSupported() const noexcept override { return true; }
    bool sessionBlacklistSupported() const noexcept override { return true; }

    mutable std::mutex mutex;
    HidHideSessionSnapshot state;
    std::vector<std::wstring> sessionDevices;
    bool failRead{false};
    bool failWrite{false};
    bool failAdd{false};
    bool failClear{false};
};

struct Harness {
    test::SyntheticPhase3EvidenceFixture evidenceFixture;
    std::shared_ptr<FakeRecoveryRuntime> recoveryRuntime{
        std::make_shared<FakeRecoveryRuntime>()};
    std::shared_ptr<FakeGateCRuntime> gateRuntime{
        std::make_shared<FakeGateCRuntime>()};
    std::shared_ptr<FakeHidHidePlatform> hidHide{
        std::make_shared<FakeHidHidePlatform>()};
    std::shared_ptr<recovery::RecoveryProcessAttachmentAuthority> recoveryAuthority{
        std::make_shared<recovery::RecoveryProcessAttachmentAuthority>()};
    ProductionActivationBridgeConfig config;
    ProductionActivationBridgeDependencies dependencies;
    std::shared_ptr<ProductionActivationResourceBridges> bridges;

    Harness() {
        config.gateCProfiles = {profileFor("bridge-game")};
        config.inputEvidenceClass = ProductionInputEvidenceClass::Physical;
        config.physicalAcceptanceEvidence = evidenceFixture.evidence();
        config.recoveryRoot = std::filesystem::temp_directory_path() /
            ("hydraseat-production-bridge-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        config.hidHidePlatform = hidHide;
        config.monitorIntervalMilliseconds = 20u;
        dependencies.recoveryAttachmentAuthority = recoveryAuthority;
        dependencies.recoveryLeaseRuntime = recoveryRuntime;
        dependencies.gateCSessionRuntime = gateRuntime;
        bridges = makeProductionActivationResourceBridges(config, dependencies);
    }

    ~Harness() {
        bridges.reset();
        std::error_code ignored;
        std::filesystem::remove_all(config.recoveryRoot, ignored);
    }
};

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

void testContextOrderingAndExactAttachment() {
    Harness harness;
    const auto plan = inputPlan(1u, 0x1111u);
    auto context = std::make_shared<TestActivationContext>(epochFor(1u, plan.fingerprint));
    std::string error;
    auto recovery = harness.bridges->createRecoveryResource(plan, context, error);
    auto input = harness.bridges->createInputResource(plan, context, error);
    check(recovery && input, "Recovery and Input resources are created from one PreProcess context");
    if (!recovery || !input) return;

    const runtime::SeatGameBinding binding{"player-one", plan.target.gameId};
    check(recovery->prepare(plan, binding, error),
          "Recovery accepts exact Seat/session/game PreProcess context");
    check(input->prepare(plan, binding, error),
          "Input accepts exact typed physical scope during PreProcess prepare");
    check(recovery->activate(error) && recovery->verifyActive(error),
          "Recovery enters staged AwaitingExactProcess without inventing PID");
    check(!recovery->active() && !harness.recoveryRuntime->registration(1u),
          "Recovery does not claim a process-bound lease before Process authority exists");

    check(!input->activate(error),
          "Input activation fails closed while exact process context is unavailable");
    check(!harness.recoveryRuntime->registration(1u),
          "too-early Input activation cannot manufacture a recovery attachment");

    context->publish(processIdentity(55001u, 0x1234567800ull));
    check(input->activate(error), "Input activates after exact ProcessIdentity publication");
    check(input->verifyActive(error) && input->active(),
          "Input success requires current receiver-side Gate-C and recovery verification");
    harness.gateRuntime->metricsSnapshot.acceptedSamples = 3u;
    InputMetricsSnapshot liveMetrics;
    check(harness.bridges->inputMetricsSnapshot(plan, liveMetrics, error) &&
              liveMetrics.acceptedSamples == 3u && harness.gateRuntime->metricsCalls == 1u,
          "read-only metrics observation succeeds only through the exact active Gate-C receiver session");
    auto wrongFingerprintPlan = plan;
    ++wrongFingerprintPlan.fingerprint;
    InputMetricsSnapshot wrongFingerprintMetrics;
    check(!harness.bridges->inputMetricsSnapshot(
              wrongFingerprintPlan, wrongFingerprintMetrics, error),
          "metrics observation rejects a different immutable Seat plan fingerprint");
    const auto registration = harness.recoveryRuntime->registration(1u);
    check(registration && registration->identity.seatId == 1u &&
              registration->identity.hostSessionId == context->snapshot().epoch.sessionId &&
              registration->identity.sessionGeneration ==
                  context->snapshot().epoch.sessionGeneration &&
              registration->identity.seatGameGeneration ==
                  context->snapshot().epoch.seatGameGeneration &&
              registration->identity.process.processId == 55001u &&
              registration->identity.process.creationTime100ns == 0x1234567800ull,
          "recovery lease binds exact Seat/session/generation/PID/creation identity");
    check(registration && registration->manifest.actions.size() == 2u &&
              registration->manifest.actions[0].kind ==
                  watchdog::RollbackActionKind::TerminateOwnedProcess &&
              registration->manifest.actions[1].kind ==
                  watchdog::RollbackActionKind::RestoreSnapshotState,
          "physical activation arms one exact process action plus one HidHide snapshot restore action");

    check(input->rollback(error) && input->verifySafe(error),
          "Input rollback restores HidHide and verifies Gate-C receiver teardown");
    InputMetricsSnapshot afterRollbackMetrics;
    check(!harness.bridges->inputMetricsSnapshot(plan, afterRollbackMetrics, error),
          "metrics observation fails closed as soon as Input rollback removes live receiver authority");
    context->invalidate();
    check(recovery->rollback(error) && recovery->verifySafe(error),
          "Recovery disarms only after exact Process context is invalidated/teardown-complete");
    check(!harness.recoveryRuntime->registration(1u),
          "rollback leaves no exact recovery lease armed");
}

void testPhysicalEvidenceAndContextPolicy() {
    Harness harness;
    const auto plan = inputPlan(1u, 0x2201u);
    const runtime::SeatGameBinding binding{"player-one", plan.target.gameId};
    std::string error;

    auto noEvidenceConfig = harness.config;
    noEvidenceConfig.inputEvidenceClass = ProductionInputEvidenceClass::None;
    noEvidenceConfig.physicalAcceptanceEvidence.reset();
    auto noEvidenceBridge = makeProductionActivationResourceBridges(
        noEvidenceConfig, harness.dependencies);
    auto noEvidenceContext = std::make_shared<TestActivationContext>(
        epochFor(1u, plan.fingerprint));
    auto noEvidenceInput = noEvidenceBridge->createInputResource(
        plan, noEvidenceContext, error);
    check(noEvidenceInput && !noEvidenceInput->prepare(plan, binding, error),
          "physical production Input rejects missing P3-HW evidence before mutation");

    auto noProfileConfig = harness.config;
    noProfileConfig.gateCProfiles.clear();
    noProfileConfig.inputEvidenceClass = ProductionInputEvidenceClass::None;
    noProfileConfig.physicalAcceptanceEvidence.reset();
    auto noProfileBridge = makeProductionActivationResourceBridges(
        noProfileConfig, harness.dependencies);
    auto noProfileRecoveryContext = std::make_shared<TestActivationContext>(
        epochFor(1u, plan.fingerprint));
    auto noProfileRecovery = noProfileBridge->createRecoveryResource(
        plan, noProfileRecoveryContext, error);
    check(noProfileRecovery != nullptr,
          "base Host composition can provide Recovery without inventing a Game Gate-C profile");
    auto noProfileInputContext = std::make_shared<TestActivationContext>(
        epochFor(1u, plan.fingerprint));
    auto noProfileInput = noProfileBridge->createInputResource(
        plan, noProfileInputContext, error);
    check(!noProfileInput && error.find("no trusted production Gate-C profile") != std::string::npos,
          "base Host composition keeps keyboard/mouse Input fail-closed without an exact reviewed Game profile");

    auto syntheticConfig = harness.config;
    syntheticConfig.inputEvidenceClass = ProductionInputEvidenceClass::Synthetic;
    auto syntheticBridge = makeProductionActivationResourceBridges(
        syntheticConfig, harness.dependencies);
    auto syntheticContext = std::make_shared<TestActivationContext>(
        epochFor(1u, plan.fingerprint));
    auto syntheticInput = syntheticBridge->createInputResource(
        plan, syntheticContext, error);
    check(syntheticInput && !syntheticInput->prepare(plan, binding, error),
          "Synthetic/Controlled evidence cannot authorize the Physical mutation path");

    auto wrongSeatContext = std::make_shared<TestActivationContext>(
        epochFor(2u, plan.fingerprint));
    check(!harness.bridges->createInputResource(plan, wrongSeatContext, error),
          "bridge rejects a context authority for the wrong Seat");

    auto invalidatedContext = std::make_shared<TestActivationContext>(
        epochFor(1u, plan.fingerprint));
    invalidatedContext->invalidate();
    check(!harness.bridges->createInputResource(plan, invalidatedContext, error),
          "bridge rejects an invalidated/stale Host activation context");

    auto thirdSeatPlan = inputPlan(3u, 0x2203u);
    auto thirdSeatContext = std::make_shared<TestActivationContext>(
        epochFor(3u, thirdSeatPlan.fingerprint));
    check(!harness.bridges->createInputResource(
              thirdSeatPlan, thirdSeatContext, error),
          "production bridge rejects a third v1 Seat");

    const auto sameKeyPlan = recoveryOnlyPlan(1u, 0x2299u);
    auto firstContext = std::make_shared<TestActivationContext>(
        epochFor(1u, sameKeyPlan.fingerprint));
    auto firstRecovery = harness.bridges->createRecoveryResource(
        sameKeyPlan, firstContext, error);
    check(firstRecovery != nullptr,
          "first immutable recovery context is accepted");
    auto staleReplacement = std::make_shared<TestActivationContext>(
        epochFor(1u, sameKeyPlan.fingerprint, 4u, 7u));
    check(!harness.bridges->createRecoveryResource(
              sameKeyPlan, staleReplacement, error),
          "same activation key cannot be rebound to a stale session-generation authority");
}

void testRecoveryHandoffPidReuseAndStaleDisarm() {
    Harness harness;
    const auto plan = recoveryOnlyPlan(1u, 0x3301u);
    auto context = std::make_shared<TestActivationContext>(epochFor(1u, plan.fingerprint));
    std::string error;
    auto recovery = harness.bridges->createRecoveryResource(plan, context, error);
    const runtime::SeatGameBinding binding{"player-one", plan.target.gameId};
    check(recovery && recovery->prepare(plan, binding, error) &&
              recovery->activate(error),
          "recovery-only resource enters staged activation");
    if (!recovery) return;

    const auto root = processIdentity(56001u, 0x2200000001ull);
    context->publish(root, 0u, process::ProcessHandoffState::RootActive);
    check(waitUntil([&] { return harness.recoveryRuntime->registration(1u).has_value(); }),
          "Recovery monitor attaches once exact root process authority is published");
    const auto rootRegistration = harness.recoveryRuntime->registration(1u);
    check(rootRegistration && rootRegistration->identity.process.processId == root.processId &&
              rootRegistration->identity.process.creationTime100ns == root.creationTime100ns &&
              rootRegistration->identity.recoveryEpoch == 1u,
          "root recovery attachment uses exact PID+creation and monotonic handoff epoch");
    if (!rootRegistration) return;

    const auto duplicate = harness.recoveryAuthority->registerAttachment(*rootRegistration);
    check(duplicate.code == recovery::RecoveryAttachmentCode::AlreadySatisfied,
          "exact duplicate recovery registration is idempotent");

    auto wrongSeat = rootRegistration->identity;
    wrongSeat.seatId = 2u;
    check(!harness.recoveryAuthority->verifyArmed(
              wrongSeat, rootRegistration->manifest.lease).succeeded(),
          "wrong Seat cannot reuse another Seat recovery lease");
    auto wrongSession = rootRegistration->identity;
    wrongSession.hostSessionId.bytes[0] ^= 0x40u;
    check(!harness.recoveryAuthority->verifyArmed(
              wrongSession, rootRegistration->manifest.lease).succeeded(),
          "wrong Host session cannot reuse an exact recovery lease");
    auto staleGame = rootRegistration->identity;
    --staleGame.seatGameGeneration;
    check(!harness.recoveryAuthority->verifyArmed(
              staleGame, rootRegistration->manifest.lease).succeeded(),
          "stale Seat-game generation is rejected by recovery authority");
    auto reusedPid = rootRegistration->identity;
    ++reusedPid.process.creationTime100ns;
    check(!harness.recoveryAuthority->disarm(
              reusedPid, rootRegistration->manifest.lease).succeeded(),
          "same PID with another creation time cannot stale-disarm recovery");
    check(harness.recoveryAuthority->verifyArmed(
              rootRegistration->identity, rootRegistration->manifest.lease).succeeded(),
          "failed stale disarm leaves the exact original attachment armed");

    const auto descendant = processIdentity(56002u, 0x2200000002ull);
    context->publish(descendant, 1u,
                     process::ProcessHandoffState::DescendantActive);
    check(waitUntil([&] {
        const auto current = harness.recoveryRuntime->registration(1u);
        return current && current->identity.process.processId == descendant.processId &&
               current->identity.process.creationTime100ns == descendant.creationTime100ns &&
               current->identity.recoveryEpoch == 2u;
    }), "trusted handoff explicitly retires the old attachment and arms the exact descendant");
    const auto descendantRegistration = harness.recoveryRuntime->registration(1u);
    check(descendantRegistration &&
              !harness.recoveryAuthority->verifyArmed(
                  rootRegistration->identity,
                  rootRegistration->manifest.lease).succeeded() &&
              harness.recoveryAuthority->verifyArmed(
                  descendantRegistration->identity,
                  descendantRegistration->manifest.lease).succeeded(),
          "old handoff authority is invalid while the new exact recovery lease is armed");

    context->invalidate();
    check(waitUntil([&] { return !harness.recoveryRuntime->registration(1u); }),
          "invalidated context retires recovery after exact process lifetime is terminal");
    check(recovery->rollback(error) && recovery->verifySafe(error),
          "Recovery rollback is idempotently safe after monitor retirement");
}

void testTwoSeatPhysicalIsolation() {
    Harness harness;
    const auto plan1 = inputPlan(1u, 0x4401u);
    const auto plan2 = inputPlan(2u, 0x4402u);
    auto context1 = std::make_shared<TestActivationContext>(epochFor(1u, plan1.fingerprint));
    auto context2 = std::make_shared<TestActivationContext>(epochFor(2u, plan2.fingerprint));
    std::string error;
    auto recovery1 = harness.bridges->createRecoveryResource(plan1, context1, error);
    auto input1 = harness.bridges->createInputResource(plan1, context1, error);
    auto recovery2 = harness.bridges->createRecoveryResource(plan2, context2, error);
    auto input2 = harness.bridges->createInputResource(plan2, context2, error);
    const runtime::SeatGameBinding binding1{"p1", plan1.target.gameId};
    const runtime::SeatGameBinding binding2{"p2", plan2.target.gameId};
    check(recovery1 && input1 && recovery2 && input2 &&
              recovery1->prepare(plan1, binding1, error) &&
              input1->prepare(plan1, binding1, error) &&
              recovery2->prepare(plan2, binding2, error) &&
              input2->prepare(plan2, binding2, error) &&
              recovery1->activate(error) && recovery2->activate(error),
          "two Seats independently prepare one shared production bridge composition");
    if (!recovery1 || !input1 || !recovery2 || !input2) return;

    context1->publish(processIdentity(57001u, 0x3300000001ull));
    context2->publish(processIdentity(57002u, 0x3300000002ull));
    check(input1->activate(error) && input2->activate(error) &&
              input1->verifyActive(error) && input2->verifyActive(error),
          "two exact Seat process/input sessions become active concurrently");
    check(harness.recoveryRuntime->registration(1u) &&
              harness.recoveryRuntime->registration(2u),
          "two Seats own distinct recovery leases concurrently");
    {
        std::lock_guard lock(harness.hidHide->mutex);
        check(harness.hidHide->sessionDevices.size() == 4u,
              "HidHide process-lifetime blacklist contains the exact union of both Seat device scopes");
        check(harness.hidHide->state.active,
              "shared persistent HidHide state remains active while either Seat is active");
    }

    check(input1->rollback(error) && input1->verifySafe(error),
          "Seat 1 Input rollback succeeds while Seat 2 remains active");
    check(input2->verifyActive(error) && input2->active(),
          "Seat 1 rollback does not tear down Seat 2 Gate-C or recovery verification");
    check(!harness.gateRuntime->hasActive(1u) &&
              harness.gateRuntime->hasActive(2u) &&
              harness.recoveryRuntime->registration(2u),
          "Seat 2 Gate-C session/recovery lease survive Seat 1 teardown");
    {
        std::lock_guard lock(harness.hidHide->mutex);
        check(harness.hidHide->sessionDevices.size() == 2u &&
                  std::all_of(harness.hidHide->sessionDevices.begin(),
                              harness.hidHide->sessionDevices.end(),
                              [](const auto& id) {
                                  return id.find(L"VID_2222") != std::wstring::npos;
                              }),
              "Seat 1 cleanup rebuilds HidHide session blacklist with only Seat 2 devices");
        check(harness.hidHide->state.active,
              "Seat 1 persistent rollback preserves Seat 2's active HidHide union");
    }
    context1->invalidate();
    check(recovery1->rollback(error) && recovery1->verifySafe(error),
          "Seat 1 recovery lease retires independently after Process teardown");
    check(input2->verifyActive(error),
          "Seat 2 remains verified after Seat 1 recovery disarm");

    check(input2->rollback(error) && input2->verifySafe(error),
          "Seat 2 Input rollback succeeds independently");
    context2->invalidate();
    check(recovery2->rollback(error) && recovery2->verifySafe(error),
          "Seat 2 recovery lease retires independently");
    {
        std::lock_guard lock(harness.hidHide->mutex);
        check(harness.hidHide->sessionDevices.empty() &&
                  !harness.hidHide->state.active &&
                  harness.hidHide->state.allowedApplications.empty(),
              "last Seat rollback restores the exact pre-activation HidHide baseline");
    }
}

void testInputHandoffFailsClosedAndRecoveryReattaches() {
    Harness harness;
    const auto plan = inputPlan(1u, 0x5501u);
    auto context = std::make_shared<TestActivationContext>(epochFor(1u, plan.fingerprint));
    std::string error;
    auto recovery = harness.bridges->createRecoveryResource(plan, context, error);
    auto input = harness.bridges->createInputResource(plan, context, error);
    const runtime::SeatGameBinding binding{"p1", plan.target.gameId};
    check(recovery && input && recovery->prepare(plan, binding, error) &&
              input->prepare(plan, binding, error) && recovery->activate(error),
          "handoff test prepares Recovery/Input resources");
    if (!recovery || !input) return;

    const auto root = processIdentity(58001u, 0x4400000001ull);
    context->publish(root);
    check(input->activate(error) && input->verifyActive(error),
          "Input initially binds exact root authority");
    const auto oldRegistration = harness.recoveryRuntime->registration(1u);
    check(oldRegistration && oldRegistration->identity.process.processId == root.processId,
          "root recovery attachment is present before handoff");

    const auto game = processIdentity(58002u, 0x4400000002ull);
    context->publish(game, 1u, process::ProcessHandoffState::DescendantActive);
    check(waitUntil([&] { return !input->active(); }),
          "Input fails closed when exact authoritative process changes underneath it");
    check(input->verifySafe(error),
          "handoff-triggered Input rollback verifies receiver/HidHide safety");
    check(waitUntil([&] {
        const auto current = harness.recoveryRuntime->registration(1u);
        return current && current->identity.process.processId == game.processId &&
               current->identity.process.creationTime100ns == game.creationTime100ns &&
               current->identity.recoveryEpoch == 2u &&
               current->manifest.actions.size() == 1u;
    }), "Recovery explicitly re-attaches to the new exact handoff authority after Input teardown");
    check(!harness.gateRuntime->hasActive(1u),
          "old Gate-C process session is not silently redirected across handoff");
    if (oldRegistration) {
        check(!harness.recoveryAuthority->verifyArmed(
                  oldRegistration->identity,
                  oldRegistration->manifest.lease).succeeded(),
              "old recovery process authority is invalid after handoff replacement");
    }

    context->invalidate();
    check(waitUntil([&] { return !harness.recoveryRuntime->registration(1u); }),
          "new handoff recovery lease retires after activation context invalidation");
    check(recovery->rollback(error) && recovery->verifySafe(error),
          "Recovery is safe after handoff/invalidation cleanup");
}

void testHotUnplugTransportAndProcessExit() {
    {
        Harness harness;
        const auto plan = inputPlan(1u, 0x6601u);
        auto context = std::make_shared<TestActivationContext>(epochFor(1u, plan.fingerprint));
        std::string error;
        auto recovery = harness.bridges->createRecoveryResource(plan, context, error);
        auto input = harness.bridges->createInputResource(plan, context, error);
        const runtime::SeatGameBinding binding{"p1", plan.target.gameId};
        check(recovery && input && recovery->prepare(plan, binding, error) &&
                  input->prepare(plan, binding, error) && recovery->activate(error),
              "hot-unplug test prepares bridge resources");
        if (recovery && input) {
            context->publish(processIdentity(59001u, 0x5500000001ull));
            check(input->activate(error), "hot-unplug test activates Input");
            harness.gateRuntime->devicesPresent = false;
            check(waitUntil([&] { return !input->active(); }),
                  "assigned physical device disappearance tears down Input instead of reporting success");
            check(input->verifySafe(error),
                  "hot-unplug path verifies Gate-C/HidHide rollback");
            check(harness.recoveryRuntime->registration(1u).has_value(),
                  "hot-unplug retains exact process recovery until Process teardown");
            context->invalidate();
            check(recovery->rollback(error) && recovery->verifySafe(error),
                  "hot-unplug recovery lease is removable after Process teardown");
        }
    }

    {
        Harness harness;
        const auto plan = inputPlan(1u, 0x6602u);
        auto context = std::make_shared<TestActivationContext>(epochFor(1u, plan.fingerprint));
        std::string error;
        auto recovery = harness.bridges->createRecoveryResource(plan, context, error);
        auto input = harness.bridges->createInputResource(plan, context, error);
        const runtime::SeatGameBinding binding{"p1", plan.target.gameId};
        if (recovery && input && recovery->prepare(plan, binding, error) &&
            input->prepare(plan, binding, error) && recovery->activate(error)) {
            context->publish(processIdentity(59002u, 0x5500000002ull));
            check(input->activate(error), "transport-loss test activates Input");
            harness.gateRuntime->transportPresent = false;
            check(waitUntil([&] { return !input->active(); }),
                  "Gate-C transport disappearance tears down production Input");
            check(input->verifySafe(error),
                  "transport-loss path verifies rollback rather than ordinary success");
            context->invalidate();
            check(recovery->rollback(error),
                  "transport-loss recovery is clean after Process teardown");
        } else {
            check(false, "transport-loss test could not prepare bridge resources");
        }
    }

    {
        Harness harness;
        const auto plan = inputPlan(1u, 0x6603u);
        auto context = std::make_shared<TestActivationContext>(epochFor(1u, plan.fingerprint));
        std::string error;
        auto recovery = harness.bridges->createRecoveryResource(plan, context, error);
        auto input = harness.bridges->createInputResource(plan, context, error);
        const runtime::SeatGameBinding binding{"p1", plan.target.gameId};
        if (recovery && input && recovery->prepare(plan, binding, error) &&
            input->prepare(plan, binding, error) && recovery->activate(error)) {
            context->publish(processIdentity(59003u, 0x5500000003ull));
            check(input->activate(error), "process-exit test activates Input");
            context->setStage(ProductionActivationContextStage::ProcessExited,
                              process::ProcessHandoffState::TreeExited);
            check(waitUntil([&] { return !input->active(); }),
                  "target process exit invalidates process-bound Input session");
            check(input->verifySafe(error),
                  "process-exit path rolls back physical/process-local state");
            check(waitUntil([&] { return !harness.recoveryRuntime->registration(1u); }),
                  "process-exit terminal state retires the exact recovery attachment");
            check(recovery->rollback(error),
                  "Recovery rollback is safe after natural target exit");
        } else {
            check(false, "process-exit test could not prepare bridge resources");
        }
    }
}

void testExplicitRollbackFailures() {
    {
        Harness harness;
        const auto plan = inputPlan(1u, 0x7701u);
        auto context = std::make_shared<TestActivationContext>(epochFor(1u, plan.fingerprint));
        std::string error;
        auto recovery = harness.bridges->createRecoveryResource(plan, context, error);
        auto input = harness.bridges->createInputResource(plan, context, error);
        const runtime::SeatGameBinding binding{"p1", plan.target.gameId};
        if (recovery && input && recovery->prepare(plan, binding, error) &&
            input->prepare(plan, binding, error) && recovery->activate(error)) {
            context->publish(processIdentity(60001u, 0x6600000001ull));
            check(input->activate(error), "Gate rollback failure test activates Input");
            harness.gateRuntime->failStop = true;
            check(!input->rollback(error) && !input->verifySafe(error),
                  "receiver rollback failure remains explicit instead of fake safe success");
            check(!recovery->verifyActive(error) &&
                      harness.recoveryRuntime->registration(1u),
                  "receiver rollback failure stops lease renewal while retaining exact recovery authority");
            harness.gateRuntime->failStop = false;
            check(input->rollback(error),
                  "receiver rollback can be retried against the same exact process session");
            context->invalidate();
            check(recovery->rollback(error),
                  "recovery lease can be cleanly retired after retry and Process teardown");
        } else {
            check(false, "Gate rollback failure test could not prepare resources");
        }
    }

    {
        Harness harness;
        const auto plan = inputPlan(1u, 0x7702u);
        auto context = std::make_shared<TestActivationContext>(epochFor(1u, plan.fingerprint));
        std::string error;
        auto recovery = harness.bridges->createRecoveryResource(plan, context, error);
        auto input = harness.bridges->createInputResource(plan, context, error);
        const runtime::SeatGameBinding binding{"p1", plan.target.gameId};
        if (recovery && input && recovery->prepare(plan, binding, error) &&
            input->prepare(plan, binding, error) && recovery->activate(error)) {
            context->publish(processIdentity(60002u, 0x6600000002ull));
            check(input->activate(error), "HidHide rollback failure test activates Input");
            harness.hidHide->failClear = true;
            check(!input->rollback(error) && !input->verifySafe(error),
                  "HidHide rollback verification failure remains explicit");
            check(!recovery->verifyActive(error) &&
                      harness.recoveryRuntime->registration(1u),
                  "unverifiable shared physical rollback escalates to recovery instead of clearing another Seat blindly");
        } else {
            check(false, "HidHide rollback failure test could not prepare resources");
        }
    }

    {
        Harness harness;
        const auto plan = recoveryOnlyPlan(1u, 0x7703u);
        auto context = std::make_shared<TestActivationContext>(epochFor(1u, plan.fingerprint));
        std::string error;
        auto recovery = harness.bridges->createRecoveryResource(plan, context, error);
        const runtime::SeatGameBinding binding{"p1", plan.target.gameId};
        if (recovery && recovery->prepare(plan, binding, error) && recovery->activate(error)) {
            context->publish(processIdentity(60003u, 0x6600000003ull));
            check(waitUntil([&] { return harness.recoveryRuntime->registration(1u).has_value(); }),
                  "recovery-disarm failure test arms exact attachment");
            harness.recoveryRuntime->failDisarm = true;
            context->invalidate();
            check(!recovery->rollback(error) && !recovery->verifySafe(error),
                  "unverifiable recovery disarm returns failure/RecoveryRequired semantics");
        } else {
            check(false, "recovery-disarm failure test could not prepare resources");
        }
    }
}

void testReceiverVerificationAndRecoveryVerificationGates() {
    {
        Harness harness;
        harness.gateRuntime->receiverVerified = false;
        const auto plan = inputPlan(1u, 0x8801u);
        auto context = std::make_shared<TestActivationContext>(epochFor(1u, plan.fingerprint));
        std::string error;
        auto recovery = harness.bridges->createRecoveryResource(plan, context, error);
        auto input = harness.bridges->createInputResource(plan, context, error);
        const runtime::SeatGameBinding binding{"p1", plan.target.gameId};
        if (recovery && input && recovery->prepare(plan, binding, error) &&
            input->prepare(plan, binding, error) && recovery->activate(error)) {
            context->publish(processIdentity(61001u, 0x7700000001ull));
            check(!input->activate(error),
                  "Gate-C API/start success without receiver verification is rejected");
            check(!harness.recoveryRuntime->registration(1u),
                  "receiver verification failure occurs before physical/recovery mutation");
            context->invalidate();
            check(recovery->rollback(error),
                  "receiver verification failure leaves staged Recovery clean");
        } else {
            check(false, "receiver-verification test could not prepare resources");
        }
    }

    {
        Harness harness;
        const auto plan = inputPlan(1u, 0x8802u);
        auto context = std::make_shared<TestActivationContext>(epochFor(1u, plan.fingerprint));
        std::string error;
        auto recovery = harness.bridges->createRecoveryResource(plan, context, error);
        auto input = harness.bridges->createInputResource(plan, context, error);
        const runtime::SeatGameBinding binding{"p1", plan.target.gameId};
        if (recovery && input && recovery->prepare(plan, binding, error) &&
            input->prepare(plan, binding, error) && recovery->activate(error)) {
            context->publish(processIdentity(61002u, 0x7700000002ull));
            check(input->activate(error), "recovery-verification test activates Input");
            harness.recoveryRuntime->failVerify = true;
            check(waitUntil([&] { return !input->active(); }),
                  "loss of exact recovery verification tears down Input");
            check(!recovery->verifyActive(error),
                  "Recovery resource reports verification loss explicitly");
        } else {
            check(false, "recovery-verification test could not prepare resources");
        }
    }
}

} // namespace

int main() {
    testContextOrderingAndExactAttachment();
    testPhysicalEvidenceAndContextPolicy();
    testRecoveryHandoffPidReuseAndStaleDisarm();
    testTwoSeatPhysicalIsolation();
    testInputHandoffFailsClosedAndRecoveryReattaches();
    testHotUnplugTransportAndProcessExit();
    testExplicitRollbackFailures();
    testReceiverVerificationAndRecoveryVerificationGates();

    if (failures != 0) {
        std::cerr << failures << " production activation bridge test(s) failed.\n";
        return 1;
    }
    std::cout << "Production activation bridge tests passed.\n";
    return 0;
}
