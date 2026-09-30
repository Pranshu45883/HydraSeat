#ifdef NDEBUG
#undef NDEBUG
#endif

#include "hydra/host_protocol.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

int main() {
    using namespace hydra::hostipc;

    const Hello hello{ClientRole::Control};
    const auto helloBytes = encodeHello(hello);
    assert(!helloBytes.empty());
    assert(decodeHello(helloBytes) == hello);

    const HelloAck ack{ClientRole::Control, kHostProtocolVersion,
                       static_cast<std::uint8_t>(kHostSeatCount)};
    const auto ackBytes = encodeHelloAck(ack);
    assert(!ackBytes.empty());
    assert(decodeHelloAck(ackBytes) == ack);

    HostSnapshot snapshot;
    snapshot.authorityRevision = 7;
    snapshot.seats[0] = SeatSnapshot{1, 3, true, true, true, false};
    snapshot.seats[1] = SeatSnapshot{2, 0, false, false, false, false};
    const auto snapshotBytes = encodeSnapshot(snapshot);
    assert(snapshotBytes.size() == 56);
    assert(decodeSnapshot(snapshotBytes) == snapshot);

    Frame frame{MessageType::Snapshot, 42, snapshotBytes};
    auto frameBytes = encodeFrame(frame);
    assert(frameBytes.size() == kHostProtocolHeaderBytes + snapshotBytes.size());

    DecodeResult decodeResult;
    const auto decodedFrame = decodeFrame(frameBytes, &decodeResult);
    assert(decodedFrame.has_value());
    assert(decodedFrame->type == MessageType::Snapshot);
    assert(decodedFrame->correlationId == 42);
    assert(decodedFrame->payload == snapshotBytes);
    assert(decodeResult.error == ErrorCode::None);

    auto wrongVersion = frameBytes;
    wrongVersion[4] = std::byte{2};
    const auto versionResult = decodeFrame(wrongVersion, &decodeResult);
    assert(!versionResult.has_value());
    assert(decodeResult.error == ErrorCode::VersionMismatch);

    auto reservedFrame = frameBytes;
    reservedFrame[20] = std::byte{1};
    assert(!decodeFrame(reservedFrame, &decodeResult).has_value());
    assert(decodeResult.error == ErrorCode::Malformed);

    Frame noCorrelation{MessageType::Ping, 0, encodePing(9)};
    assert(encodeFrame(noCorrelation).empty());

    Frame oversized{
        MessageType::Ping,
        1,
        std::vector<std::byte>(kHostProtocolMaxPayloadBytes + 1, std::byte{0})};
    assert(encodeFrame(oversized).empty());

    auto truncatedSnapshot = snapshotBytes;
    truncatedSnapshot.pop_back();
    assert(!decodeSnapshot(truncatedSnapshot).has_value());

    auto invalidSnapshot = snapshot;
    invalidSnapshot.seats[1] = SeatSnapshot{2, 0, false, false, true, false};
    assert(encodeSnapshot(invalidSnapshot).empty());

    const auto ping = encodePing(0x1234u);
    assert(decodePing(ping) == std::optional<std::uint64_t>{0x1234u});
    assert(encodePing(0).empty());

    const ErrorPayload error{ErrorCode::Unsupported, "read-only protocol layer"};
    const auto errorBytes = encodeError(error);
    assert(!errorBytes.empty());
    assert(decodeError(errorBytes) == error);

    const ErrorPayload tooLong{
        ErrorCode::Malformed,
        std::string(kHostProtocolMaxDiagnosticBytes + 1, 'x')};
    assert(encodeError(tooLong).empty());

    assert(responseTypeFor(MessageType::Hello) == MessageType::HelloAck);
    assert(responseTypeFor(MessageType::GetSnapshot) == MessageType::Snapshot);
    assert(responseTypeFor(MessageType::Ping) == MessageType::Pong);
    assert(responseTypeFor(MessageType::Snapshot) == MessageType::Error);

    return 0;
}
