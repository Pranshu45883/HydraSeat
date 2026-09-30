#include "hydra/virtual_input_state.hpp"

#include <algorithm>
#include <limits>

namespace hydra::gatec {
namespace {

std::int32_t saturatingAdd(std::int32_t left, std::int32_t right) noexcept {
    const auto sum = static_cast<std::int64_t>(left) +
                     static_cast<std::int64_t>(right);
    if (sum > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (sum < std::numeric_limits<std::int32_t>::min()) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(sum);
}

std::int64_t saturatingAdd(std::int64_t left, std::int64_t right) noexcept {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return left + right;
}

} // namespace

bool VirtualInputState::bit(const std::array<std::uint8_t, 32>& bits,
                            std::uint32_t index) noexcept {
    if (index >= 256) {
        return false;
    }
    const auto byteIndex = static_cast<std::size_t>(index / 8);
    const auto mask = static_cast<std::uint8_t>(1u << (index % 8));
    return (bits[byteIndex] & mask) != 0;
}

void VirtualInputState::setBit(std::array<std::uint8_t, 32>& bits,
                               std::uint32_t index, bool value) noexcept {
    if (index >= 256) {
        return;
    }
    const auto byteIndex = static_cast<std::size_t>(index / 8);
    const auto mask = static_cast<std::uint8_t>(1u << (index % 8));
    if (value) {
        bits[byteIndex] = static_cast<std::uint8_t>(bits[byteIndex] | mask);
    } else {
        bits[byteIndex] = static_cast<std::uint8_t>(bits[byteIndex] &
                                                    static_cast<std::uint8_t>(~mask));
    }
}

void VirtualInputState::applyMouseButtons(std::uint16_t flags) noexcept {
    const auto setButton = [this, flags](std::uint16_t downFlag,
                                         std::uint16_t upFlag,
                                         std::uint32_t buttonMask) {
        if ((flags & downFlag) != 0) {
            m_mouseButtonsDown |= buttonMask;
        }
        if ((flags & upFlag) != 0) {
            m_mouseButtonsDown &= ~buttonMask;
        }
    };

    setButton(kMouseLeftDown, kMouseLeftUp, 1u << 0);
    setButton(kMouseRightDown, kMouseRightUp, 1u << 1);
    setButton(kMouseMiddleDown, kMouseMiddleUp, 1u << 2);
    setButton(kMouseButton4Down, kMouseButton4Up, 1u << 3);
    setButton(kMouseButton5Down, kMouseButton5Up, 1u << 4);
}

void VirtualInputState::clampCursor() noexcept {
    if (!m_clipEnabled || m_clipRight <= m_clipLeft ||
        m_clipBottom <= m_clipTop) {
        return;
    }
    m_cursorX = std::clamp(m_cursorX, m_clipLeft,
                           static_cast<std::int32_t>(m_clipRight - 1));
    m_cursorY = std::clamp(m_cursorY, m_clipTop,
                           static_cast<std::int32_t>(m_clipBottom - 1));
}

bool VirtualInputState::applyInput(std::uint64_t sequence,
                                   const InputEventMessage& message) {
    if (sequence == 0 || sequence <= m_lastAppliedSequence) {
        return false;
    }

    if (message.kind == InputKind::Keyboard) {
        if (message.vkey >= 256) {
            return false;
        }
        const bool wasDown = bit(m_keyDownBits, message.vkey);
        if (message.keyTransition == KeyTransition::Down) {
            setBit(m_keyDownBits, message.vkey, true);
            if (!wasDown) {
                setBit(m_keyPressedEdgeBits, message.vkey, true);
            }
        } else if (message.keyTransition == KeyTransition::Up) {
            setBit(m_keyDownBits, message.vkey, false);
        } else {
            return false;
        }
    } else if (message.kind == InputKind::Mouse) {
        m_cursorX = saturatingAdd(m_cursorX, message.deltaX);
        m_cursorY = saturatingAdd(m_cursorY, message.deltaY);
        m_wheelAccumulator = saturatingAdd(
            m_wheelAccumulator, static_cast<std::int64_t>(message.wheelDelta));
        applyMouseButtons(message.mouseButtonFlags);
        clampCursor();
    } else {
        return false;
    }

    m_lastAppliedSequence = sequence;
    return true;
}

bool VirtualInputState::applyControl(std::uint64_t sequence,
                                     const ControlStateMessage& message) {
    if (sequence == 0 || sequence <= m_lastAppliedSequence) {
        return false;
    }
    if (message.clipEnabled &&
        (message.clipRight <= message.clipLeft ||
         message.clipBottom <= message.clipTop)) {
        return false;
    }

    m_cursorX = message.cursorX;
    m_cursorY = message.cursorY;
    m_clipEnabled = message.clipEnabled;
    m_virtualForeground = message.virtualForeground;
    m_virtualCapture = message.virtualCapture;
    m_clipLeft = message.clipLeft;
    m_clipTop = message.clipTop;
    m_clipRight = message.clipRight;
    m_clipBottom = message.clipBottom;
    clampCursor();
    m_lastAppliedSequence = sequence;
    return true;
}

void VirtualInputState::setVirtualCursor(std::int32_t x,
                                         std::int32_t y) noexcept {
    m_cursorX = x;
    m_cursorY = y;
    clampCursor();
}

bool VirtualInputState::setVirtualClip(bool enabled, std::int32_t left,
                                       std::int32_t top,
                                       std::int32_t right,
                                       std::int32_t bottom) noexcept {
    if (enabled && (right <= left || bottom <= top)) return false;
    m_clipEnabled = enabled;
    m_clipLeft = enabled ? left : 0;
    m_clipTop = enabled ? top : 0;
    m_clipRight = enabled ? right : 0;
    m_clipBottom = enabled ? bottom : 0;
    clampCursor();
    return true;
}

bool VirtualInputState::keyDown(std::uint32_t vkey) const noexcept {
    return bit(m_keyDownBits, vkey);
}

std::uint16_t VirtualInputState::consumeAsyncKeyState(
    std::uint32_t vkey) noexcept {
    if (vkey >= 256) {
        return 0;
    }
    std::uint16_t result = 0;
    if (bit(m_keyDownBits, vkey)) {
        result = static_cast<std::uint16_t>(result | 0x8000u);
    }
    if (bit(m_keyPressedEdgeBits, vkey)) {
        result = static_cast<std::uint16_t>(result | 0x0001u);
        setBit(m_keyPressedEdgeBits, vkey, false);
    }
    return result;
}

std::array<std::uint8_t, 256> VirtualInputState::keyboardState() const noexcept {
    std::array<std::uint8_t, 256> state{};
    for (std::uint32_t vkey = 0; vkey < 256; ++vkey) {
        if (bit(m_keyDownBits, vkey)) {
            state[static_cast<std::size_t>(vkey)] = 0x80u;
        }
    }
    return state;
}

StateSnapshotMessage VirtualInputState::snapshot() const noexcept {
    StateSnapshotMessage result;
    result.lastAppliedSequence = m_lastAppliedSequence;
    result.keyDownBits = m_keyDownBits;
    result.keyPressedEdgeBits = m_keyPressedEdgeBits;
    result.mouseButtonsDown = m_mouseButtonsDown;
    result.wheelAccumulator = m_wheelAccumulator;
    result.cursorX = m_cursorX;
    result.cursorY = m_cursorY;
    result.clipEnabled = m_clipEnabled;
    result.virtualForeground = m_virtualForeground;
    result.virtualCapture = m_virtualCapture;
    result.clipLeft = m_clipLeft;
    result.clipTop = m_clipTop;
    result.clipRight = m_clipRight;
    result.clipBottom = m_clipBottom;
    return result;
}

void VirtualInputState::reset() noexcept {
    m_keyDownBits.fill(0);
    m_keyPressedEdgeBits.fill(0);
    m_mouseButtonsDown = 0;
    m_wheelAccumulator = 0;
    m_cursorX = 0;
    m_cursorY = 0;
    m_clipEnabled = false;
    m_virtualForeground = false;
    m_virtualCapture = false;
    m_clipLeft = 0;
    m_clipTop = 0;
    m_clipRight = 0;
    m_clipBottom = 0;
    m_lastAppliedSequence = 0;
}

bool snapshotKeyDown(const StateSnapshotMessage& snapshot,
                     std::uint32_t vkey) noexcept {
    if (vkey >= 256) {
        return false;
    }
    const auto byteIndex = static_cast<std::size_t>(vkey / 8);
    const auto mask = static_cast<std::uint8_t>(1u << (vkey % 8));
    return (snapshot.keyDownBits[byteIndex] & mask) != 0;
}

bool snapshotKeyPressedEdge(const StateSnapshotMessage& snapshot,
                            std::uint32_t vkey) noexcept {
    if (vkey >= 256) {
        return false;
    }
    const auto byteIndex = static_cast<std::size_t>(vkey / 8);
    const auto mask = static_cast<std::uint8_t>(1u << (vkey % 8));
    return (snapshot.keyPressedEdgeBits[byteIndex] & mask) != 0;
}

} // namespace hydra::gatec
