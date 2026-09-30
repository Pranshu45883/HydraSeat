#include "hydra/seat_game_lifecycle.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace hydra::runtime;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct FakeInstance final : ISeatGameInstance {
    hydra::SeatId seatId{0};
    bool live{false};
    bool failStart{false};
    bool throwStart{false};
    bool failStop{false};
    bool failVerify{false};
    unsigned stopFailuresRemaining{0};
    bool blockStart{false};
    std::mutex mutex;
    std::condition_variable condition;
    bool startEntered{false};
    bool releaseStart{false};

    bool start(const SeatGameBinding&, std::string& error) override {
        if (blockStart) {
            std::unique_lock lock(mutex);
            startEntered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return releaseStart; });
        }
        if (throwStart) throw std::runtime_error("injected start exception");
        if (failStart) {
            error = "injected start failure";
            return false;
        }
        live = true;
        return true;
    }

    bool stop(std::string& error) noexcept override {
        if (stopFailuresRemaining != 0u) {
            --stopFailuresRemaining;
            error = "injected transient Seat-local stop failure";
            return false;
        }
        if (failStop) {
            error = "injected Seat-local stop failure";
            return false;
        }
        live = false;
        return true;
    }

    bool verifyStopped(std::string& error) noexcept override {
        if (failVerify || live) {
            error = "injected stopped-state verification failure";
            return false;
        }
        return true;
    }

    bool running() const noexcept override { return live; }
};

struct FakeFactory final : ISeatGameInstanceFactory {
    std::vector<FakeInstance*> created;
    std::mutex mutex;
    std::condition_variable condition;
    bool failCreate{false};
    bool throwCreate{false};
    bool nextFailStart{false};
    bool nextThrowStart{false};
    bool nextBlockStart{false};
    unsigned nextStopFailures{0};

    std::unique_ptr<ISeatGameInstance> create(hydra::SeatId seatId,
                                               std::string& error) override {
        if (throwCreate) throw std::runtime_error("injected factory exception");
        if (failCreate) {
            error = "injected factory failure";
            return {};
        }
        auto instance = std::make_unique<FakeInstance>();
        instance->seatId = seatId;
        instance->failStart = nextFailStart;
        instance->throwStart = nextThrowStart;
        instance->blockStart = nextBlockStart;
        instance->stopFailuresRemaining = nextStopFailures;
        nextFailStart = false;
        nextThrowStart = false;
        nextBlockStart = false;
        nextStopFailures = 0;
        {
            std::lock_guard lock(mutex);
            created.push_back(instance.get());
            condition.notify_all();
        }
        return instance;
    }
};

SeatGameBinding binding(std::string player, std::string game) {
    return {std::move(player), std::move(game)};
}

const SeatGameState* seat(const SeatGameCommandResult& result, hydra::SeatId id) {
    for (const auto& value : result.seats) {
        if (value.seatId == id) return &value;
    }
    return nullptr;
}

void testIndependentStartStopRestartAndReturnPolicy() {
    auto factory = std::make_shared<FakeFactory>();
    const std::vector<hydra::SeatId> ids{1, 2};
    SeatGameLifecycle lifecycle(ids, factory);

    check(lifecycle.assign(1, binding("player-a", "game-a"), 1).succeeded(),
          "Seat 1 accepts an Idle-only temporary binding");
    auto firstPlaying = lifecycle.start(1, 2);
    check(firstPlaying.succeeded() && seat(firstPlaying, 1)->phase == SeatGamePhase::Playing &&
              seat(firstPlaying, 2)->phase == SeatGamePhase::Idle &&
              !firstPlaying.wholeMachineReturnRequested,
          "Seat 1 Playing / Seat 2 Idle is healthy without global return");
    auto* firstInstance = factory->created.back();

    check(lifecycle.assign(2, binding("player-b", "game-b"), 3).succeeded(),
          "Seat 2 plans independently while Seat 1 plays");
    auto bothPlaying = lifecycle.start(2, 4);
    check(bothPlaying.succeeded() && seat(bothPlaying, 1)->phase == SeatGamePhase::Playing &&
              seat(bothPlaying, 2)->phase == SeatGamePhase::Playing,
          "both controlled Seat games play independently");
    const auto instancesBeforeStop = factory->created.size();

    auto secondStopped = lifecycle.stop(2, 5);
    check(secondStopped.succeeded() && firstInstance->live &&
              seat(secondStopped, 1)->phase == SeatGamePhase::Playing &&
              seat(secondStopped, 2)->phase == SeatGamePhase::Idle &&
              !secondStopped.wholeMachineReturnRequested,
          "stopping Seat 2 preserves Seat 1 exact live instance and global resources");

    check(lifecycle.assign(2, binding("player-c", "game-c"), 6).succeeded() &&
              lifecycle.start(2, 7).succeeded(),
          "idle Seat 2 restarts with a different temporary Player/Game binding");
    check(factory->created.size() == instancesBeforeStop + 1u && firstInstance->live,
          "Seat restart creates new Seat-local ownership without replacing Seat 1");

    check(lifecycle.stop(2, 8).succeeded(), "restarted Seat 2 stops cleanly");
    auto bothEnded = lifecycle.stop(1, 9);
    check(bothEnded.succeeded() && bothEnded.wholeMachineReturnRequested &&
              seat(bothEnded, 1)->phase == SeatGamePhase::Idle &&
              seat(bothEnded, 2)->phase == SeatGamePhase::Idle,
          "only both-ended state requests declared whole-machine return");
}

void testExitFaultDuplicateAndMalformedBindings() {
    auto factory = std::make_shared<FakeFactory>();
    const std::vector<hydra::SeatId> ids{1, 2};
    SeatGameLifecycle lifecycle(ids, factory);

    check(lifecycle.assign(1, binding("", "game"), 10).code ==
              SeatGameResultCode::InvalidBinding,
          "empty Player identity fails closed");
    check(lifecycle.assign(1, binding("player", std::string(257, 'g')), 11).code ==
              SeatGameResultCode::InvalidBinding,
          "oversized Game identity fails closed");
    check(lifecycle.assign(1, binding("player-a", "game-a"), 12).succeeded(),
          "valid binding survives prior malformed input");
    check(lifecycle.start(1, 13).succeeded(), "Seat 1 starts for exit observation");
    auto* first = factory->created.back();
    first->live = false;
    auto cleanExit = lifecycle.observeTargetExit(1, true, {}, 14);
    check(cleanExit.succeeded() && seat(cleanExit, 1)->phase == SeatGamePhase::Idle,
          "normal exact target exit returns only its Seat to Idle");

    check(lifecycle.assign(1, binding("player-a", "game-a"), 15).succeeded() &&
              lifecycle.start(1, 16).succeeded() &&
              lifecycle.assign(2, binding("player-b", "game-b"), 17).succeeded() &&
              lifecycle.start(2, 18).succeeded(),
          "both Seats restart for fault isolation");
    auto* seatOne = factory->created[1];
    auto* seatTwo = factory->created[2];
    seatTwo->live = false;
    auto fault = lifecycle.observeTargetExit(2, false, "synthetic crash", 19);
    check(fault.code == SeatGameResultCode::BackendFailure &&
              seat(fault, 2)->phase == SeatGamePhase::Degraded && seatOne->live &&
              seat(fault, 1)->phase == SeatGamePhase::Playing &&
              !fault.wholeMachineReturnRequested,
          "one Seat fault degrades only that Seat and preserves the healthy Seat");

    // Seat 1 is generation two after its restart above. A delayed generation-one
    // process-exit signal must not be allowed to clean the current instance even
    // if that current process happens to be observed exited at the same moment.
    seatOne->live = false;
    const auto staleExit = lifecycle.observeTargetExit(
        1, 1u, false, "delayed old-generation exit", 20);
    check(staleExit.code == SeatGameResultCode::InvalidState &&
              seat(staleExit, 1)->phase == SeatGamePhase::Playing &&
              seat(staleExit, 1)->generation == 2u,
          "stale target-exit generation cannot consume a newer Seat game instance");
    seatOne->live = true;

    const auto acknowledgedFault = lifecycle.stop(2, 21);
    check(acknowledgedFault.succeeded() &&
              seat(acknowledgedFault, 2)->phase == SeatGamePhase::Idle && seatOne->live,
          "verified exited degraded Seat can return to Idle without stopping healthy Seat");

    const auto duplicate = lifecycle.stop(1, 19);
    check(duplicate.code == SeatGameResultCode::DuplicateCorrelation && seatOne->live,
          "duplicate correlation cannot replay a mutation");
    check(lifecycle.stop(99, 22).code == SeatGameResultCode::InvalidSeat && seatOne->live,
          "unknown Seat command cannot affect an owned process");
}

void testFailuresConcurrencyAndV1Limit() {
    auto factory = std::make_shared<FakeFactory>();
    const std::vector<hydra::SeatId> ids{1, 2};
    SeatGameLifecycle lifecycle(ids, factory);

    factory->nextFailStart = true;
    check(lifecycle.assign(1, binding("player-a", "bad-game"), 30).succeeded(),
          "failing game first reaches planned state");
    const auto failed = lifecycle.start(1, 31);
    check(failed.code == SeatGameResultCode::BackendFailure &&
              seat(failed, 1)->phase == SeatGamePhase::Idle &&
              !seat(failed, 1)->binding,
          "partial start failure rolls Seat-local state back to Idle");

    factory->throwCreate = true;
    check(lifecycle.assign(1, binding("player-a", "throwing-factory"), 44).succeeded(),
          "throwing factory case reaches planned state");
    const auto createException = lifecycle.start(1, 45);
    check(createException.code == SeatGameResultCode::BackendFailure &&
              seat(createException, 1)->phase == SeatGamePhase::Idle &&
              createException.diagnostic.find("factory exception") != std::string::npos,
          "factory exception is isolated as a Seat-local backend failure");
    factory->throwCreate = false;

    factory->nextThrowStart = true;
    check(lifecycle.assign(1, binding("player-a", "throwing-start"), 46).succeeded(),
          "throwing start case reaches planned state");
    const auto startException = lifecycle.start(1, 47);
    check(startException.code == SeatGameResultCode::BackendFailure &&
              seat(startException, 1)->phase == SeatGamePhase::Idle &&
              startException.diagnostic.find("start exception") != std::string::npos,
          "start exception is contained and cleaned without terminating the Host");

    factory->nextFailStart = true;
    factory->nextStopFailures = 1;
    check(lifecycle.assign(2, binding("player-b", "partial-game"), 41).succeeded(),
          "partial Seat 2 start reaches planned state");
    const auto retained = lifecycle.start(2, 42);
    check(retained.code == SeatGameResultCode::RecoveryRequired &&
              seat(retained, 2)->phase == SeatGamePhase::RecoveryRequired &&
              retained.diagnostic.find("retained for cleanup retry") != std::string::npos,
          "failed start cleanup retains exact Seat-local instance ownership");
    const auto retriedCleanup = lifecycle.stop(2, 43);
    check(retriedCleanup.succeeded() &&
              seat(retriedCleanup, 2)->phase == SeatGamePhase::Idle &&
              !seat(retriedCleanup, 2)->binding,
          "Seat-local stop retries and verifies retained partial-start cleanup");

    factory->nextBlockStart = true;
    check(lifecycle.assign(1, binding("player-a", "slow-game"), 32).succeeded(),
          "blocking start precondition plans");
    const auto createdBeforeBlockingStart = factory->created.size();
    std::thread starter([&] { (void)lifecycle.start(1, 33); });
    FakeInstance* blocking = nullptr;
    {
        std::unique_lock lock(factory->mutex);
        factory->condition.wait(lock, [&] {
            return factory->created.size() > createdBeforeBlockingStart;
        });
        blocking = factory->created.back();
    }
    {
        std::unique_lock lock(blocking->mutex);
        blocking->condition.wait(lock, [&] { return blocking->startEntered; });
    }
    const auto busy = lifecycle.stop(2, 34);
    check(busy.code == SeatGameResultCode::Busy,
          "simultaneous Seat mutation is rejected while single writer is active");
    {
        std::lock_guard lock(blocking->mutex);
        blocking->releaseStart = true;
        blocking->condition.notify_all();
    }
    starter.join();

    auto failingStopInstance = blocking;
    failingStopInstance->failStop = true;
    const auto unverified = lifecycle.stop(1, 35);
    check(unverified.code == SeatGameResultCode::RecoveryRequired &&
              seat(unverified, 1)->phase == SeatGamePhase::RecoveryRequired,
          "unverified Seat-local cleanup enters RecoveryRequired");
    const auto emergencyFailure = lifecycle.emergencyStopAll(36);
    check(emergencyFailure.code == SeatGameResultCode::RecoveryRequired &&
              seat(emergencyFailure, 1)->binding.has_value() &&
              seat(emergencyFailure, 2)->phase == SeatGamePhase::Idle,
          "failed emergency cleanup preserves exact failing binding while safe Seat stays Idle");

    auto limitFactory = std::make_shared<FakeFactory>();
    const std::vector<hydra::SeatId> three{1, 2, 3};
    SeatGameLifecycle tooMany(three, limitFactory);
    const auto rejected = tooMany.assign(1, binding("player", "game"), 40);
    check(rejected.code == SeatGameResultCode::V1SeatLimitExceeded &&
              rejected.seats.empty() && limitFactory->created.empty(),
          "third active v1 Seat is rejected before any runtime instance exists");
}

void testInvalidConfigurationFailsClosedConsistently() {
    auto factory = std::make_shared<FakeFactory>();

    const std::vector<hydra::SeatId> none;
    SeatGameLifecycle empty(none, factory);
    check(empty.assign(1, binding("player", "game"), 50).code == SeatGameResultCode::InvalidSeat,
          "empty Seat configuration reports InvalidSeat on assign");
    check(empty.start(1, 51).code == SeatGameResultCode::InvalidSeat,
          "empty Seat configuration reports InvalidSeat on start");
    check(empty.stop(1, 52).code == SeatGameResultCode::InvalidSeat,
          "empty Seat configuration reports InvalidSeat on stop");
    check(empty.reconcile(53).code == SeatGameResultCode::InvalidSeat,
          "empty Seat configuration rejects reconcile consistently");
    const auto emptyEmergency = empty.emergencyStopAll(54);
    check(emptyEmergency.code == SeatGameResultCode::InvalidSeat &&
              !emptyEmergency.wholeMachineReturnRequested,
          "invalid empty configuration cannot claim whole-machine return readiness");

    const std::vector<hydra::SeatId> duplicate{1, 1};
    SeatGameLifecycle duplicateSeats(duplicate, factory);
    check(duplicateSeats.start(1, 55).code == SeatGameResultCode::InvalidSeat,
          "duplicate Seat identifiers are not misreported as the v1 Seat-count limit");

    const std::vector<hydra::SeatId> validIds{1, 2};
    SeatGameLifecycle missingFactory(validIds, {});
    check(missingFactory.assign(1, binding("player", "game"), 56).code ==
              SeatGameResultCode::BackendFailure,
          "missing lifecycle factory is reported as a backend configuration failure");
    check(missingFactory.start(1, 57).code == SeatGameResultCode::BackendFailure &&
              missingFactory.stop(1, 58).code == SeatGameResultCode::BackendFailure &&
              missingFactory.reconcile(59).code == SeatGameResultCode::BackendFailure,
          "missing lifecycle factory fails closed consistently across mutations");
    const auto factoryEmergency = missingFactory.emergencyStopAll(60);
    check(factoryEmergency.code == SeatGameResultCode::BackendFailure &&
              !factoryEmergency.wholeMachineReturnRequested,
          "missing lifecycle factory cannot claim whole-machine return readiness");
}

} // namespace

int main() {
    testIndependentStartStopRestartAndReturnPolicy();
    testExitFaultDuplicateAndMalformedBindings();
    testFailuresConcurrencyAndV1Limit();
    testInvalidConfigurationFailsClosedConsistently();

    if (failures != 0) {
        std::cerr << failures << " Seat game lifecycle test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Seat game lifecycle tests passed.\n";
    return EXIT_SUCCESS;
}
