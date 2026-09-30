#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/host_transport.hpp"

#include <cassert>
#include <cstdint>

int main() {
    using namespace hydra::hostipc;
    using namespace hydra::runtime;

    RuntimeHost host;
    HostConnectionSession session(host);

    Frame beforeHello{MessageType::GetSnapshot, 1, {}};
    const auto denied = session.handle(beforeHello);
    assert(denied.type == MessageType::Error);
    const auto deniedError = decodeError(denied.payload);
    assert(deniedError.has_value());
    assert(deniedError->code == ErrorCode::PermissionDenied);

    Frame badHello{MessageType::Hello, 2, {}};
    const auto malformed = session.handle(badHello);
    assert(malformed.type == MessageType::Error);
    const auto malformedError = decodeError(malformed.payload);
    assert(malformedError.has_value());
    assert(malformedError->code == ErrorCode::Malformed);

    Frame hello{
        MessageType::Hello,
        3,
        encodeHello(Hello{ClientRole::Control})};
    const auto helloAck = session.handle(hello);
    assert(helloAck.type == MessageType::HelloAck);
    assert(helloAck.correlationId == hello.correlationId);
    const auto ack = decodeHelloAck(helloAck.payload);
    assert(ack.has_value());
    assert(ack->role == ClientRole::Control);

    const auto repeatedHello = session.handle(hello);
    assert(repeatedHello.type == MessageType::Error);
    const auto repeatedError = decodeError(repeatedHello.payload);
    assert(repeatedError.has_value());
    assert(repeatedError->code == ErrorCode::Malformed);

    const auto activation = host.beginSeatActivation(1);
    assert(activation.valid());
    const ProcessIdentity process{1234, 5678};
    assert(host.publishProcess(activation, process));

    Frame snapshotRequest{MessageType::GetSnapshot, 4, {}};
    const auto snapshotResponse = session.handle(snapshotRequest);
    assert(snapshotResponse.type == MessageType::Snapshot);
    const auto snapshot = decodeSnapshot(snapshotResponse.payload);
    assert(snapshot.has_value());
    assert(snapshot->seats[0].active);
    assert(snapshot->seats[0].processOwned);
    assert(!snapshot->seats[1].active);

    Frame badSnapshotRequest{
        MessageType::GetSnapshot,
        5,
        encodePing(1)};
    const auto badSnapshotResponse = session.handle(badSnapshotRequest);
    assert(badSnapshotResponse.type == MessageType::Error);
    const auto badSnapshotError = decodeError(badSnapshotResponse.payload);
    assert(badSnapshotError.has_value());
    assert(badSnapshotError->code == ErrorCode::Malformed);

    Frame ping{MessageType::Ping, 6, encodePing(0x77)};
    const auto pong = session.handle(ping);
    assert(pong.type == MessageType::Pong);
    assert(decodePing(pong.payload) == std::optional<std::uint64_t>{0x77});

    // Control role is intentionally non-authoritative in protocol v1. Unknown
    // request directions cannot mutate RuntimeHost.
    Frame forgedResponseDirection{MessageType::Snapshot, 7, {}};
    const auto unsupported = session.handle(forgedResponseDirection);
    assert(unsupported.type == MessageType::Error);
    const auto unsupportedError = decodeError(unsupported.payload);
    assert(unsupportedError.has_value());
    assert(unsupportedError->code == ErrorCode::Unsupported);

    const auto after = host.snapshot();
    assert(after.seats[0].active);
    assert(after.seats[0].processOwned);

    return 0;
}
