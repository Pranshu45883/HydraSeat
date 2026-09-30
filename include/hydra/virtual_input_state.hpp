#pragma once

#include "hydra/gate_c_protocol.hpp"

#include <array>
#include <cstdint>

namespace hydra::gatec {

inline constexpr std::uint16_t kMouseLeftDown = 0x0001;
inline constexpr std::uint16_t kMouseLeftUp = 0x0002;
inline constexpr std::uint16_t kMouseRightDown = 0x0004;
inline constexpr std::uint16_t kMouseRightUp = 0x0008;
inline constexpr std::uint16_t kMouseMiddleDown = 0x0010;
inline constexpr std::uint16_t kMouseMiddleUp = 0x0020;
inline constexpr std::uint16_t kMouseButton4Down = 0x0040;
inline constexpr std::uint16_t kMouseButton4Up = 0x0080;
inline constexpr std::uint16_t kMouseButton5Down = 0x0100;
inline constexpr std::uint16_t kMouseButton5Up = 0x0200;

class VirtualInputState {
public:
    bool applyInput(std::uint64_t sequence,
                    const InputEventMessage& message);
    bool applyControl(std::uint64_t sequence,
                      const ControlStateMessage& message);
    void setVirtualCursor(std::int32_t x, std::int32_t y) noexcept;
    bool setVirtualClip(bool enabled, std::int32_t left,
                        std::int32_t top, std::int32_t right,
                        std::int32_t bottom) noexcept;

    bool keyDown(std::uint32_t vkey) const noexcept;
    std::uint16_t consumeAsyncKeyState(std::uint32_t vkey) noexcept;
    std::array<std::uint8_t, 256> keyboardState() const noexcept;

    std::uint32_t mouseButtonsDown() const noexcept {
        return m_mouseButtonsDown;
    }
    std::int64_t wheelAccumulator() const noexcept {
        return m_wheelAccumulator;
    }
    std::int32_t cursorX() const noexcept { return m_cursorX; }
    std::int32_t cursorY() const noexcept { return m_cursorY; }
    bool clipEnabled() const noexcept { return m_clipEnabled; }
    std::int32_t clipLeft() const noexcept { return m_clipLeft; }
    std::int32_t clipTop() const noexcept { return m_clipTop; }
    std::int32_t clipRight() const noexcept { return m_clipRight; }
    std::int32_t clipBottom() const noexcept { return m_clipBottom; }
    bool virtualForeground() const noexcept { return m_virtualForeground; }
    bool virtualCapture() const noexcept { return m_virtualCapture; }
    std::uint64_t lastAppliedSequence() const noexcept {
        return m_lastAppliedSequence;
    }

    StateSnapshotMessage snapshot() const noexcept;
    void reset() noexcept;

private:
    static bool bit(const std::array<std::uint8_t, 32>& bits,
                    std::uint32_t index) noexcept;
    static void setBit(std::array<std::uint8_t, 32>& bits,
                       std::uint32_t index, bool value) noexcept;
    void applyMouseButtons(std::uint16_t flags) noexcept;
    void clampCursor() noexcept;

    std::array<std::uint8_t, 32> m_keyDownBits{};
    std::array<std::uint8_t, 32> m_keyPressedEdgeBits{};
    std::uint32_t m_mouseButtonsDown{0};
    std::int64_t m_wheelAccumulator{0};

    std::int32_t m_cursorX{0};
    std::int32_t m_cursorY{0};
    bool m_clipEnabled{false};
    bool m_virtualForeground{false};
    bool m_virtualCapture{false};
    std::int32_t m_clipLeft{0};
    std::int32_t m_clipTop{0};
    std::int32_t m_clipRight{0};
    std::int32_t m_clipBottom{0};

    std::uint64_t m_lastAppliedSequence{0};
};

bool snapshotKeyDown(const StateSnapshotMessage& snapshot,
                     std::uint32_t vkey) noexcept;
bool snapshotKeyPressedEdge(const StateSnapshotMessage& snapshot,
                            std::uint32_t vkey) noexcept;

} // namespace hydra::gatec
