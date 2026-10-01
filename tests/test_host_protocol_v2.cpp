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
    snapshot.seats[0] =
        SeatSnapshot{
            1, 3, true, true, true, true, true, false, 4242, 0x1122334455667788ull};
    snapshot.seats[1] =
        SeatSnapshot{2, 0, false, false, false, false, false, false, 0, 0};
    const auto snapshotBytes = encodeSnapshot(snapshot);
    assert(snapshotBytes.size() == 88);
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
    wrongVersion[4] = std::byte{1};
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
    invalidSnapshot.seats[1] =
        SeatSnapshot{2, 0, false, false, false, false, true, false, 0, 0};
    assert(encodeSnapshot(invalidSnapshot).empty());

    auto processWithoutGameLease = snapshot;
    processWithoutGameLease.seats[0].gameLeaseActive = false;
    assert(encodeSnapshot(processWithoutGameLease).empty());

    const SeatRequest seatRequest{2};
    const auto seatRequestBytes = encodeSeatRequest(seatRequest);
    assert(decodeSeatRequest(seatRequestBytes) == seatRequest);
    assert(encodeSeatRequest(SeatRequest{3}).empty());

    const ControllerPairRequest pairRequest{
        1, 2, "container:{12345678-1234-1234-1234-1234567890AB}"};
    const auto pairBytes = encodeControllerPairRequest(pairRequest);
    assert(!pairBytes.empty());
    assert(decodeControllerPairRequest(pairBytes) == pairRequest);

    auto invalidPair = pairRequest;
    invalidPair.runtimeXInputSlot = 4;
    assert(encodeControllerPairRequest(invalidPair).empty());

    invalidPair = pairRequest;
    invalidPair.persistentControllerId = "has space";
    assert(encodeControllerPairRequest(invalidPair).empty());

    const ProcessRequest processRequest{4242, 0x1122334455667788ull};
    const auto processBytes = encodeProcessRequest(processRequest);
    assert(processBytes.size() == 12);
    assert(decodeProcessRequest(processBytes) == processRequest);
    assert(encodeProcessRequest(ProcessRequest{0, 1}).empty());
    assert(encodeProcessRequest(ProcessRequest{1, 0}).empty());

    const AudioRouteRequest audioRoute{
        processRequest,
        "{0.0.0.00000000}.{12345678-1234-1234-1234-1234567890AB}"};
    const auto audioRouteBytes = encodeAudioRouteRequest(audioRoute);
    assert(!audioRouteBytes.empty());
    assert(decodeAudioRouteRequest(audioRouteBytes) == audioRoute);

    auto invalidAudioRoute = audioRoute;
    invalidAudioRoute.endpointId = "endpoint with spaces";
    assert(encodeAudioRouteRequest(invalidAudioRoute).empty());

    const AudioMutationResult audioResult{
        AudioMutationStatus::IdentityMismatch};
    const auto audioResultBytes = encodeAudioMutationResult(audioResult);
    assert(audioResultBytes.size() == 8);
    assert(decodeAudioMutationResult(audioResultBytes) == audioResult);

    const auto ping = encodePing(0x1234u);
    assert(decodePing(ping) == std::optional<std::uint64_t>{0x1234u});
    assert(encodePing(0).empty());

    const ErrorPayload error{ErrorCode::Unsupported, "unsupported direction"};
    const auto errorBytes = encodeError(error);
    assert(!errorBytes.empty());
    assert(decodeError(errorBytes) == error);

    const ErrorPayload tooLong{
        ErrorCode::Malformed,
        std::string(kHostProtocolMaxDiagnosticBytes + 1, 'x')};
    assert(encodeError(tooLong).empty());

    assert(isMutatingRequest(MessageType::AcquireUiLease));
    assert(isMutatingRequest(MessageType::ReleaseUiLease));
    assert(isMutatingRequest(MessageType::PairController));
    assert(isMutatingRequest(MessageType::RouteAudio));
    assert(isMutatingRequest(MessageType::ResetAudio));
    assert(!isMutatingRequest(MessageType::GetSnapshot));

    assert(responseTypeFor(MessageType::Hello) == MessageType::HelloAck);
    assert(responseTypeFor(MessageType::GetSnapshot) == MessageType::Snapshot);
    assert(responseTypeFor(MessageType::Ping) == MessageType::Pong);
    assert(responseTypeFor(MessageType::AcquireUiLease) ==
           MessageType::AcquireUiLeaseResult);
    assert(responseTypeFor(MessageType::ReleaseUiLease) ==
           MessageType::ReleaseUiLeaseResult);
    assert(responseTypeFor(MessageType::PairController) ==
           MessageType::PairControllerResult);
    assert(responseTypeFor(MessageType::RouteAudio) ==
           MessageType::RouteAudioResult);
    assert(responseTypeFor(MessageType::ResetAudio) ==
           MessageType::ResetAudioResult);
    assert(responseTypeFor(MessageType::Snapshot) == MessageType::Error);

    return 0;
}
