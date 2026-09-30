#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/host_transport.hpp"

#include <cassert>
#include <cstdint>

namespace {

class FakeAudioRouter final : public hydra::runtime::AudioRouter {
public:
    hydra::runtime::AudioRouteStatus assignStatus{
        hydra::runtime::AudioRouteStatus::Success};
    hydra::runtime::AudioRouteStatus clearStatus{
        hydra::runtime::AudioRouteStatus::Success};
    std::uint32_t assignCalls{0};
    std::uint32_t clearCalls{0};
    hydra::runtime::ProcessIdentity lastProcess{};
    hydra::runtime::AudioEndpointIdentity lastEndpoint{};

    hydra::runtime::AudioRouteStatus assignEndpoint(
        const hydra::runtime::ProcessIdentity& process,
        const hydra::runtime::AudioEndpointIdentity& endpoint) noexcept override {
        ++assignCalls;
        lastProcess = process;
        lastEndpoint = endpoint;
        return assignStatus;
    }

    hydra::runtime::AudioRouteStatus clearAssignment(
        const hydra::runtime::ProcessIdentity& process) noexcept override {
        ++clearCalls;
        lastProcess = process;
        return clearStatus;
    }
};

hydra::hostipc::Frame hello(
    hydra::hostipc::HostConnectionSession& session,
    hydra::hostipc::ClientRole role,
    std::uint64_t correlation) {
    using namespace hydra::hostipc;
    return session.handle(Frame{
        MessageType::Hello,
        correlation,
        encodeHello(Hello{role})});
}

} // namespace

int main() {
    using namespace hydra::hostipc;
    using namespace hydra::runtime;

    RuntimeHost host;
    FakeAudioRouter audioRouter;

    {
        HostConnectionSession readOnly(host);
        const auto ack = hello(readOnly, ClientRole::ReadOnly, 1);
        assert(ack.type == MessageType::HelloAck);

        const auto denied = readOnly.handle(Frame{
            MessageType::AcquireUiLease,
            2,
            encodeSeatRequest(SeatRequest{1})});
        assert(denied.type == MessageType::Error);
        const auto deniedError = decodeError(denied.payload);
        assert(deniedError);
        assert(deniedError->code == ErrorCode::PermissionDenied);
        assert(!host.snapshot().seats[0].active);
    }

    {
        HostConnectionSession session(host, &audioRouter);

        Frame beforeHello{MessageType::GetSnapshot, 3, {}};
        const auto denied = session.handle(beforeHello);
        assert(denied.type == MessageType::Error);
        const auto deniedError = decodeError(denied.payload);
        assert(deniedError);
        assert(deniedError->code == ErrorCode::PermissionDenied);

        Frame badHello{MessageType::Hello, 4, {}};
        const auto malformed = session.handle(badHello);
        assert(malformed.type == MessageType::Error);
        const auto malformedError = decodeError(malformed.payload);
        assert(malformedError);
        assert(malformedError->code == ErrorCode::Malformed);

        const auto helloAck = hello(session, ClientRole::Control, 5);
        assert(helloAck.type == MessageType::HelloAck);
        const auto ack = decodeHelloAck(helloAck.payload);
        assert(ack);
        assert(ack->role == ClientRole::Control);

        const auto repeatedHello =
            hello(session, ClientRole::Control, 6);
        assert(repeatedHello.type == MessageType::Error);

        const auto acquired = session.handle(Frame{
            MessageType::AcquireUiLease,
            7,
            encodeSeatRequest(SeatRequest{1})});
        assert(acquired.type == MessageType::AcquireUiLeaseResult);
        auto snapshot = decodeSnapshot(acquired.payload);
        assert(snapshot);
        assert(snapshot->seats[0].active);
        assert(snapshot->seats[0].uiLeaseActive);
        assert(!snapshot->seats[0].gameLeaseActive);

        const auto duplicate = session.handle(Frame{
            MessageType::AcquireUiLease,
            8,
            encodeSeatRequest(SeatRequest{1})});
        assert(duplicate.type == MessageType::Error);
        const auto duplicateError = decodeError(duplicate.payload);
        assert(duplicateError);
        assert(duplicateError->code == ErrorCode::InvalidState);

        // The host can acquire the independent game lease inside the same epoch.
        const auto activation = host.beginSeatActivation(1);
        assert(activation.valid());
        const ProcessIdentity process{1234, 5678};
        assert(host.publishProcess(activation, process));

        Frame snapshotRequest{MessageType::GetSnapshot, 9, {}};
        const auto snapshotResponse = session.handle(snapshotRequest);
        assert(snapshotResponse.type == MessageType::Snapshot);
        snapshot = decodeSnapshot(snapshotResponse.payload);
        assert(snapshot);
        assert(snapshot->seats[0].uiLeaseActive);
        assert(snapshot->seats[0].gameLeaseActive);
        assert(snapshot->seats[0].processOwned);
        assert(!snapshot->seats[1].active);

        const auto pairWithoutSeat2Lease = session.handle(Frame{
            MessageType::PairController,
            10,
            encodeControllerPairRequest(ControllerPairRequest{
                2, 0, "container:{12345678-1234-1234-1234-1234567890AB}"})});
        assert(pairWithoutSeat2Lease.type == MessageType::Error);
        const auto pairError = decodeError(pairWithoutSeat2Lease.payload);
        assert(pairError);
        assert(pairError->code == ErrorCode::InvalidState);

        const std::string endpointId =
            "{0.0.0.00000000}.{12345678-1234-1234-1234-1234567890AB}";
        const auto routed = session.handle(Frame{
            MessageType::RouteAudio,
            11,
            encodeAudioRouteRequest(AudioRouteRequest{
                ProcessRequest{1, process.pid, process.creationIdentity},
                endpointId})});
        assert(routed.type == MessageType::RouteAudioResult);
        const auto routeResult = decodeAudioMutationResult(routed.payload);
        assert(routeResult);
        assert(routeResult->status == AudioMutationStatus::Success);
        assert(audioRouter.assignCalls == 1);
        assert(audioRouter.lastProcess == process);
        assert(audioRouter.lastEndpoint.endpointId ==
               std::wstring(endpointId.begin(), endpointId.end()));

        // A reused PID / wrong creation identity is rejected by host authority
        // before the audio backend is invoked.
        const auto staleRoute = session.handle(Frame{
            MessageType::RouteAudio,
            12,
            encodeAudioRouteRequest(AudioRouteRequest{
                ProcessRequest{1, process.pid, process.creationIdentity + 1},
                endpointId})});
        assert(staleRoute.type == MessageType::RouteAudioResult);
        const auto staleResult = decodeAudioMutationResult(staleRoute.payload);
        assert(staleResult);
        assert(staleResult->status == AudioMutationStatus::InvalidProcess);
        assert(audioRouter.assignCalls == 1);

        const auto reset = session.handle(Frame{
            MessageType::ResetAudio,
            13,
            encodeProcessRequest(ProcessRequest{
                1, process.pid, process.creationIdentity})});
        assert(reset.type == MessageType::ResetAudioResult);
        const auto resetResult = decodeAudioMutationResult(reset.payload);
        assert(resetResult);
        assert(resetResult->status == AudioMutationStatus::Success);
        assert(audioRouter.clearCalls == 1);

        const auto released = session.handle(Frame{
            MessageType::ReleaseUiLease,
            14,
            encodeSeatRequest(SeatRequest{1})});
        assert(released.type == MessageType::ReleaseUiLeaseResult);
        snapshot = decodeSnapshot(released.payload);
        assert(snapshot);
        assert(snapshot->seats[0].active);
        assert(!snapshot->seats[0].uiLeaseActive);
        assert(snapshot->seats[0].gameLeaseActive);
        assert(snapshot->seats[0].processOwned);

        const auto deniedAfterRelease = session.handle(Frame{
            MessageType::RouteAudio,
            15,
            encodeAudioRouteRequest(AudioRouteRequest{
                ProcessRequest{1, process.pid, process.creationIdentity},
                "{0.0.0.00000000}.{12345678-1234-1234-1234-1234567890AB}"})});
        assert(deniedAfterRelease.type == MessageType::Error);
        const auto deniedAudio = decodeError(deniedAfterRelease.payload);
        assert(deniedAudio);
        assert(deniedAudio->code == ErrorCode::InvalidState);
        assert(audioRouter.assignCalls == 1);

        Frame ping{MessageType::Ping, 16, encodePing(0x77)};
        const auto pong = session.handle(ping);
        assert(pong.type == MessageType::Pong);
        assert(decodePing(pong.payload) == std::optional<std::uint64_t>{0x77});

        Frame forgedResponseDirection{
            MessageType::AcquireUiLeaseResult, 17, {}};
        const auto unsupported = session.handle(forgedResponseDirection);
        assert(unsupported.type == MessageType::Error);
        const auto unsupportedError = decodeError(unsupported.payload);
        assert(unsupportedError);
        assert(unsupportedError->code == ErrorCode::Unsupported);

        assert(host.endSeatActivation(activation));
        assert(!host.snapshot().seats[0].active);
    }

    // A connection-scoped UI lease is released automatically at disconnect.
    {
        HostConnectionSession temporary(host);
        assert(hello(temporary, ClientRole::Control, 20).type ==
               MessageType::HelloAck);
        const auto acquired = temporary.handle(Frame{
            MessageType::AcquireUiLease,
            21,
            encodeSeatRequest(SeatRequest{2})});
        assert(acquired.type == MessageType::AcquireUiLeaseResult);
        assert(host.snapshot().seats[1].uiLeaseActive);
    }
    assert(!host.snapshot().seats[1].active);

    return 0;
}
