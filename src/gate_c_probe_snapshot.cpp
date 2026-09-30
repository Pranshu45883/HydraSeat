#include "hydra/gate_c_probe_snapshot.hpp"

#include <bit>
#include <limits>
#include <utility>

namespace hydra::gatec {
namespace {

class Writer {
public:
    void u8(std::uint8_t value) { m_bytes.push_back(static_cast<std::byte>(value)); }
    void boolean(bool value) { u8(value ? 1u : 0u); }
    void u16(std::uint16_t value) {
        for (unsigned shift = 0; shift < 16; shift += 8) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void i16(std::int16_t value) { u16(std::bit_cast<std::uint16_t>(value)); }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void i32(std::int32_t value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void i64(std::int64_t value) { u64(std::bit_cast<std::uint64_t>(value)); }
    template <std::size_t Size>
    void array(const std::array<std::uint8_t, Size>& value) {
        for (const auto byte : value) u8(byte);
    }
    void padding(std::size_t count) {
        m_bytes.insert(m_bytes.end(), count, std::byte{0});
    }
    std::vector<std::byte> take() { return std::move(m_bytes); }

private:
    std::vector<std::byte> m_bytes;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : m_bytes(bytes) {}

    bool u8(std::uint8_t& value) {
        if (!require(1)) return false;
        value = std::to_integer<std::uint8_t>(m_bytes[m_offset++]);
        return true;
    }
    bool boolean(bool& value) {
        std::uint8_t raw = 0;
        if (!u8(raw)) return false;
        if (raw > 1u) {
            m_error = "boolean field is not 0 or 1";
            return false;
        }
        value = raw != 0;
        return true;
    }
    bool u16(std::uint16_t& value) {
        if (!require(2)) return false;
        value = static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(m_bytes[m_offset])) |
            static_cast<std::uint16_t>(
                std::to_integer<std::uint8_t>(m_bytes[m_offset + 1])) << 8;
        m_offset += 2;
        return true;
    }
    bool i16(std::int16_t& value) {
        std::uint16_t raw = 0;
        if (!u16(raw)) return false;
        value = std::bit_cast<std::int16_t>(raw);
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (!require(4)) return false;
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(
                    m_bytes[m_offset + shift / 8])) << shift;
        }
        m_offset += 4;
        return true;
    }
    bool i32(std::int32_t& value) {
        std::uint32_t raw = 0;
        if (!u32(raw)) return false;
        value = std::bit_cast<std::int32_t>(raw);
        return true;
    }
    bool u64(std::uint64_t& value) {
        if (!require(8)) return false;
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(
                    m_bytes[m_offset + shift / 8])) << shift;
        }
        m_offset += 8;
        return true;
    }
    bool i64(std::int64_t& value) {
        std::uint64_t raw = 0;
        if (!u64(raw)) return false;
        value = std::bit_cast<std::int64_t>(raw);
        return true;
    }
    template <std::size_t Size>
    bool array(std::array<std::uint8_t, Size>& value) {
        if (!require(Size)) return false;
        for (std::size_t index = 0; index < Size; ++index) {
            value[index] =
                std::to_integer<std::uint8_t>(m_bytes[m_offset + index]);
        }
        m_offset += Size;
        return true;
    }
    bool padding(std::size_t count) {
        if (!require(count)) return false;
        for (std::size_t index = 0; index < count; ++index) {
            if (m_bytes[m_offset + index] != std::byte{0}) {
                m_error = "reserved field is not zero";
                return false;
            }
        }
        m_offset += count;
        return true;
    }
    bool finished() const noexcept { return m_offset == m_bytes.size(); }
    const std::string& error() const noexcept { return m_error; }

private:
    bool require(std::size_t count) {
        if (count > m_bytes.size() - m_offset) {
            m_error = "probe snapshot is truncated";
            return false;
        }
        return true;
    }

    std::span<const std::byte> m_bytes;
    std::size_t m_offset{0};
    std::string m_error;
};

void writeRect(Writer& writer, const ProbeRect& rect) {
    writer.i32(rect.left);
    writer.i32(rect.top);
    writer.i32(rect.right);
    writer.i32(rect.bottom);
}

bool readRect(Reader& reader, ProbeRect& rect) {
    return reader.i32(rect.left) && reader.i32(rect.top) &&
           reader.i32(rect.right) && reader.i32(rect.bottom);
}

bool keyDown(std::int16_t value) noexcept {
    return (static_cast<std::uint16_t>(value) & 0x8000u) != 0;
}

bool adapterKeyDown(std::uint16_t value) noexcept {
    return (value & 0x8000u) != 0;
}

bool validVkey(std::uint16_t vkey) noexcept {
    return vkey < 256u;
}

bool validRect(const ProbeRect& rect) noexcept {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool validate(const ProbeComparison& comparison, std::string& error) {
    if (comparison.schemaVersion != kProbeSnapshotSchemaVersion) {
        error = "unsupported probe snapshot schema version";
        return false;
    }
    if (comparison.sequence == 0 ||
        comparison.monotonicTimestampMicros == 0 ||
        comparison.processId == 0 || comparison.threadId == 0 ||
        comparison.seatId == 0 || !validVkey(comparison.probeVkey) ||
        comparison.targetWindowRuntimeValue == 0) {
        error = "probe snapshot identity fields are invalid";
        return false;
    }
    if (comparison.adapter.clipEnabled &&
        !validRect(comparison.adapter.clipRectangle)) {
        error = "adapter clip rectangle is invalid";
        return false;
    }
    if (comparison.os.clipRectangleSucceeded &&
        !validRect(comparison.os.clipRectangle)) {
        error = "OS clip rectangle is invalid";
        return false;
    }
    constexpr std::uint32_t kMaximumAdapterResult = 7;
    if (comparison.adapter.snapshotResult > kMaximumAdapterResult ||
        comparison.adapter.keyStateResult > kMaximumAdapterResult ||
        comparison.adapter.keyboardStateResult > kMaximumAdapterResult ||
        comparison.adapter.controlStateResult > kMaximumAdapterResult ||
        comparison.adapter.mouseStateResult > kMaximumAdapterResult ||
        comparison.adapter.windowStateResult > kMaximumAdapterResult) {
        error = "adapter result code is invalid";
        return false;
    }
    if (!comparison.rawInput.enabled &&
        (comparison.rawInput.registrationLifecyclePassed ||
         comparison.rawInput.registrationQueryPassed ||
         comparison.rawInput.dataQueryPassed ||
         comparison.rawInput.bufferReadPassed ||
         comparison.rawInput.registeredCount != 0 ||
         comparison.rawInput.keyboardExpected != 0 ||
         comparison.rawInput.keyboardCross != 0 ||
         comparison.rawInput.mouseExpected != 0 ||
         comparison.rawInput.mouseCross != 0 ||
         comparison.rawInput.dataReads != 0 ||
         comparison.rawInput.bufferPackets != 0 ||
         comparison.rawInput.apiFailures != 0 ||
         comparison.rawInput.destroyedTargetFailures != 0 ||
         comparison.rawInput.staleTokenFailures != 0 ||
         comparison.rawInput.queueOverflowFailures != 0 ||
         comparison.rawInput.lastSystemError != 0)) {
        error = "disabled Raw Input snapshot contains observations";
        return false;
    }
    ProbeComparison expected = comparison;
    updateProbeComparison(expected);
    if (expected.asyncDownMatches != comparison.asyncDownMatches ||
        expected.keyStateDownMatches != comparison.keyStateDownMatches ||
        expected.keyboardStateDownMatches !=
            comparison.keyboardStateDownMatches ||
        expected.cursorMatches != comparison.cursorMatches ||
        expected.clipRectangleMatches != comparison.clipRectangleMatches ||
        expected.foregroundMatches != comparison.foregroundMatches ||
        expected.activeMatches != comparison.activeMatches ||
        expected.focusMatches != comparison.focusMatches ||
        expected.captureMatches != comparison.captureMatches ||
        expected.osForegroundIsTarget != comparison.osForegroundIsTarget ||
        expected.osActiveIsTarget != comparison.osActiveIsTarget ||
        expected.osFocusIsTarget != comparison.osFocusIsTarget ||
        expected.osCaptureIsTarget != comparison.osCaptureIsTarget) {
        error = "probe comparison fields are inconsistent";
        return false;
    }
    return true;
}

} // namespace

void updateProbeComparison(ProbeComparison& comparison) noexcept {
    const auto keyIndex = static_cast<std::size_t>(comparison.probeVkey);
    constexpr std::uint32_t kAdapterOk = 0;
    const bool adapterSnapshotValid =
        comparison.adapter.snapshotResult == kAdapterOk;
    const bool adapterKeyStateValid =
        comparison.adapter.keyStateResult == kAdapterOk;
    const bool adapterKeyboardStateValid =
        comparison.adapter.keyboardStateResult == kAdapterOk;
    const bool adapterControlStateValid =
        comparison.adapter.controlStateResult == kAdapterOk;
    const bool adapterWindowStateValid =
        comparison.adapter.windowStateResult == kAdapterOk;
    comparison.asyncDownMatches =
        adapterSnapshotValid &&
        keyDown(comparison.os.asyncKeyState) ==
        adapterKeyDown(comparison.adapter.asyncKeyState);
    comparison.keyStateDownMatches =
        adapterKeyStateValid &&
        keyDown(comparison.os.keyState) ==
        adapterKeyDown(comparison.adapter.keyState);
    comparison.keyboardStateDownMatches =
        comparison.os.keyboardStateSucceeded && adapterKeyboardStateValid &&
        keyIndex < 256u &&
        ((comparison.os.keyboardState[keyIndex] & 0x80u) != 0) ==
            ((comparison.adapter.keyboardState[keyIndex] & 0x80u) != 0);
    comparison.cursorMatches = comparison.os.cursorPositionSucceeded &&
        adapterControlStateValid &&
        comparison.os.cursorX == comparison.adapter.cursorX &&
        comparison.os.cursorY == comparison.adapter.cursorY;
    comparison.clipRectangleMatches =
        comparison.os.clipRectangleSucceeded &&
        adapterControlStateValid &&
        comparison.adapter.clipEnabled &&
        comparison.os.clipRectangle == comparison.adapter.clipRectangle;
    comparison.osForegroundIsTarget =
        comparison.os.foregroundWindowRuntimeValue ==
        comparison.targetWindowRuntimeValue;
    comparison.osActiveIsTarget =
        comparison.os.activeWindowRuntimeValue ==
        comparison.targetWindowRuntimeValue;
    comparison.osFocusIsTarget =
        comparison.os.focusWindowRuntimeValue ==
        comparison.targetWindowRuntimeValue;
    comparison.osCaptureIsTarget =
        comparison.os.captureWindowRuntimeValue ==
        comparison.targetWindowRuntimeValue;
    comparison.foregroundMatches = adapterWindowStateValid &&
        comparison.os.foregroundWindowRuntimeValue ==
            comparison.adapter.logicalForegroundWindowRuntimeValue;
    comparison.activeMatches = adapterWindowStateValid &&
        comparison.os.activeWindowRuntimeValue ==
            comparison.adapter.logicalActiveWindowRuntimeValue;
    comparison.focusMatches = adapterWindowStateValid &&
        comparison.os.focusWindowRuntimeValue ==
            comparison.adapter.logicalFocusWindowRuntimeValue;
    comparison.captureMatches = adapterWindowStateValid &&
        comparison.os.captureWindowRuntimeValue ==
            comparison.adapter.virtualCaptureWindowRuntimeValue;
}

std::vector<std::byte> encodeProbeComparison(
    const ProbeComparison& comparison) {
    std::string error;
    if (!validate(comparison, error)) return {};

    Writer writer;
    writer.u32(kProbeSnapshotMagic);
    writer.u16(comparison.schemaVersion);
    writer.u16(kProbeSnapshotHeaderBytes);
    writer.u32(static_cast<std::uint32_t>(kProbeSnapshotWireBytes));
    writer.u32(0);

    writer.u64(comparison.sequence);
    writer.u64(comparison.monotonicTimestampMicros);
    writer.u32(comparison.processId);
    writer.u32(comparison.threadId);
    writer.u32(comparison.seatId);
    writer.u16(comparison.probeVkey);
    writer.padding(2);
    writer.u64(comparison.targetWindowRuntimeValue);

    writer.i16(comparison.os.asyncKeyState);
    writer.i16(comparison.os.keyState);
    writer.boolean(comparison.os.keyboardStateSucceeded);
    writer.padding(3);
    writer.u32(comparison.os.keyboardStateError);
    writer.array(comparison.os.keyboardState);
    writer.boolean(comparison.os.cursorPositionSucceeded);
    writer.padding(3);
    writer.u32(comparison.os.cursorPositionError);
    writer.i32(comparison.os.cursorX);
    writer.i32(comparison.os.cursorY);
    writer.boolean(comparison.os.clipRectangleSucceeded);
    writer.padding(3);
    writer.u32(comparison.os.clipRectangleError);
    writeRect(writer, comparison.os.clipRectangle);
    writer.u64(comparison.os.foregroundWindowRuntimeValue);
    writer.u64(comparison.os.activeWindowRuntimeValue);
    writer.u64(comparison.os.focusWindowRuntimeValue);
    writer.u64(comparison.os.captureWindowRuntimeValue);

    writer.u32(comparison.adapter.snapshotResult);
    writer.u32(comparison.adapter.keyStateResult);
    writer.u32(comparison.adapter.keyboardStateResult);
    writer.u32(comparison.adapter.controlStateResult);
    writer.u32(comparison.adapter.mouseStateResult);
    writer.u32(comparison.adapter.windowStateResult);
    writer.u64(comparison.adapter.lastAppliedSequence);
    writer.u16(comparison.adapter.asyncKeyState);
    writer.u16(comparison.adapter.keyState);
    writer.array(comparison.adapter.keyboardState);
    writer.array(comparison.adapter.keyDownBits);
    writer.array(comparison.adapter.keyPressedEdgeBits);
    writer.u32(comparison.adapter.mouseButtonsDown);
    writer.i64(comparison.adapter.wheelAccumulator);
    writer.i32(comparison.adapter.cursorX);
    writer.i32(comparison.adapter.cursorY);
    writer.boolean(comparison.adapter.clipEnabled);
    writer.boolean(comparison.adapter.virtualForeground);
    writer.boolean(comparison.adapter.virtualCapture);
    writer.padding(1);
    writeRect(writer, comparison.adapter.clipRectangle);
    writer.u64(comparison.adapter.targetWindowRuntimeValue);
    writer.u64(comparison.adapter.logicalForegroundWindowRuntimeValue);
    writer.u64(comparison.adapter.logicalActiveWindowRuntimeValue);
    writer.u64(comparison.adapter.logicalFocusWindowRuntimeValue);
    writer.u64(comparison.adapter.virtualCaptureWindowRuntimeValue);

    writer.boolean(comparison.rawInput.enabled);
    writer.boolean(comparison.rawInput.registrationLifecyclePassed);
    writer.boolean(comparison.rawInput.registrationQueryPassed);
    writer.boolean(comparison.rawInput.dataQueryPassed);
    writer.boolean(comparison.rawInput.bufferReadPassed);
    writer.padding(3);
    writer.u32(comparison.rawInput.registeredCount);
    writer.u32(comparison.rawInput.keyboardExpected);
    writer.u32(comparison.rawInput.keyboardCross);
    writer.u32(comparison.rawInput.mouseExpected);
    writer.u32(comparison.rawInput.mouseCross);
    writer.u32(comparison.rawInput.dataReads);
    writer.u32(comparison.rawInput.bufferPackets);
    writer.u32(comparison.rawInput.apiFailures);
    writer.u32(comparison.rawInput.destroyedTargetFailures);
    writer.u32(comparison.rawInput.staleTokenFailures);
    writer.u32(comparison.rawInput.queueOverflowFailures);
    writer.u32(comparison.rawInput.lastSystemError);

    writer.boolean(comparison.asyncDownMatches);
    writer.boolean(comparison.keyStateDownMatches);
    writer.boolean(comparison.keyboardStateDownMatches);
    writer.boolean(comparison.cursorMatches);
    writer.boolean(comparison.clipRectangleMatches);
    writer.boolean(comparison.foregroundMatches);
    writer.boolean(comparison.activeMatches);
    writer.boolean(comparison.focusMatches);
    writer.boolean(comparison.captureMatches);
    writer.boolean(comparison.osForegroundIsTarget);
    writer.boolean(comparison.osActiveIsTarget);
    writer.boolean(comparison.osFocusIsTarget);
    writer.boolean(comparison.osCaptureIsTarget);
    writer.padding(3);

    auto bytes = writer.take();
    if (bytes.size() != kProbeSnapshotWireBytes ||
        bytes.size() > kMaximumProbeSnapshotBytes) {
        return {};
    }
    return bytes;
}

ProbeSnapshotDecodeResult decodeProbeComparison(
    std::span<const std::byte> bytes) {
    ProbeSnapshotDecodeResult result;
    if (bytes.size() > kMaximumProbeSnapshotBytes) {
        result.error = "probe snapshot exceeds maximum size";
        return result;
    }
    if (bytes.size() != kProbeSnapshotWireBytes) {
        result.error = bytes.size() < kProbeSnapshotWireBytes
                           ? "probe snapshot is truncated"
                           : "probe snapshot size is invalid";
        return result;
    }

    Reader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t headerBytes = 0;
    std::uint32_t encodedBytes = 0;
    if (!reader.u32(magic) || !reader.u16(version) ||
        !reader.u16(headerBytes) || !reader.u32(encodedBytes) ||
        !reader.padding(4)) {
        result.error = reader.error();
        return result;
    }
    if (magic != kProbeSnapshotMagic) {
        result.error = "probe snapshot magic mismatch";
        return result;
    }
    if (version != kProbeSnapshotSchemaVersion) {
        result.error = "unsupported probe snapshot schema version";
        return result;
    }
    if (headerBytes != kProbeSnapshotHeaderBytes ||
        encodedBytes != bytes.size()) {
        result.error = "probe snapshot header size is invalid";
        return result;
    }

    ProbeComparison comparison;
    comparison.schemaVersion = version;
    if (!reader.u64(comparison.sequence) ||
        !reader.u64(comparison.monotonicTimestampMicros) ||
        !reader.u32(comparison.processId) ||
        !reader.u32(comparison.threadId) ||
        !reader.u32(comparison.seatId) ||
        !reader.u16(comparison.probeVkey) || !reader.padding(2) ||
        !reader.u64(comparison.targetWindowRuntimeValue) ||
        !reader.i16(comparison.os.asyncKeyState) ||
        !reader.i16(comparison.os.keyState) ||
        !reader.boolean(comparison.os.keyboardStateSucceeded) ||
        !reader.padding(3) ||
        !reader.u32(comparison.os.keyboardStateError) ||
        !reader.array(comparison.os.keyboardState) ||
        !reader.boolean(comparison.os.cursorPositionSucceeded) ||
        !reader.padding(3) ||
        !reader.u32(comparison.os.cursorPositionError) ||
        !reader.i32(comparison.os.cursorX) ||
        !reader.i32(comparison.os.cursorY) ||
        !reader.boolean(comparison.os.clipRectangleSucceeded) ||
        !reader.padding(3) ||
        !reader.u32(comparison.os.clipRectangleError) ||
        !readRect(reader, comparison.os.clipRectangle) ||
        !reader.u64(comparison.os.foregroundWindowRuntimeValue) ||
        !reader.u64(comparison.os.activeWindowRuntimeValue) ||
        !reader.u64(comparison.os.focusWindowRuntimeValue) ||
        !reader.u64(comparison.os.captureWindowRuntimeValue) ||
        !reader.u32(comparison.adapter.snapshotResult) ||
        !reader.u32(comparison.adapter.keyStateResult) ||
        !reader.u32(comparison.adapter.keyboardStateResult) ||
        !reader.u32(comparison.adapter.controlStateResult) ||
        !reader.u32(comparison.adapter.mouseStateResult) ||
        !reader.u32(comparison.adapter.windowStateResult) ||
        !reader.u64(comparison.adapter.lastAppliedSequence) ||
        !reader.u16(comparison.adapter.asyncKeyState) ||
        !reader.u16(comparison.adapter.keyState) ||
        !reader.array(comparison.adapter.keyboardState) ||
        !reader.array(comparison.adapter.keyDownBits) ||
        !reader.array(comparison.adapter.keyPressedEdgeBits) ||
        !reader.u32(comparison.adapter.mouseButtonsDown) ||
        !reader.i64(comparison.adapter.wheelAccumulator) ||
        !reader.i32(comparison.adapter.cursorX) ||
        !reader.i32(comparison.adapter.cursorY) ||
        !reader.boolean(comparison.adapter.clipEnabled) ||
        !reader.boolean(comparison.adapter.virtualForeground) ||
        !reader.boolean(comparison.adapter.virtualCapture) ||
        !reader.padding(1) ||
        !readRect(reader, comparison.adapter.clipRectangle) ||
        !reader.u64(comparison.adapter.targetWindowRuntimeValue) ||
        !reader.u64(
            comparison.adapter.logicalForegroundWindowRuntimeValue) ||
        !reader.u64(comparison.adapter.logicalActiveWindowRuntimeValue) ||
        !reader.u64(comparison.adapter.logicalFocusWindowRuntimeValue) ||
        !reader.u64(comparison.adapter.virtualCaptureWindowRuntimeValue) ||
        !reader.boolean(comparison.rawInput.enabled) ||
        !reader.boolean(comparison.rawInput.registrationLifecyclePassed) ||
        !reader.boolean(comparison.rawInput.registrationQueryPassed) ||
        !reader.boolean(comparison.rawInput.dataQueryPassed) ||
        !reader.boolean(comparison.rawInput.bufferReadPassed) ||
        !reader.padding(3) ||
        !reader.u32(comparison.rawInput.registeredCount) ||
        !reader.u32(comparison.rawInput.keyboardExpected) ||
        !reader.u32(comparison.rawInput.keyboardCross) ||
        !reader.u32(comparison.rawInput.mouseExpected) ||
        !reader.u32(comparison.rawInput.mouseCross) ||
        !reader.u32(comparison.rawInput.dataReads) ||
        !reader.u32(comparison.rawInput.bufferPackets) ||
        !reader.u32(comparison.rawInput.apiFailures) ||
        !reader.u32(comparison.rawInput.destroyedTargetFailures) ||
        !reader.u32(comparison.rawInput.staleTokenFailures) ||
        !reader.u32(comparison.rawInput.queueOverflowFailures) ||
        !reader.u32(comparison.rawInput.lastSystemError) ||
        !reader.boolean(comparison.asyncDownMatches) ||
        !reader.boolean(comparison.keyStateDownMatches) ||
        !reader.boolean(comparison.keyboardStateDownMatches) ||
        !reader.boolean(comparison.cursorMatches) ||
        !reader.boolean(comparison.clipRectangleMatches) ||
        !reader.boolean(comparison.foregroundMatches) ||
        !reader.boolean(comparison.activeMatches) ||
        !reader.boolean(comparison.focusMatches) ||
        !reader.boolean(comparison.captureMatches) ||
        !reader.boolean(comparison.osForegroundIsTarget) ||
        !reader.boolean(comparison.osActiveIsTarget) ||
        !reader.boolean(comparison.osFocusIsTarget) ||
        !reader.boolean(comparison.osCaptureIsTarget) ||
        !reader.padding(3) || !reader.finished()) {
        result.error = reader.error().empty()
                           ? "probe snapshot contains trailing bytes"
                           : reader.error();
        return result;
    }

    if (!validate(comparison, result.error)) return result;
    result.comparison = std::move(comparison);
    return result;
}

} // namespace hydra::gatec
