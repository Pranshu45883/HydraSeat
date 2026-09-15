#include "hydra/virtual_xinput_protocol.hpp"

#include <cassert>

void testVirtualXInputProtocol() {
    using namespace hydra::controller::ipc;

    VirtualXInputRequest request{};
    request.opcode = ProtocolOpcode::GetState;
    request.seatId = 1;
    request.activationGeneration = 7;
    request.sourceGeneration = 3;
    request.logicalSlot = 0;

    const auto bytes = encodeRequest(request);
    const auto decoded = decodeRequest(bytes);
    assert(decoded.has_value());
    assert(*decoded == request);

    auto badVersion = bytes;
    badVersion[0] = 0xFF;
    assert(!decodeRequest(badVersion).has_value());

    auto truncated = bytes;
    truncated.pop_back();
    assert(!decodeRequest(truncated).has_value());

    auto invalidSlot = request;
    invalidSlot.logicalSlot = 4;
    const auto invalidSlotBytes = encodeRequest(invalidSlot);
    assert(!decodeRequest(invalidSlotBytes).has_value());

    auto invalidSeat = request;
    invalidSeat.seatId = 3;
    const auto invalidSeatBytes = encodeRequest(invalidSeat);
    assert(!decodeRequest(invalidSeatBytes).has_value());

    VirtualXInputResponse response{};
    response.status = ProtocolStatus::Ok;
    response.hasState = true;
    response.state.packetNumber = 9;
    response.state.buttons = 0x0040;
    response.state.leftTrigger = 10;
    response.state.rightTrigger = 20;
    response.state.thumbLX = -100;
    response.state.thumbLY = 200;
    response.state.thumbRX = -300;
    response.state.thumbRY = 400;

    const auto responseBytes = encodeResponse(response);
    const auto decodedResponse = decodeResponse(responseBytes);
    assert(decodedResponse.has_value());
    assert(*decodedResponse == response);

    auto badResponseStatus = responseBytes;
    badResponseStatus[2] = 0xFF;
    assert(!decodeResponse(badResponseStatus).has_value());
}
