#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hydra::gatec {

enum class PollingImport : std::uint8_t {
    GetAsyncKeyState = 0,
    GetKeyState = 1,
    GetKeyboardState = 2
};

enum class CursorFocusImport : std::uint8_t {
    GetCursorPos = 0,
    SetCursorPos = 1,
    ClipCursor = 2,
    GetClipCursor = 3,
    GetForegroundWindow = 4,
    GetActiveWindow = 5,
    GetFocus = 6,
    GetCapture = 7,
    SetCapture = 8,
    ReleaseCapture = 9
};

enum class RawInputImport : std::uint8_t {
    RegisterRawInputDevices = 0,
    GetRegisteredRawInputDevices = 1,
    GetRawInputData = 2,
    GetRawInputBuffer = 3
};

inline constexpr std::size_t kPollingImportCount = 3;
inline constexpr std::uint32_t kPollingImportMask = 0x7u;
inline constexpr std::size_t kCursorFocusImportCount = 10;
inline constexpr std::uint32_t kCursorFocusImportMask = 0x3ffu;
inline constexpr std::size_t kRawInputImportCount = 4;
inline constexpr std::uint32_t kRawInputImportMask = 0x0fu;

constexpr std::uint32_t pollingImportBit(PollingImport value) noexcept {
    return 1u << static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t cursorFocusImportBit(
    CursorFocusImport value) noexcept {
    return 1u << static_cast<std::uint32_t>(value);
}

constexpr std::uint32_t rawInputImportBit(RawInputImport value) noexcept {
    return 1u << static_cast<std::uint32_t>(value);
}

struct PollingIatSlot {
    PollingImport function{PollingImport::GetAsyncKeyState};
    std::uintptr_t* address{nullptr};
    std::uintptr_t original{0};
    std::uintptr_t replacement{0};
};

struct CursorFocusIatSlot {
    CursorFocusImport function{CursorFocusImport::GetCursorPos};
    std::uintptr_t* address{nullptr};
    std::uintptr_t original{0};
    std::uintptr_t replacement{0};
};

struct RawInputIatSlot {
    RawInputImport function{RawInputImport::RegisterRawInputDevices};
    std::uintptr_t* address{nullptr};
    std::uintptr_t original{0};
    std::uintptr_t replacement{0};
};

enum class IatPatchStatus : std::uint8_t {
    Success,
    AlreadyInstalled,
    MissingImport,
    DuplicateImport,
    AlreadyPatched,
    InvalidImage,
    ProtectionFailure,
    PatchFailure,
    RollbackFailure,
    UnsupportedPlatform
};

struct IatWriteResult {
    bool success{false};
    bool valueRestored{true};
    bool protectionRestored{true};
    bool protectionFailure{false};
    std::uint32_t systemError{0};
};

using IatSlotWriter = IatWriteResult (*)(
    std::uintptr_t* address,
    std::uintptr_t expected,
    std::uintptr_t replacement,
    void* context) noexcept;

struct IatPatchReport {
    IatPatchStatus status{IatPatchStatus::InvalidImage};
    std::uint32_t systemError{0};
    std::uint32_t discoveredMask{0};
    std::uint32_t patchedMask{0};
    std::uint32_t restoredMask{0};
    bool rollbackComplete{true};
    std::string error;

    explicit operator bool() const noexcept {
        return status == IatPatchStatus::Success ||
               status == IatPatchStatus::AlreadyInstalled;
    }
};

class PollingIatPatchSet {
public:
    IatPatchReport install(std::span<const PollingIatSlot> slots,
                           IatSlotWriter writer,
                           void* writerContext = nullptr);
    IatPatchReport installProfiled(std::span<const PollingIatSlot> slots,
                                   std::uint32_t requiredMask,
                                   IatSlotWriter writer,
                                   void* writerContext = nullptr);
    IatPatchReport uninstall(IatSlotWriter writer,
                             void* writerContext = nullptr);

    bool installed() const noexcept { return m_installed; }
    const std::vector<PollingIatSlot>& records() const noexcept {
        return m_records;
    }

private:
    std::vector<PollingIatSlot> m_records;
    bool m_installed{false};
};

class CursorFocusIatPatchSet {
public:
    IatPatchReport install(std::span<const CursorFocusIatSlot> slots,
                           IatSlotWriter writer,
                           void* writerContext = nullptr);
    IatPatchReport installProfiled(std::span<const CursorFocusIatSlot> slots,
                                   std::uint32_t requiredMask,
                                   IatSlotWriter writer,
                                   void* writerContext = nullptr);
    IatPatchReport uninstall(IatSlotWriter writer,
                             void* writerContext = nullptr);

    bool installed() const noexcept { return m_installed; }
    const std::vector<CursorFocusIatSlot>& records() const noexcept {
        return m_records;
    }

private:
    std::vector<CursorFocusIatSlot> m_records;
    bool m_installed{false};
};

class RawInputIatPatchSet {
public:
    IatPatchReport install(std::span<const RawInputIatSlot> slots,
                           IatSlotWriter writer,
                           void* writerContext = nullptr);
    IatPatchReport installProfiled(std::span<const RawInputIatSlot> slots,
                                   std::uint32_t requiredMask,
                                   IatSlotWriter writer,
                                   void* writerContext = nullptr);
    IatPatchReport uninstall(IatSlotWriter writer,
                             void* writerContext = nullptr);

    bool installed() const noexcept { return m_installed; }
    const std::vector<RawInputIatSlot>& records() const noexcept {
        return m_records;
    }

private:
    std::vector<RawInputIatSlot> m_records;
    bool m_installed{false};
};

IatWriteResult writeProcessIatSlot(
    std::uintptr_t* address,
    std::uintptr_t expected,
    std::uintptr_t replacement,
    void* context) noexcept;

IatPatchReport discoverCurrentProcessPollingImports(
    const std::array<std::uintptr_t, kPollingImportCount>& replacements,
    std::vector<PollingIatSlot>& slots);
IatPatchReport discoverCurrentProcessPollingImports(
    const std::array<std::uintptr_t, kPollingImportCount>& replacements,
    std::uint32_t requiredMask,
    std::vector<PollingIatSlot>& slots);

IatPatchReport discoverCurrentProcessCursorFocusImports(
    const std::array<std::uintptr_t, kCursorFocusImportCount>& replacements,
    std::vector<CursorFocusIatSlot>& slots);
IatPatchReport discoverCurrentProcessCursorFocusImports(
    const std::array<std::uintptr_t, kCursorFocusImportCount>& replacements,
    std::uint32_t requiredMask,
    std::vector<CursorFocusIatSlot>& slots);

IatPatchReport discoverCurrentProcessRawInputImports(
    const std::array<std::uintptr_t, kRawInputImportCount>& replacements,
    std::vector<RawInputIatSlot>& slots);
IatPatchReport discoverCurrentProcessRawInputImports(
    const std::array<std::uintptr_t, kRawInputImportCount>& replacements,
    std::uint32_t requiredMask,
    std::vector<RawInputIatSlot>& slots);

} // namespace hydra::gatec
