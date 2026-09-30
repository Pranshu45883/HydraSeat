#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/host_transport.hpp"

#include <cassert>
#include <cstdint>

namespace {

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
        HostConnectionSession session(host);

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

        const auto released = session.handle(Frame{
            MessageType::ReleaseUiLease,
            11,
            encodeSeatRequest(SeatRequest{1})});
        assert(released.type == MessageType::ReleaseUiLeaseResult);
        snapshot = decodeSnapshot(released.payload);
        assert(snapshot);
        assert(snapshot->seats[0].active);
        assert(!snapshot->seats[0].uiLeaseActive);
        assert(snapshot->seats[0].gameLeaseActive);
        assert(snapshot->seats[0].processOwned);

        Frame ping{MessageType::Ping, 12, encodePing(0x77)};
        const auto pong = session.handle(ping);
        assert(pong.type == MessageType::Pong);
        assert(decodePing(pong.payload) == std::optional<std::uint64_t>{0x77});

        Frame forgedResponseDirection{
            MessageType::AcquireUiLeaseResult, 13, {}};
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
