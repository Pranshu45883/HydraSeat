#include "hydra/two_seat_launch.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::launch;
using namespace hydra::runtime;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string key(SeatId seatId, ResourceKind kind) {
    return std::to_string(seatId) + ":" + std::string(resourceKindName(kind));
}

SeatLaunchInput makeInput(SeatId seatId, std::string gameId) {
    SeatLaunchInput input;
    input.seat.seatId = seatId;
    input.seat.name = L"Seat " + std::to_wstring(seatId);
    input.seat.displayIds = {L"DISPLAY-" + std::to_wstring(seatId)};
    input.seat.primaryDisplayId = input.seat.displayIds.front();
    input.seat.keyboardIds = {L"KEYBOARD-" + std::to_wstring(seatId)};
    input.seat.mouseIds = {L"MOUSE-" + std::to_wstring(seatId)};
    input.seat.controllerIds = {L"CONTROLLER-" + std::to_wstring(seatId)};
    input.seat.audioOutputEndpointId = L"AUDIO-" + std::to_wstring(seatId);
    input.seat.audioInputEndpointId = L"MIC-" + std::to_wstring(seatId);
    input.seat.targetHwnd = 1000u + seatId;
    input.seat.active = true;

    input.target.gameId = std::move(gameId);
    input.target.process.seatId = seatId;
    input.target.process.executablePath = L"C:\\HydraSeat\\fixture.exe";
    input.target.process.arguments = {L"--seat", std::to_wstring(seatId)};
    input.target.process.workingDirectory = L"C:\\HydraSeat";
    input.target.process.environmentOverrides = {{L"HYDRA_SEAT", std::to_wstring(seatId)}};
    input.target.requirements.display = true;
    input.target.requirements.keyboard = true;
    input.target.requirements.mouse = true;
    input.target.requirements.controller = true;
    input.target.requirements.audioOutput = true;
    input.target.requirements.windowOwnership = true;
    input.target.requirements.recovery = true;
    return input;
}

std::vector<SeatLaunchInput> validInputs() {
    return {makeInput(1, "game-a"), makeInput(2, "game-b")};
}

const SeatActivationPlan* seatPlan(const TwoSeatLaunchPlan& plan, SeatId seatId) {
    const auto found = std::find_if(
        plan.seats.begin(), plan.seats.end(),
        [seatId](const SeatActivationPlan& seat) { return seat.seatId == seatId; });
    return found == plan.seats.end() ? nullptr : &*found;
}

struct SharedResourceState {
    std::map<std::string, bool> active;
    std::vector<std::string> log;
    std::optional<std::pair<SeatId, ResourceKind>> failActivateAfterMutation;
    std::optional<std::pair<SeatId, ResourceKind>> failVerifyActive;
    std::optional<std::pair<SeatId, ResourceKind>> failRollback;
    std::optional<std::pair<SeatId, ResourceKind>> failPrepare;
    std::optional<std::string> failLifecycleBoundary;
    bool failLifecycleRollback{false};
    bool failLifecycleVerifySafe{false};
    bool lifecycleActive{false};
    bool lifecycleRecoveryRequired{false};
    int createCalls{0};
};

bool matches(const std::optional<std::pair<SeatId, ResourceKind>>& configured,
             SeatId seatId, ResourceKind kind) {
    return configured && configured->first == seatId && configured->second == kind;
}

class FakeResource final : public ISeatActivationResource {
public:
    FakeResource(SeatId seatId, ResourceKind kind,
                 std::shared_ptr<SharedResourceState> state)
        : seatId_(seatId), kind_(kind), state_(std::move(state)) {}

    ResourceKind kind() const noexcept override { return kind_; }

    bool prepare(const SeatActivationPlan& plan,
                 const SeatGameBinding& binding,
                 std::string& error) override {
        state_->log.push_back("prepare:" + key(seatId_, kind_));
        if (plan.seatId != seatId_ || binding.gameId != plan.target.gameId) {
            error = "fixture plan/binding mismatch";
            return false;
        }
        if (matches(state_->failPrepare, seatId_, kind_)) {
            error = "injected prepare failure";
            return false;
        }
        prepared_ = true;
        error.clear();
        return true;
    }

    bool activate(std::string& error) override {
        state_->log.push_back("activate:" + key(seatId_, kind_));
        if (!prepared_) {
            error = "activate before prepare";
            return false;
        }
        state_->active[key(seatId_, kind_)] = true;
        if (matches(state_->failActivateAfterMutation, seatId_, kind_)) {
            error = "injected activation failure after mutation";
            return false;
        }
        error.clear();
        return true;
    }

    bool verifyActive(std::string& error) override {
        state_->log.push_back("verify-active:" + key(seatId_, kind_));
        if (matches(state_->failVerifyActive, seatId_, kind_)) {
            error = "injected active verification failure";
            return false;
        }
        if (!active()) {
            error = "resource is not active";
            return false;
        }
        error.clear();
        return true;
    }

    bool rollback(std::string& error) noexcept override {
        state_->log.push_back("rollback:" + key(seatId_, kind_));
        if (matches(state_->failRollback, seatId_, kind_)) {
            error = "injected rollback failure";
            return false;
        }
        state_->active[key(seatId_, kind_)] = false;
        error.clear();
        return true;
    }

    bool verifySafe(std::string& error) noexcept override {
        state_->log.push_back("verify-safe:" + key(seatId_, kind_));
        if (active()) {
            error = "resource remains active";
            return false;
        }
        error.clear();
        return true;
    }

    bool active() const noexcept override {
        const auto found = state_->active.find(key(seatId_, kind_));
        return found != state_->active.end() && found->second;
    }

private:
    SeatId seatId_{0};
    ResourceKind kind_{ResourceKind::Recovery};
    std::shared_ptr<SharedResourceState> state_;
    bool prepared_{false};
};

class FakeResourceFactory final : public ISeatActivationResourceFactory {
public:
    explicit FakeResourceFactory(std::shared_ptr<SharedResourceState> state)
        : state_(std::move(state)) {}

    std::unique_ptr<ISeatActivationResource> create(
        ResourceKind kind, const SeatActivationPlan& plan,
        std::string& error) override {
        ++state_->createCalls;
        error.clear();
        return std::make_unique<FakeResource>(plan.seatId, kind, state_);
    }

private:
    std::shared_ptr<SharedResourceState> state_;
};

class FakeLifecycleHook final : public ISeatActivationLifecycleHook {
public:
    FakeLifecycleHook(SeatId seatId, std::shared_ptr<SharedResourceState> state)
        : seatId_(seatId), state_(std::move(state)) {}

    bool prepare(const SeatActivationPlan& plan,
                 const SeatGameBinding& binding,
                 std::string& error) override {
        state_->log.push_back("lifecycle:prepare:" + std::to_string(seatId_));
        if (plan.seatId != seatId_ || binding.gameId != plan.target.gameId) {
            error = "fixture lifecycle identity mismatch";
            return false;
        }
        prepared_ = true;
        error.clear();
        return true;
    }

    bool preSpawn(std::string& error) override { return boundary("PreSpawn", error); }
    bool startup(std::string& error) override { return boundary("Startup", error); }
    bool postWindow(std::string& error) override { return boundary("PostWindow", error); }
    bool runtime(std::string& error) override { return boundary("Runtime", error); }

    bool rollback(std::string& error) noexcept override {
        state_->log.push_back("lifecycle:rollback:" + std::to_string(seatId_));
        if (state_->failLifecycleRollback) {
            state_->lifecycleRecoveryRequired = true;
            error = "injected lifecycle rollback failure";
            return false;
        }
        state_->lifecycleActive = false;
        state_->lifecycleRecoveryRequired = false;
        error.clear();
        return true;
    }

    bool verifySafe(std::string& error) noexcept override {
        state_->log.push_back("lifecycle:verify-safe:" + std::to_string(seatId_));
        if (state_->failLifecycleVerifySafe || state_->lifecycleActive) {
            state_->lifecycleRecoveryRequired = true;
            error = "fixture lifecycle is not verified safe";
            return false;
        }
        error.clear();
        return true;
    }

    bool recoveryRequired() const noexcept override {
        return state_->lifecycleRecoveryRequired;
    }

private:
    bool boundary(std::string_view name, std::string& error) {
        state_->log.push_back("lifecycle:" + std::string(name) + ":" +
                              std::to_string(seatId_));
        if (!prepared_) {
            error = "lifecycle boundary before prepare";
            return false;
        }
        state_->lifecycleActive = true;
        if (state_->failLifecycleBoundary && *state_->failLifecycleBoundary == name) {
            error = "injected lifecycle boundary failure";
            return false;
        }
        error.clear();
        return true;
    }

    SeatId seatId_{0};
    std::shared_ptr<SharedResourceState> state_;
    bool prepared_{false};
};

bool allSeatResourcesInactive(const SharedResourceState& state, SeatId seatId,
                              const SeatActivationPlan& plan) {
    return std::all_of(plan.resources.begin(), plan.resources.end(),
                       [&](ResourceKind kind) {
                           const auto found = state.active.find(key(seatId, kind));
                           return found == state.active.end() || !found->second;
                       });
}

bool allSeatResourcesActive(const SharedResourceState& state, SeatId seatId,
                            const SeatActivationPlan& plan) {
    return std::all_of(plan.resources.begin(), plan.resources.end(),
                       [&](ResourceKind kind) {
                           const auto found = state.active.find(key(seatId, kind));
                           return found != state.active.end() && found->second;
                       });
}

void forceProcessExited(SharedResourceState& state, SeatId seatId) {
    state.active[key(seatId, ResourceKind::Process)] = false;
}

std::size_t eventPosition(const SharedResourceState& state, std::string_view event) {
    const auto found = std::find(state.log.begin(), state.log.end(), event);
    return found == state.log.end() ? state.log.size() :
                                     static_cast<std::size_t>(found - state.log.begin());
}

SeatGameBinding bindingFor(const SeatActivationPlan& seat, std::string player) {
    return {std::move(player), seat.target.gameId};
}

void testCompileDeterminismAndTransientStateRemoval() {
    auto inputs = validInputs();
    auto first = compileTwoSeatLaunchPlan(inputs);
    check(first.succeeded(), "valid exactly-two-Seat launch inputs compile");
    if (!first.plan) return;
    check(first.plan->seats.size() == 2 && first.plan->seats[0].seatId == 1 &&
              first.plan->seats[1].seatId == 2 && first.plan->fingerprint != 0,
          "compiled plan is canonical by Seat ID and fingerprinted");
    check(first.plan->seats[0].seat.targetHwnd == 0 &&
              first.plan->seats[1].seat.targetHwnd == 0,
          "legacy transient HWND is stripped from the immutable launch plan");

    std::reverse(inputs.begin(), inputs.end());
    inputs[0].seat.targetHwnd = 999999u;
    inputs[1].seat.targetHwnd = 888888u;
    auto second = compileTwoSeatLaunchPlan(inputs);
    check(second.succeeded() && second.plan &&
              second.plan->fingerprint == first.plan->fingerprint &&
              second.plan->seats == first.plan->seats,
          "input order and stale HWND do not change the canonical immutable plan");

    auto changed = validInputs();
    changed[0].target.capabilities.audio = false;
    changed[0].target.requirements.audioOutput = false;
    auto changedPlan = compileTwoSeatLaunchPlan(changed);
    check(changedPlan.succeeded() && changedPlan.plan &&
              changedPlan.plan->fingerprint != first.plan->fingerprint,
          "capability changes are covered by the immutable plan fingerprint");
}

void testCompilePreflightFailsBeforeMutation() {
    auto inputs = validInputs();
    inputs[1].seat.keyboardIds.clear();
    auto missing = compileTwoSeatLaunchPlan(inputs);
    check(!missing.succeeded() &&
              std::any_of(missing.issues.begin(), missing.issues.end(),
                          [](const CompileIssue& issue) {
                              return issue.code == CompileIssueCode::MissingKeyboard;
                          }),
          "missing keyboard fails only when the game declares keyboard required");

    inputs = validInputs();
    inputs[1].target.requirements.keyboard = false;
    inputs[1].seat.keyboardIds.clear();
    check(compileTwoSeatLaunchPlan(inputs).succeeded(),
          "Seat may omit keyboard when the selected game does not require one");

    inputs = validInputs();
    inputs[1].seat.primaryDisplayId = L"NOT-IN-GROUP";
    check(!compileTwoSeatLaunchPlan(inputs).succeeded(),
          "primary display must belong to the Seat display group");

    inputs = validInputs();
    inputs[1].seat.displayIds = inputs[0].seat.displayIds;
    inputs[1].seat.primaryDisplayId = inputs[1].seat.displayIds.front();
    check(!compileTwoSeatLaunchPlan(inputs).succeeded(),
          "exclusive required display cannot be shared across launch Seats");

    inputs = validInputs();
    inputs[1].seat.audioOutputEndpointId = inputs[0].seat.audioOutputEndpointId;
    auto duplicateAudio = compileTwoSeatLaunchPlan(inputs);
    check(!duplicateAudio.succeeded() &&
              std::any_of(duplicateAudio.issues.begin(), duplicateAudio.issues.end(),
                          [](const CompileIssue& issue) {
                              return issue.code == CompileIssueCode::DuplicateExclusiveAudioOutput;
                          }),
          "required per-Seat audio output cannot silently alias the same endpoint");

    inputs = validInputs();
    inputs[0].target.capabilities.controller = false;
    check(!compileTwoSeatLaunchPlan(inputs).succeeded(),
          "required unsupported controller capability fails before resource creation");

    inputs = validInputs();
    inputs[0].target.requirements.highRisk = true;
    check(!compileTwoSeatLaunchPlan(inputs).succeeded(),
          "high-risk launch options require explicit approval at plan compile time");
    inputs[0].target.highRiskApproved = true;
    check(compileTwoSeatLaunchPlan(inputs).succeeded(),
          "explicit high-risk approval allows the otherwise valid plan to compile");

    const auto oneSeat = std::vector<SeatLaunchInput>{makeInput(1, "one")};
    check(!compileTwoSeatLaunchPlan(oneSeat).succeeded(),
          "P5 launch plan requires exactly two active Seats");
    auto three = validInputs();
    three.push_back(makeInput(3, "three"));
    check(!compileTwoSeatLaunchPlan(three).succeeded(),
          "third active Seat is rejected before activation");
}

void testEveryActivationFailureRollsBackInReverse() {
    const auto compiled = compileTwoSeatLaunchPlan(validInputs());
    check(compiled.plan.has_value(), "failure-index fixture compiles");
    if (!compiled.plan) return;
    const auto* plan = seatPlan(*compiled.plan, 2);
    if (plan == nullptr) return;

    for (const auto failKind : plan->resources) {
        auto state = std::make_shared<SharedResourceState>();
        state->failActivateAfterMutation = std::make_pair(SeatId{2}, failKind);
        auto factory = std::make_shared<FakeResourceFactory>(state);
        PlannedSeatGameInstance instance(*plan, factory);
        std::string error;
        const bool started = instance.start(bindingFor(*plan, "luigi"), error);
        check(!started && !error.empty(),
              "each injected activation failure is surfaced");
        check(allSeatResourcesInactive(*state, 2, *plan),
              "each activation failure restores every Seat-local resource to safe state");
        check(instance.verifyStopped(error),
              "rolled-back failed activation verifies stopped");

        std::vector<ResourceKind> rollbackKinds;
        for (const auto& event : state->log) {
            if (!event.starts_with("rollback:2:")) continue;
            const auto name = event.substr(std::string("rollback:2:").size());
            const auto found = std::find_if(
                plan->resources.begin(), plan->resources.end(),
                [&](ResourceKind kind) { return resourceKindName(kind) == name; });
            if (found != plan->resources.end()) rollbackKinds.push_back(*found);
        }
        const std::vector<ResourceKind> expected(plan->resources.rbegin(),
                                                 plan->resources.rend());
        check(rollbackKinds == expected,
              "failure cleanup invokes resource rollback in exact reverse activation order");
    }
}

void testRollbackFailureRetainsRecoveryOwnership() {
    const auto compiled = compileTwoSeatLaunchPlan(validInputs());
    if (!compiled.plan) return;
    const auto* plan = seatPlan(*compiled.plan, 2);
    if (plan == nullptr) return;
    auto state = std::make_shared<SharedResourceState>();
    state->failActivateAfterMutation = std::make_pair(SeatId{2}, ResourceKind::Controller);
    state->failRollback = std::make_pair(SeatId{2}, ResourceKind::Controller);
    auto factory = std::make_shared<FakeResourceFactory>(state);
    PlannedSeatGameInstance instance(*plan, factory);
    std::string error;
    check(!instance.start(bindingFor(*plan, "luigi"), error),
          "fixture activation fails with injected rollback failure");
    check(!instance.verifyStopped(error),
          "unverified rollback is not converted to a stopped result");
    check(std::find(state->log.begin(), state->log.end(),
                    "verify-safe:2:controller") != state->log.end(),
          "rollback failure still runs explicit safe-state verification");
    check(state->active[key(2, ResourceKind::Controller)],
          "failed rollback retains exact active resource ownership for recovery retry");
    state->failRollback.reset();
    check(instance.stop(error) && instance.verifyStopped(error),
          "later Seat-local recovery retry can clean the retained exact resource");
}

void testCompatibilityLifecycleBoundariesAndReverseRollback() {
    const auto compiled = compileTwoSeatLaunchPlan(validInputs());
    if (!compiled.plan) return;
    const auto* plan = seatPlan(*compiled.plan, 2);
    if (plan == nullptr) return;

    {
        auto state = std::make_shared<SharedResourceState>();
        auto factory = std::make_shared<FakeResourceFactory>(state);
        PlannedSeatGameInstance instance(
            *plan, factory, std::make_unique<FakeLifecycleHook>(2, state));
        std::string error;
        check(instance.start(bindingFor(*plan, "luigi"), error),
              "compatibility lifecycle hook allows a valid Seat activation");
        check(eventPosition(*state, "lifecycle:PreSpawn:2") <
                  eventPosition(*state, "activate:2:process") &&
              eventPosition(*state, "verify-active:2:process") <
                  eventPosition(*state, "lifecycle:Startup:2") &&
              eventPosition(*state, "verify-active:2:window") <
                  eventPosition(*state, "lifecycle:PostWindow:2") &&
              eventPosition(*state, "verify-active:2:audio") <
                  eventPosition(*state, "lifecycle:Runtime:2"),
              "compatibility phases map to the exact Process/Window/final resource boundaries");
        check(instance.stop(error) && instance.verifyStopped(error),
              "successful lifecycle activation rolls back and verifies safe on stop");
        check(eventPosition(*state, "rollback:2:process") <
                  eventPosition(*state, "lifecycle:rollback:2") &&
              eventPosition(*state, "lifecycle:rollback:2") <
                  eventPosition(*state, "rollback:2:recovery"),
              "reverse rollback stops Process before compatibility files and Recovery after them");
    }

    const auto runLifecycleFailure = [&](std::string boundary,
                                         std::string forbiddenResourceEvent) {
        auto state = std::make_shared<SharedResourceState>();
        state->failLifecycleBoundary = boundary;
        auto factory = std::make_shared<FakeResourceFactory>(state);
        PlannedSeatGameInstance instance(
            *plan, factory, std::make_unique<FakeLifecycleHook>(2, state));
        std::string error;
        check(!instance.start(bindingFor(*plan, "luigi"), error),
              "injected compatibility lifecycle phase failure is surfaced");
        check(eventPosition(*state, forbiddenResourceEvent) == state->log.size(),
              "failed compatibility phase prevents the next lifecycle resource mutation");
        check(instance.verifyStopped(error),
              "lifecycle phase failure is reversed and verifies stopped");
    };

    runLifecycleFailure("PreSpawn", "activate:2:process");
    runLifecycleFailure("Startup", "activate:2:window");
    runLifecycleFailure("PostWindow", "activate:2:display");
    runLifecycleFailure("Runtime", "missing:runtime-has-no-next-resource");

    {
        auto state = std::make_shared<SharedResourceState>();
        state->failActivateAfterMutation = std::make_pair(SeatId{2}, ResourceKind::Process);
        auto factory = std::make_shared<FakeResourceFactory>(state);
        PlannedSeatGameInstance instance(
            *plan, factory, std::make_unique<FakeLifecycleHook>(2, state));
        std::string error;
        check(!instance.start(bindingFor(*plan, "luigi"), error) &&
                  eventPosition(*state, "lifecycle:PreSpawn:2") <
                      eventPosition(*state, "activate:2:process") &&
                  eventPosition(*state, "lifecycle:Startup:2") == state->log.size(),
              "process failure happens after PreSpawn and before Startup");
        check(instance.verifyStopped(error),
              "process failure after PreSpawn reverses compatibility state");
    }

    {
        auto state = std::make_shared<SharedResourceState>();
        state->failActivateAfterMutation = std::make_pair(SeatId{2}, ResourceKind::Window);
        auto factory = std::make_shared<FakeResourceFactory>(state);
        PlannedSeatGameInstance instance(
            *plan, factory, std::make_unique<FakeLifecycleHook>(2, state));
        std::string error;
        check(!instance.start(bindingFor(*plan, "luigi"), error) &&
                  eventPosition(*state, "lifecycle:Startup:2") <
                      eventPosition(*state, "activate:2:window") &&
                  eventPosition(*state, "lifecycle:PostWindow:2") == state->log.size(),
              "window failure happens after Startup and before PostWindow");
        check(instance.verifyStopped(error),
              "window failure after Startup reverses compatibility state");
    }

    for (const auto lateKind : {ResourceKind::Input, ResourceKind::Audio}) {
        auto state = std::make_shared<SharedResourceState>();
        state->failActivateAfterMutation = std::make_pair(SeatId{2}, lateKind);
        auto factory = std::make_shared<FakeResourceFactory>(state);
        PlannedSeatGameInstance instance(
            *plan, factory, std::make_unique<FakeLifecycleHook>(2, state));
        std::string error;
        const auto activationEvent = std::string("activate:2:") +
                                     std::string(resourceKindName(lateKind));
        check(!instance.start(bindingFor(*plan, "luigi"), error) &&
                  eventPosition(*state, "lifecycle:PostWindow:2") <
                      eventPosition(*state, activationEvent) &&
                  eventPosition(*state, "lifecycle:Runtime:2") == state->log.size(),
              "Input/Audio failure happens after PostWindow and before Runtime");
        check(instance.verifyStopped(error),
              "later Input/Audio failure after PostWindow reverses compatibility state");
    }

    {
        auto state = std::make_shared<SharedResourceState>();
        state->failLifecycleBoundary = "Runtime";
        state->failLifecycleRollback = true;
        auto factory = std::make_shared<FakeResourceFactory>(state);
        PlannedSeatGameInstance instance(
            *plan, factory, std::make_unique<FakeLifecycleHook>(2, state));
        std::string error;
        check(!instance.start(bindingFor(*plan, "luigi"), error),
              "Runtime failure with injected compatibility rollback failure is surfaced");
        check(!instance.verifyStopped(error) && state->lifecycleRecoveryRequired,
              "unverifiable compatibility rollback remains RecoveryRequired");
        state->failLifecycleRollback = false;
        state->lifecycleRecoveryRequired = false;
        check(instance.stop(error) && instance.verifyStopped(error),
              "retained compatibility recovery ownership supports a later cleanup retry");
    }
}


} // namespace

int main() {
    testCompileDeterminismAndTransientStateRemoval();
    testCompilePreflightFailsBeforeMutation();
    testEveryActivationFailureRollsBackInReverse();
    testRollbackFailureRetainsRecoveryOwnership();
    testCompatibilityLifecycleBoundariesAndReverseRollback();
    if (failures != 0) {
        std::cerr << failures << " two-seat launch foundation test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Two-seat launch foundation tests passed.\n";
    return EXIT_SUCCESS;
}
