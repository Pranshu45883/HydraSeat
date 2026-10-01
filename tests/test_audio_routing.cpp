#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/audio_routing.hpp"

#include <cassert>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::audio;

std::vector<windows::AudioRenderEndpoint> endpoints() {
    return {
        {L"OUT-A", std::nullopt, L"Output A", windows::AudioEndpointState::Active},
        {L"OUT-B", std::nullopt, L"Output B", windows::AudioEndpointState::Active},
        {L"OUT-OFF", std::nullopt, L"Offline", windows::AudioEndpointState::Unplugged},
    };
}

process::ProcessIdentity processIdentity(
    std::uint32_t pid,
    std::uint64_t creation,
    std::wstring path = L"C:\\Games\\game.exe") {
    return {pid, creation, std::move(path)};
}

process::ProcessTreeSnapshot processTree() {
    process::ProcessTreeSnapshot tree;
    tree.seatId = 1;
    tree.capability = process::ChildTrackingCapability::FullJobObject;
    tree.root = processIdentity(100, 1000);
    process::ProcessRecord root;
    root.identity = tree.root;
    root.root = true;
    tree.processes.push_back(root);

    process::ProcessRecord child;
    child.identity = processIdentity(101, 1001, L"C:\\Games\\helper.exe");
    child.parentProcessId = 100;
    child.root = false;
    tree.processes.push_back(child);
    tree.sequence = 3;
    tree.trackingComplete = true;
    return tree;
}

windows::AudioSessionObservation session(
    std::wstring endpoint,
    std::uint32_t pid,
    std::uint64_t creation,
    bool exact = true) {
    windows::AudioSessionObservation result;
    result.endpointId = std::move(endpoint);
    result.processId = pid;
    if (exact) {
        result.processIdentity =
            hydra::runtime::ProcessIdentity{pid, creation};
    }
    result.state = windows::AudioSessionState::Active;
    return result;
}

class FakeSessionSource final : public SessionObservationSource {
public:
    std::vector<windows::AudioSessionObservation> records;
    bool complete{true};
    bool fail{false};

    windows::AudioSessionInventoryResult enumerate() noexcept override {
        windows::AudioSessionInventoryResult result;
        result.sessions = records;
        result.isComplete = complete;
        if (fail) {
            result.error = windows::AudioSessionObserverError{
                windows::AudioSessionObserverError::Code::UnexpectedWindowsState,
                -1};
        }
        return result;
    }
};

enum class MutableMode {
    Normal,
    FailAfterMutation,
    PretendApplied,
    RollbackFailure,
};

class FakeMutableBackend final : public RouteBackend {
public:
    FakeMutableBackend(
        std::shared_ptr<FakeSessionSource> source,
        MutableMode mode = MutableMode::Normal)
        : source_(std::move(source)), mode_(mode) {}

    RouteBackendKind kind() const noexcept override {
        return RouteBackendKind::ProviderManaged;
    }

    RouteCapability capability(
        const RouteRequest&,
        const OwnedSessionEvidence&) const noexcept override {
        return RouteCapability::Mutable;
    }

    bool captureState(
        const RouteRequest& request,
        const OwnedSessionEvidence& evidence,
        BackendState& state,
        std::string& error) noexcept override {
        captured_.clear();
        for (const auto& item : evidence.ownedSessions) {
            assert(item.processIdentity.has_value());
            captured_[{
                item.processIdentity->pid,
                item.processIdentity->creationIdentity}] = item.endpointId;
        }
        capturedSeat_ = request.seatId;
        state.opaque = {1, 2, 3, 4};
        error.clear();
        return true;
    }

    bool apply(
        const RouteRequest& request,
        const BackendState& before,
        std::string& error) noexcept override {
        if (before.opaque != std::vector<std::uint8_t>({1, 2, 3, 4}) ||
            capturedSeat_ != request.seatId) {
            error = "capture mismatch";
            return false;
        }
        if (mode_ != MutableMode::PretendApplied) {
            for (auto& item : source_->records) {
                if (sessionMatchesOwnedProcess(request.processTree, item)) {
                    item.endpointId = request.targetEndpointId;
                }
            }
        }
        if (mode_ == MutableMode::FailAfterMutation) {
            error = "injected apply failure";
            return false;
        }
        error.clear();
        return true;
    }

    bool rollback(
        const RouteRequest& request,
        const BackendState&,
        std::string& error) noexcept override {
        if (mode_ == MutableMode::RollbackFailure) {
            error = "injected rollback failure";
            return false;
        }
        for (auto& item : source_->records) {
            if (!sessionMatchesOwnedProcess(request.processTree, item) ||
                !item.processIdentity) {
                continue;
            }
            const auto found = captured_.find({
                item.processIdentity->pid,
                item.processIdentity->creationIdentity});
            if (found != captured_.end()) {
                item.endpointId = found->second;
            }
        }
        error.clear();
        return true;
    }

private:
    std::shared_ptr<FakeSessionSource> source_;
    MutableMode mode_;
    std::map<
        std::pair<std::uint32_t, std::uint64_t>,
        std::wstring> captured_;
    SeatId capturedSeat_{0};
};

RouteRequest request(std::wstring endpoint = L"OUT-B") {
    RouteRequest value;
    value.seatId = 1;
    value.processTree = processTree();
    value.targetEndpointId = std::move(endpoint);
    return value;
}

void testObserveOnlyAndExactOwnership() {
    auto source = std::make_shared<FakeSessionSource>();
    RouteTransaction transaction(
        request(), source, std::make_shared<ObserveOnlyRouteBackend>());

    std::string error;
    auto status = transaction.attempt(endpoints(), &error);
    assert(status.phase == RoutePhase::WaitingForSession);

    source->records = {session(L"OUT-B", 100, 1000)};
    status = transaction.attempt(endpoints(), &error);
    assert(status.phase == RoutePhase::Satisfied);
    assert(status.capability == RouteCapability::SatisfiedWithoutMutation);

    source->records = {session(L"OUT-A", 100, 1000, false)};
    status = transaction.attempt(endpoints(), &error);
    assert(status.phase == RoutePhase::Failed);
    assert(status.error == RouteError::OwnershipUnverified);

    source->records = {
        session(L"OUT-A", 100, 1000),
        session(L"OUT-A", 100, 1000),
    };
    status = transaction.attempt(endpoints(), &error);
    assert(status.phase == RoutePhase::Failed);
    assert(status.error == RouteError::OwnershipUnverified);
}

void testMutableApplyAndRollback() {
    auto source = std::make_shared<FakeSessionSource>();
    source->records = {
        session(L"OUT-A", 100, 1000),
        session(L"OUT-A", 101, 1001),
        session(L"OUT-A", 900, 9000),
    };
    auto backend = std::make_shared<FakeMutableBackend>(source);
    RouteTransaction transaction(request(), source, backend);

    std::string error;
    auto status = transaction.attempt(endpoints(), &error);
    assert(status.phase == RoutePhase::Applied);
    assert(status.mutated);
    assert(source->records[0].endpointId == L"OUT-B");
    assert(source->records[1].endpointId == L"OUT-B");
    assert(source->records[2].endpointId == L"OUT-A");

    status = transaction.rollback(endpoints(), &error);
    assert(status.phase == RoutePhase::Ready);
    assert(status.rollbackVerified);
    assert(!status.mutated);
    assert(source->records[0].endpointId == L"OUT-A");
    assert(source->records[1].endpointId == L"OUT-A");
    assert(source->records[2].endpointId == L"OUT-A");
}

void testFailureContainment() {
    {
        auto source = std::make_shared<FakeSessionSource>();
        source->records = {session(L"OUT-A", 100, 1000)};
        auto backend = std::make_shared<FakeMutableBackend>(
            source, MutableMode::FailAfterMutation);
        RouteTransaction transaction(request(), source, backend);
        const auto status = transaction.attempt(endpoints());
        assert(status.phase == RoutePhase::Failed);
        assert(status.error == RouteError::ApplyFailed);
        assert(status.rollbackVerified);
        assert(!status.mutated);
        assert(source->records[0].endpointId == L"OUT-A");
    }

    {
        auto source = std::make_shared<FakeSessionSource>();
        source->records = {session(L"OUT-A", 100, 1000)};
        auto backend = std::make_shared<FakeMutableBackend>(
            source, MutableMode::PretendApplied);
        RouteTransaction transaction(request(), source, backend);
        const auto status = transaction.attempt(endpoints());
        assert(status.phase == RoutePhase::Failed);
        assert(status.error == RouteError::VerificationFailed);
        assert(status.rollbackVerified);
    }

    {
        auto source = std::make_shared<FakeSessionSource>();
        source->records = {session(L"OUT-A", 100, 1000)};
        auto backend = std::make_shared<FakeMutableBackend>(
            source, MutableMode::RollbackFailure);
        RouteTransaction transaction(request(), source, backend);
        auto status = transaction.attempt(endpoints());
        assert(status.phase == RoutePhase::Applied);
        status = transaction.rollback(endpoints());
        assert(status.phase == RoutePhase::RecoveryRequired);
        assert(status.error == RouteError::RollbackFailed);
        assert(status.mutated);
    }
}

void testValidationAndIncompleteObservation() {
    auto source = std::make_shared<FakeSessionSource>();
    source->records = {session(L"OUT-A", 100, 1000)};

    auto badSeat = request();
    badSeat.seatId = 2;
    RouteTransaction seatTransaction(badSeat, source);
    assert(
        seatTransaction.attempt(endpoints()).error ==
        RouteError::InvalidSeat);

    RouteTransaction missing(request(L"MISSING"), source);
    assert(
        missing.attempt(endpoints()).error ==
        RouteError::TargetEndpointMissing);

    RouteTransaction unavailable(request(L"OUT-OFF"), source);
    assert(
        unavailable.attempt(endpoints()).error ==
        RouteError::TargetEndpointUnavailable);

    auto incompleteTree = request();
    incompleteTree.processTree.trackingComplete = false;
    RouteTransaction incompleteTreeTransaction(incompleteTree, source);
    assert(
        incompleteTreeTransaction.attempt(endpoints()).error ==
        RouteError::InvalidProcessTree);

    source->complete = false;
    RouteTransaction incompleteObservation(request(), source);
    assert(
        incompleteObservation.attempt(endpoints()).error ==
        RouteError::SnapshotFailed);
}

} // namespace

int main() {
    testObserveOnlyAndExactOwnership();
    testMutableApplyAndRollback();
    testFailureContainment();
    testValidationAndIncompleteObservation();
    return 0;
}
