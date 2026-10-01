#include "hydra/gate_c_probe_snapshot.hpp"
#include "hydra/gate_c_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

hydra::gatec::ProbeComparison sampleComparison() {
    hydra::gatec::ProbeComparison comparison;
    comparison.sequence = 42;
    comparison.monotonicTimestampMicros = 123456789;
    comparison.processId = 1001;
    comparison.threadId = 2002;
    comparison.seatId = 2;
    comparison.probeVkey = 0x41;
    comparison.targetWindowRuntimeValue = 0x11112222u;

    comparison.os.asyncKeyState = static_cast<std::int16_t>(0x8001u);
    comparison.os.keyState = static_cast<std::int16_t>(0x8000u);
    comparison.os.keyboardStateSucceeded = true;
    comparison.os.keyboardState[0x41] = 0x80u;
    comparison.os.cursorPositionSucceeded = true;
    comparison.os.cursorX = 400;
    comparison.os.cursorY = 300;
    comparison.os.clipRectangleSucceeded = true;
    comparison.os.clipRectangle = {0, 0, 1920, 1080};
    comparison.os.foregroundWindowRuntimeValue = 0x33334444u;
    comparison.os.activeWindowRuntimeValue =
        comparison.targetWindowRuntimeValue;
    comparison.os.focusWindowRuntimeValue =
        comparison.targetWindowRuntimeValue;
    comparison.os.captureWindowRuntimeValue = 0;

    comparison.adapter.asyncKeyState = 0x8001u;
    comparison.adapter.lastAppliedSequence = 41;
    comparison.adapter.keyState = 0x8000u;
    comparison.adapter.keyboardState[0x41] = 0x80u;
    comparison.adapter.keyDownBits[0x41 / 8] =
        static_cast<std::uint8_t>(1u << (0x41 % 8));
    comparison.adapter.keyPressedEdgeBits[0x41 / 8] =
        static_cast<std::uint8_t>(1u << (0x41 % 8));
    comparison.adapter.mouseButtonsDown = 1;
    comparison.adapter.wheelAccumulator = 120;
    comparison.adapter.cursorX = 15;
    comparison.adapter.cursorY = 27;
    comparison.adapter.clipEnabled = true;
    comparison.adapter.virtualForeground = true;
    comparison.adapter.virtualCapture = true;
    comparison.adapter.clipRectangle = {0, 0, 100, 100};
    comparison.adapter.targetWindowRuntimeValue =
        comparison.targetWindowRuntimeValue;
    comparison.adapter.logicalForegroundWindowRuntimeValue =
        comparison.targetWindowRuntimeValue;
    comparison.adapter.logicalActiveWindowRuntimeValue =
        comparison.targetWindowRuntimeValue;
    comparison.adapter.logicalFocusWindowRuntimeValue =
        comparison.targetWindowRuntimeValue;
    comparison.adapter.virtualCaptureWindowRuntimeValue =
        comparison.targetWindowRuntimeValue;

    comparison.rawInput.enabled = true;
    comparison.rawInput.registrationLifecyclePassed = true;
    comparison.rawInput.registrationQueryPassed = true;
    comparison.rawInput.dataQueryPassed = true;
    comparison.rawInput.bufferReadPassed = true;
    comparison.rawInput.registeredCount = 2;
    comparison.rawInput.keyboardExpected = 1;
    comparison.rawInput.mouseExpected = 1;
    comparison.rawInput.dataReads = 1;
    comparison.rawInput.bufferPackets = 1;

    hydra::gatec::updateProbeComparison(comparison);
    return comparison;
}

void testRoundTrip() {
    using namespace hydra::gatec;
    static_assert(kProbeSnapshotWireBytes <= kMaximumPayloadBytes);
    const auto comparison = sampleComparison();
    const auto encoded = encodeProbeComparison(comparison);
    check(encoded.size() == kProbeSnapshotWireBytes,
          "schema v3 snapshot has the declared fixed wire size");
    const auto decoded = decodeProbeComparison(encoded);
    check(decoded && decoded.comparison == comparison,
          "OS/adapter probe comparison round-trips exactly");
    const auto framed = encodeFrame(
        MessageType::ProbeSnapshot, comparison.sequence, encoded);
    const auto decodedFrame = decodeFrame(framed);
    check(decodedFrame && decodedFrame.frame &&
              decodedFrame.frame->type == MessageType::ProbeSnapshot &&
              decodedFrame.frame->sequence == comparison.sequence &&
              decodeProbeComparison(decodedFrame.frame->payload),
          "the comparison remains bounded inside the Gate C ProbeSnapshot frame");
    check(!comparison.foregroundMatches &&
              comparison.adapter.virtualForeground &&
              !comparison.osForegroundIsTarget,
          "the snapshot preserves an intentional OS/adapter foreground difference");
}

void testInvalidVersionAndSchemaFields() {
    using namespace hydra::gatec;
    auto encoded = encodeProbeComparison(sampleComparison());
    encoded[4] = std::byte{4};
    encoded[5] = std::byte{0};
    check(!decodeProbeComparison(encoded),
          "future snapshot schema versions are rejected");

    encoded = encodeProbeComparison(sampleComparison());
    encoded[0] = std::byte{0};
    check(!decodeProbeComparison(encoded),
          "invalid snapshot magic is rejected");

    auto inconsistent = sampleComparison();
    inconsistent.foregroundMatches = true;
    check(encodeProbeComparison(inconsistent).empty(),
          "inconsistent comparison flags are rejected before serialization");

    auto disabledRawWithCounters = sampleComparison();
    disabledRawWithCounters.rawInput.enabled = false;
    check(encodeProbeComparison(disabledRawWithCounters).empty(),
          "disabled raw snapshot cannot carry lifecycle flags or counters");
}

void testInvalidSizesAndMalformedFields() {
    using namespace hydra::gatec;
    auto encoded = encodeProbeComparison(sampleComparison());
    encoded.pop_back();
    check(!decodeProbeComparison(encoded),
          "truncated snapshots are rejected");

    encoded = encodeProbeComparison(sampleComparison());
    encoded.push_back(std::byte{0});
    check(!decodeProbeComparison(encoded),
          "snapshots larger than the fixed schema are rejected");

    std::vector<std::byte> oversized(kMaximumProbeSnapshotBytes + 1,
                                     std::byte{0});
    check(!decodeProbeComparison(oversized),
          "snapshots larger than the bounded maximum are rejected");

    encoded = encodeProbeComparison(sampleComparison());
    encoded[kProbeSnapshotWireBytes - 12] = std::byte{2};
    check(!decodeProbeComparison(encoded),
          "malformed boolean fields are rejected");

    auto invalidIdentity = sampleComparison();
    invalidIdentity.targetWindowRuntimeValue = 0;
    hydra::gatec::updateProbeComparison(invalidIdentity);
    check(encodeProbeComparison(invalidIdentity).empty(),
          "a missing target window cannot be serialized as a valid snapshot");
}

void testFailedAdapterCallsNeverReportMatches() {
    using namespace hydra::gatec;
    auto comparison = sampleComparison();
    comparison.os.cursorX = comparison.adapter.cursorX;
    comparison.os.cursorY = comparison.adapter.cursorY;
    comparison.os.clipRectangle = comparison.adapter.clipRectangle;
    comparison.os.foregroundWindowRuntimeValue =
        comparison.targetWindowRuntimeValue;
    comparison.os.captureWindowRuntimeValue =
        comparison.targetWindowRuntimeValue;
    comparison.adapter.snapshotResult = 1;
    comparison.adapter.keyStateResult = 1;
    comparison.adapter.keyboardStateResult = 1;
    comparison.adapter.controlStateResult = 1;
    comparison.adapter.windowStateResult = 1;
    updateProbeComparison(comparison);

    check(!comparison.asyncDownMatches &&
              !comparison.keyStateDownMatches &&
              !comparison.keyboardStateDownMatches &&
              !comparison.cursorMatches &&
              !comparison.clipRectangleMatches &&
              !comparison.foregroundMatches &&
              !comparison.captureMatches,
          "failed adapter reads cannot be summarized as matching OS state");
}

} // namespace

int main() {
    testRoundTrip();
    testInvalidVersionAndSchemaFields();
    testInvalidSizesAndMalformedFields();
    testFailedAdapterCallsNeverReportMatches();
    std::cout << "Gate C API probe snapshot tests passed.\n";
    return EXIT_SUCCESS;
}
