#include "hydra/win32_iat_patch.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hydra::gatec {
namespace {

IatPatchReport failure(IatPatchStatus status, std::string error,
                       std::uint32_t systemError = 0) {
    IatPatchReport report;
    report.status = status;
    report.systemError = systemError;
    report.error = std::move(error);
    return report;
}

bool validFunction(PollingImport function) noexcept {
    return static_cast<std::size_t>(function) < kPollingImportCount;
}

bool validFunction(CursorFocusImport function) noexcept {
    return static_cast<std::size_t>(function) < kCursorFocusImportCount;
}

bool validFunction(RawInputImport function) noexcept {
    return static_cast<std::size_t>(function) < kRawInputImportCount;
}

#ifdef _WIN32

bool imageRange(std::uint32_t rva, std::size_t bytes,
                std::size_t imageBytes) noexcept {
    const auto start = static_cast<std::size_t>(rva);
    return start <= imageBytes && bytes <= imageBytes - start;
}

bool boundedAsciiString(const std::byte* base, std::size_t imageBytes,
                        std::uint32_t rva, std::string_view& value) noexcept {
    if (!imageRange(rva, 1, imageBytes)) return false;
    const char* text = reinterpret_cast<const char*>(base + rva);
    const std::size_t maximum = imageBytes - static_cast<std::size_t>(rva);
    const void* terminator = std::memchr(text, '\0', maximum);
    if (terminator == nullptr) return false;
    value = std::string_view(
        text, static_cast<const char*>(terminator) - text);
    return true;
}

char asciiLower(char value) noexcept {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

bool asciiEqual(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](char a, char b) {
                          return asciiLower(a) == asciiLower(b);
                      });
}

bool asciiStartsWith(std::string_view value,
                     std::string_view prefix) noexcept {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin(),
                      [](char a, char b) {
                          return asciiLower(a) == asciiLower(b);
                      });
}

bool allowedPollingModule(std::string_view module) noexcept {
    // A forwarded export is already resolved in FirstThunk by the loader.
    // API-set imports still need an explicit source-module allowlist here.
    return asciiEqual(module, "user32.dll") ||
           asciiStartsWith(module, "api-ms-win-ntuser-") ||
           asciiStartsWith(module, "ext-ms-win-ntuser-");
}

std::optional<PollingImport> pollingFunction(
    std::string_view name) noexcept {
    if (name == "GetAsyncKeyState") return PollingImport::GetAsyncKeyState;
    if (name == "GetKeyState") return PollingImport::GetKeyState;
    if (name == "GetKeyboardState") return PollingImport::GetKeyboardState;
    return std::nullopt;
}

std::optional<CursorFocusImport> cursorFocusFunction(
    std::string_view name) noexcept {
    if (name == "GetCursorPos") return CursorFocusImport::GetCursorPos;
    if (name == "SetCursorPos") return CursorFocusImport::SetCursorPos;
    if (name == "ClipCursor") return CursorFocusImport::ClipCursor;
    if (name == "GetClipCursor") return CursorFocusImport::GetClipCursor;
    if (name == "GetForegroundWindow") {
        return CursorFocusImport::GetForegroundWindow;
    }
    if (name == "GetActiveWindow") return CursorFocusImport::GetActiveWindow;
    if (name == "GetFocus") return CursorFocusImport::GetFocus;
    if (name == "GetCapture") return CursorFocusImport::GetCapture;
    if (name == "SetCapture") return CursorFocusImport::SetCapture;
    if (name == "ReleaseCapture") return CursorFocusImport::ReleaseCapture;
    return std::nullopt;
}

std::optional<RawInputImport> rawInputFunction(
    std::string_view name) noexcept {
    if (name == "RegisterRawInputDevices") {
        return RawInputImport::RegisterRawInputDevices;
    }
    if (name == "GetRegisteredRawInputDevices") {
        return RawInputImport::GetRegisteredRawInputDevices;
    }
    if (name == "GetRawInputData") return RawInputImport::GetRawInputData;
    if (name == "GetRawInputBuffer") return RawInputImport::GetRawInputBuffer;
    return std::nullopt;
}

#endif

} // namespace

IatPatchReport PollingIatPatchSet::install(
    std::span<const PollingIatSlot> slots, IatSlotWriter writer,
    void* writerContext) {
    return installProfiled(slots, kPollingImportMask, writer, writerContext);
}

IatPatchReport PollingIatPatchSet::installProfiled(
    std::span<const PollingIatSlot> slots, std::uint32_t requiredMask,
    IatSlotWriter writer, void* writerContext) {
    if (requiredMask == 0 || (requiredMask & ~kPollingImportMask) != 0) {
        return failure(IatPatchStatus::InvalidImage,
                       "polling profiled mask is invalid");
    }
    if (m_installed) {
        IatPatchReport report;
        report.status = IatPatchStatus::AlreadyInstalled;
        for (const auto& record : m_records) {
            report.discoveredMask |= pollingImportBit(record.function);
            report.patchedMask |= pollingImportBit(record.function);
        }
        if (report.discoveredMask != requiredMask) {
            report.status = IatPatchStatus::PatchFailure;
            report.error = "polling shim is already installed with a different profile mask";
        }
        return report;
    }
    if (writer == nullptr) {
        return failure(IatPatchStatus::PatchFailure,
                       "IAT slot writer is missing");
    }

    std::array<const PollingIatSlot*, kPollingImportCount> ordered{};
    std::uint32_t discoveredMask = 0;
    for (const auto& slot : slots) {
        if (!validFunction(slot.function) || slot.address == nullptr ||
            slot.original == 0 || slot.replacement == 0) {
            auto report = failure(IatPatchStatus::InvalidImage,
                                  "polling IAT slot is invalid");
            report.discoveredMask = discoveredMask;
            return report;
        }
        const auto index = static_cast<std::size_t>(slot.function);
        const auto bit = pollingImportBit(slot.function);
        if (ordered[index] != nullptr) {
            auto report = failure(IatPatchStatus::DuplicateImport,
                                  "polling import occurs more than once");
            report.discoveredMask = discoveredMask | bit;
            return report;
        }
        ordered[index] = &slot;
        discoveredMask |= bit;
    }
    if (discoveredMask != requiredMask) {
        auto report = failure(IatPatchStatus::MissingImport,
                              "one or more required polling imports are missing");
        report.discoveredMask = discoveredMask;
        return report;
    }
    for (const auto* slot : ordered) {
        if (slot == nullptr) continue;
        if (slot->original == slot->replacement ||
            *slot->address == slot->replacement) {
            auto report = failure(IatPatchStatus::AlreadyPatched,
                                  "polling import is already patched");
            report.discoveredMask = discoveredMask;
            return report;
        }
        if (*slot->address != slot->original) {
            auto report = failure(IatPatchStatus::PatchFailure,
                                  "polling import changed after discovery");
            report.discoveredMask = discoveredMask;
            return report;
        }
    }

    std::vector<PollingIatSlot> applied;
    applied.reserve(slots.size());
    std::vector<PollingIatSlot> rollbackRemaining;
    rollbackRemaining.reserve(slots.size());
    IatPatchReport report;
    report.status = IatPatchStatus::Success;
    report.discoveredMask = discoveredMask;
    for (const auto* slot : ordered) {
        if (slot == nullptr) continue;
        const auto write = writer(slot->address, slot->original,
                                  slot->replacement, writerContext);
        if (write.success) {
            applied.push_back(*slot);
            report.patchedMask |= pollingImportBit(slot->function);
            continue;
        }

        report.status = write.protectionFailure
                            ? IatPatchStatus::ProtectionFailure
                            : IatPatchStatus::PatchFailure;
        report.systemError = write.systemError;
        report.rollbackComplete = write.valueRestored &&
                                  write.protectionRestored;
        if (!write.valueRestored) applied.push_back(*slot);
        for (auto current = applied.rbegin(); current != applied.rend();
             ++current) {
            if (*current->address == current->original) {
                report.restoredMask |= pollingImportBit(current->function);
                continue;
            }
            const auto restored = writer(
                current->address, current->replacement, current->original,
                writerContext);
            if (restored.success) {
                report.restoredMask |= pollingImportBit(current->function);
            } else {
                rollbackRemaining.push_back(*current);
                report.rollbackComplete = false;
                if (report.systemError == 0) {
                    report.systemError = restored.systemError;
                }
            }
        }
        report.patchedMask = report.rollbackComplete ? 0u
                                                     : report.patchedMask;
        if (!report.rollbackComplete) {
            report.status = IatPatchStatus::RollbackFailure;
            if (!rollbackRemaining.empty()) {
                std::reverse(rollbackRemaining.begin(),
                             rollbackRemaining.end());
                m_records = std::move(rollbackRemaining);
                m_installed = true;
                report.patchedMask = 0;
                for (const auto& remaining : m_records) {
                    report.patchedMask |= pollingImportBit(
                        remaining.function);
                }
            }
        }
        return report;
    }

    m_records = std::move(applied);
    m_installed = true;
    return report;
}

IatPatchReport PollingIatPatchSet::uninstall(
    IatSlotWriter writer, void* writerContext) {
    IatPatchReport report;
    report.status = IatPatchStatus::Success;
    report.rollbackComplete = true;
    for (const auto& record : m_records) {
        report.discoveredMask |= pollingImportBit(record.function);
        report.patchedMask |= pollingImportBit(record.function);
    }
    if (!m_installed || m_records.empty()) return report;
    if (writer == nullptr) {
        report.status = IatPatchStatus::RollbackFailure;
        report.rollbackComplete = false;
        return report;
    }

    std::vector<PollingIatSlot> remaining;
    remaining.reserve(m_records.size());
    for (auto current = m_records.rbegin(); current != m_records.rend();
         ++current) {
        if (*current->address == current->original) {
            report.restoredMask |= pollingImportBit(current->function);
            continue;
        }
        const auto restored = writer(
            current->address, current->replacement, current->original,
            writerContext);
        if (restored.success) {
            report.restoredMask |= pollingImportBit(current->function);
        } else {
            remaining.push_back(*current);
            report.rollbackComplete = false;
            if (report.systemError == 0) report.systemError = restored.systemError;
        }
    }
    if (!remaining.empty()) {
        std::reverse(remaining.begin(), remaining.end());
        m_records = std::move(remaining);
        report.status = IatPatchStatus::RollbackFailure;
        return report;
    }
    m_records.clear();
    m_installed = false;
    report.patchedMask = 0;
    return report;
}

IatPatchReport CursorFocusIatPatchSet::install(
    std::span<const CursorFocusIatSlot> slots, IatSlotWriter writer,
    void* writerContext) {
    return installProfiled(slots, kCursorFocusImportMask, writer, writerContext);
}

IatPatchReport CursorFocusIatPatchSet::installProfiled(
    std::span<const CursorFocusIatSlot> slots, std::uint32_t requiredMask,
    IatSlotWriter writer, void* writerContext) {
    if (requiredMask == 0 || (requiredMask & ~kCursorFocusImportMask) != 0) {
        return failure(IatPatchStatus::InvalidImage,
                       "cursor/focus profiled mask is invalid");
    }
    if (m_installed) {
        IatPatchReport report;
        report.status = IatPatchStatus::AlreadyInstalled;
        for (const auto& record : m_records) {
            report.discoveredMask |= cursorFocusImportBit(record.function);
            report.patchedMask |= cursorFocusImportBit(record.function);
        }
        if (report.discoveredMask != requiredMask) {
            report.status = IatPatchStatus::PatchFailure;
            report.error = "cursor/focus shim is already installed with a different profile mask";
        }
        return report;
    }
    if (writer == nullptr) {
        return failure(IatPatchStatus::PatchFailure,
                       "cursor/focus IAT slot writer is missing");
    }

    std::array<const CursorFocusIatSlot*, kCursorFocusImportCount> ordered{};
    std::uint32_t discoveredMask = 0;
    for (const auto& slot : slots) {
        if (!validFunction(slot.function) || slot.address == nullptr ||
            slot.original == 0 || slot.replacement == 0) {
            auto report = failure(IatPatchStatus::InvalidImage,
                                  "cursor/focus IAT slot is invalid");
            report.discoveredMask = discoveredMask;
            return report;
        }
        const auto index = static_cast<std::size_t>(slot.function);
        const auto bit = cursorFocusImportBit(slot.function);
        if (ordered[index] != nullptr) {
            auto report = failure(IatPatchStatus::DuplicateImport,
                                  "cursor/focus import occurs more than once");
            report.discoveredMask = discoveredMask | bit;
            return report;
        }
        ordered[index] = &slot;
        discoveredMask |= bit;
    }
    if (discoveredMask != requiredMask) {
        auto report = failure(IatPatchStatus::MissingImport,
                              "one or more required cursor/focus imports are missing");
        report.discoveredMask = discoveredMask;
        return report;
    }
    for (const auto* slot : ordered) {
        if (slot == nullptr) continue;
        if (slot->original == slot->replacement ||
            *slot->address == slot->replacement) {
            auto report = failure(IatPatchStatus::AlreadyPatched,
                                  "cursor/focus import is already patched");
            report.discoveredMask = discoveredMask;
            return report;
        }
        if (*slot->address != slot->original) {
            auto report = failure(IatPatchStatus::PatchFailure,
                                  "cursor/focus import changed after discovery");
            report.discoveredMask = discoveredMask;
            return report;
        }
    }

    std::vector<CursorFocusIatSlot> applied;
    applied.reserve(slots.size());
    std::vector<CursorFocusIatSlot> rollbackRemaining;
    rollbackRemaining.reserve(slots.size());
    IatPatchReport report;
    report.status = IatPatchStatus::Success;
    report.discoveredMask = discoveredMask;
    for (const auto* slot : ordered) {
        if (slot == nullptr) continue;
        const auto write = writer(slot->address, slot->original,
                                  slot->replacement, writerContext);
        if (write.success) {
            applied.push_back(*slot);
            report.patchedMask |= cursorFocusImportBit(slot->function);
            continue;
        }

        report.status = write.protectionFailure
                            ? IatPatchStatus::ProtectionFailure
                            : IatPatchStatus::PatchFailure;
        report.systemError = write.systemError;
        report.rollbackComplete = write.valueRestored &&
                                  write.protectionRestored;
        if (!write.valueRestored) applied.push_back(*slot);
        for (auto current = applied.rbegin(); current != applied.rend();
             ++current) {
            if (*current->address == current->original) {
                report.restoredMask |= cursorFocusImportBit(current->function);
                continue;
            }
            const auto restored = writer(
                current->address, current->replacement, current->original,
                writerContext);
            if (restored.success) {
                report.restoredMask |= cursorFocusImportBit(current->function);
            } else {
                rollbackRemaining.push_back(*current);
                report.rollbackComplete = false;
                if (report.systemError == 0) {
                    report.systemError = restored.systemError;
                }
            }
        }
        report.patchedMask = report.rollbackComplete ? 0u
                                                     : report.patchedMask;
        if (!report.rollbackComplete) {
            report.status = IatPatchStatus::RollbackFailure;
            if (!rollbackRemaining.empty()) {
                std::reverse(rollbackRemaining.begin(),
                             rollbackRemaining.end());
                m_records = std::move(rollbackRemaining);
                m_installed = true;
                report.patchedMask = 0;
                for (const auto& remaining : m_records) {
                    report.patchedMask |= cursorFocusImportBit(
                        remaining.function);
                }
            }
        }
        return report;
    }

    m_records = std::move(applied);
    m_installed = true;
    return report;
}

IatPatchReport CursorFocusIatPatchSet::uninstall(
    IatSlotWriter writer, void* writerContext) {
    IatPatchReport report;
    report.status = IatPatchStatus::Success;
    report.rollbackComplete = true;
    for (const auto& record : m_records) {
        report.discoveredMask |= cursorFocusImportBit(record.function);
        report.patchedMask |= cursorFocusImportBit(record.function);
    }
    if (!m_installed || m_records.empty()) return report;
    if (writer == nullptr) {
        report.status = IatPatchStatus::RollbackFailure;
        report.rollbackComplete = false;
        return report;
    }

    std::vector<CursorFocusIatSlot> remaining;
    remaining.reserve(m_records.size());
    for (auto current = m_records.rbegin(); current != m_records.rend();
         ++current) {
        if (*current->address == current->original) {
            report.restoredMask |= cursorFocusImportBit(current->function);
            continue;
        }
        const auto restored = writer(
            current->address, current->replacement, current->original,
            writerContext);
        if (restored.success) {
            report.restoredMask |= cursorFocusImportBit(current->function);
        } else {
            remaining.push_back(*current);
            report.rollbackComplete = false;
            if (report.systemError == 0) report.systemError = restored.systemError;
        }
    }
    if (!remaining.empty()) {
        std::reverse(remaining.begin(), remaining.end());
        m_records = std::move(remaining);
        report.status = IatPatchStatus::RollbackFailure;
        return report;
    }
    m_records.clear();
    m_installed = false;
    report.patchedMask = 0;
    return report;
}

IatPatchReport RawInputIatPatchSet::install(
    std::span<const RawInputIatSlot> slots, IatSlotWriter writer,
    void* writerContext) {
    return installProfiled(slots, kRawInputImportMask, writer, writerContext);
}

IatPatchReport RawInputIatPatchSet::installProfiled(
    std::span<const RawInputIatSlot> slots, std::uint32_t requiredMask,
    IatSlotWriter writer, void* writerContext) {
    if (requiredMask == 0 || (requiredMask & ~kRawInputImportMask) != 0) {
        return failure(IatPatchStatus::InvalidImage,
                       "Raw Input profiled mask is invalid");
    }
    if (m_installed) {
        IatPatchReport report;
        report.status = IatPatchStatus::AlreadyInstalled;
        for (const auto& record : m_records) {
            report.discoveredMask |= rawInputImportBit(record.function);
            report.patchedMask |= rawInputImportBit(record.function);
        }
        if (report.discoveredMask != requiredMask) {
            report.status = IatPatchStatus::PatchFailure;
            report.error = "Raw Input shim is already installed with a different profile mask";
        }
        return report;
    }
    if (writer == nullptr) {
        return failure(IatPatchStatus::PatchFailure,
                       "Raw Input IAT slot writer is missing");
    }
    std::array<const RawInputIatSlot*, kRawInputImportCount> ordered{};
    std::uint32_t discoveredMask = 0;
    for (const auto& slot : slots) {
        if (!validFunction(slot.function) || slot.address == nullptr ||
            slot.original == 0 || slot.replacement == 0) {
            auto report = failure(IatPatchStatus::InvalidImage,
                                  "Raw Input IAT slot is invalid");
            report.discoveredMask = discoveredMask;
            return report;
        }
        const auto index = static_cast<std::size_t>(slot.function);
        const auto bit = rawInputImportBit(slot.function);
        if (ordered[index] != nullptr) {
            auto report = failure(IatPatchStatus::DuplicateImport,
                                  "Raw Input import occurs more than once");
            report.discoveredMask = discoveredMask | bit;
            return report;
        }
        ordered[index] = &slot;
        discoveredMask |= bit;
    }
    if (discoveredMask != requiredMask) {
        auto report = failure(IatPatchStatus::MissingImport,
                              "one or more required Raw Input imports are missing");
        report.discoveredMask = discoveredMask;
        return report;
    }
    for (const auto* slot : ordered) {
        if (slot == nullptr) continue;
        if (slot->original == slot->replacement ||
            *slot->address == slot->replacement) {
            auto report = failure(IatPatchStatus::AlreadyPatched,
                                  "Raw Input import is already patched");
            report.discoveredMask = discoveredMask;
            return report;
        }
        if (*slot->address != slot->original) {
            auto report = failure(IatPatchStatus::PatchFailure,
                                  "Raw Input import changed after discovery");
            report.discoveredMask = discoveredMask;
            return report;
        }
    }

    std::vector<RawInputIatSlot> applied;
    applied.reserve(slots.size());
    std::vector<RawInputIatSlot> rollbackRemaining;
    rollbackRemaining.reserve(slots.size());
    IatPatchReport report;
    report.status = IatPatchStatus::Success;
    report.discoveredMask = discoveredMask;
    for (const auto* slot : ordered) {
        if (slot == nullptr) continue;
        const auto write = writer(slot->address, slot->original,
                                  slot->replacement, writerContext);
        if (write.success) {
            applied.push_back(*slot);
            report.patchedMask |= rawInputImportBit(slot->function);
            continue;
        }
        report.status = write.protectionFailure
                            ? IatPatchStatus::ProtectionFailure
                            : IatPatchStatus::PatchFailure;
        report.systemError = write.systemError;
        report.rollbackComplete = write.valueRestored &&
                                  write.protectionRestored;
        if (!write.valueRestored) applied.push_back(*slot);
        for (auto current = applied.rbegin(); current != applied.rend();
             ++current) {
            if (*current->address == current->original) {
                report.restoredMask |= rawInputImportBit(current->function);
                continue;
            }
            const auto restored = writer(
                current->address, current->replacement, current->original,
                writerContext);
            if (restored.success) {
                report.restoredMask |= rawInputImportBit(current->function);
            } else {
                rollbackRemaining.push_back(*current);
                report.rollbackComplete = false;
                if (report.systemError == 0) {
                    report.systemError = restored.systemError;
                }
            }
        }
        report.patchedMask = report.rollbackComplete ? 0u
                                                     : report.patchedMask;
        if (!report.rollbackComplete) {
            report.status = IatPatchStatus::RollbackFailure;
            if (!rollbackRemaining.empty()) {
                std::reverse(rollbackRemaining.begin(),
                             rollbackRemaining.end());
                m_records = std::move(rollbackRemaining);
                m_installed = true;
                report.patchedMask = 0;
                for (const auto& remaining : m_records) {
                    report.patchedMask |= rawInputImportBit(
                        remaining.function);
                }
            }
        }
        return report;
    }
    m_records = std::move(applied);
    m_installed = true;
    return report;
}

IatPatchReport RawInputIatPatchSet::uninstall(
    IatSlotWriter writer, void* writerContext) {
    IatPatchReport report;
    report.status = IatPatchStatus::Success;
    report.rollbackComplete = true;
    for (const auto& record : m_records) {
        report.discoveredMask |= rawInputImportBit(record.function);
        report.patchedMask |= rawInputImportBit(record.function);
    }
    if (!m_installed || m_records.empty()) return report;
    if (writer == nullptr) {
        report.status = IatPatchStatus::RollbackFailure;
        report.rollbackComplete = false;
        return report;
    }
    std::vector<RawInputIatSlot> remaining;
    remaining.reserve(m_records.size());
    for (auto current = m_records.rbegin(); current != m_records.rend();
         ++current) {
        if (*current->address == current->original) {
            report.restoredMask |= rawInputImportBit(current->function);
            continue;
        }
        const auto restored = writer(
            current->address, current->replacement, current->original,
            writerContext);
        if (restored.success) {
            report.restoredMask |= rawInputImportBit(current->function);
        } else {
            remaining.push_back(*current);
            report.rollbackComplete = false;
            if (report.systemError == 0) {
                report.systemError = restored.systemError;
            }
        }
    }
    if (!remaining.empty()) {
        std::reverse(remaining.begin(), remaining.end());
        m_records = std::move(remaining);
        report.status = IatPatchStatus::RollbackFailure;
        return report;
    }
    m_records.clear();
    m_installed = false;
    report.patchedMask = 0;
    return report;
}

IatWriteResult writeProcessIatSlot(
    std::uintptr_t* address, std::uintptr_t expected,
    std::uintptr_t replacement, void* context) noexcept {
    (void)context;
    IatWriteResult result;
#ifdef _WIN32
    if (address == nullptr || expected == 0 || replacement == 0) {
        result.systemError = ERROR_INVALID_PARAMETER;
        return result;
    }
    if (*address != expected) {
        result.systemError = ERROR_INVALID_DATA;
        return result;
    }

    DWORD originalProtection = 0;
    if (VirtualProtect(address, sizeof(*address), PAGE_READWRITE,
                       &originalProtection) == FALSE) {
        result.protectionFailure = true;
        result.systemError = GetLastError();
        return result;
    }
    if (*address != expected) {
        DWORD ignored = 0;
        result.protectionRestored =
            VirtualProtect(address, sizeof(*address), originalProtection,
                           &ignored) != FALSE;
        result.protectionFailure = !result.protectionRestored;
        result.systemError = result.protectionRestored
                                 ? ERROR_INVALID_DATA
                                 : GetLastError();
        return result;
    }

    InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile*>(address),
        reinterpret_cast<PVOID>(replacement));
    DWORD ignored = 0;
    if (VirtualProtect(address, sizeof(*address), originalProtection,
                       &ignored) == FALSE) {
        result.protectionFailure = true;
        result.systemError = GetLastError();
        InterlockedExchangePointer(
            reinterpret_cast<PVOID volatile*>(address),
            reinterpret_cast<PVOID>(expected));
        result.valueRestored = *address == expected;
        DWORD retryIgnored = 0;
        result.protectionRestored =
            VirtualProtect(address, sizeof(*address), originalProtection,
                           &retryIgnored) != FALSE;
        return result;
    }
    result.success = true;
    return result;
#else
    (void)address;
    (void)expected;
    (void)replacement;
    result.systemError = 0;
    return result;
#endif
}

IatPatchReport discoverCurrentProcessPollingImports(
    const std::array<std::uintptr_t, kPollingImportCount>& replacements,
    std::vector<PollingIatSlot>& slots) {
    return discoverCurrentProcessPollingImports(
        replacements, kPollingImportMask, slots);
}

IatPatchReport discoverCurrentProcessPollingImports(
    const std::array<std::uintptr_t, kPollingImportCount>& replacements,
    std::uint32_t requiredMask,
    std::vector<PollingIatSlot>& slots) {
    slots.clear();
    if (requiredMask == 0 || (requiredMask & ~kPollingImportMask) != 0) {
        return failure(IatPatchStatus::InvalidImage,
                       "polling required mask is invalid");
    }
#ifdef _WIN32
    const HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable module is unavailable",
                       GetLastError());
    }
    const auto* base = reinterpret_cast<const std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<std::size_t>(dos->e_lfanew) > 4096u) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable DOS header is invalid");
    }
#if defined(_WIN64)
    using NtHeaders = IMAGE_NT_HEADERS64;
    using ThunkData = IMAGE_THUNK_DATA64;
    constexpr WORD kOptionalMagic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#else
    using NtHeaders = IMAGE_NT_HEADERS32;
    using ThunkData = IMAGE_THUNK_DATA32;
    constexpr WORD kOptionalMagic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#endif
    const auto* nt = reinterpret_cast<const NtHeaders*>(
        base + static_cast<std::size_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != kOptionalMagic ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(nt->OptionalHeader)) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable NT header is invalid");
    }
    const std::size_t imageBytes = nt->OptionalHeader.SizeOfImage;
    if (imageBytes < sizeof(IMAGE_DOS_HEADER) ||
        nt->OptionalHeader.NumberOfRvaAndSizes <=
            IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable image size is invalid");
    }
    const auto directory = nt->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0 ||
        !imageRange(directory.VirtualAddress, directory.Size, imageBytes)) {
        return failure(IatPatchStatus::MissingImport,
                       "current executable has no bounded import directory");
    }
    const auto* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    const std::size_t descriptorLimit =
        directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    bool terminated = false;
    std::uint32_t discoveredMask = 0;
    for (std::size_t descriptorIndex = 0;
         descriptorIndex < descriptorLimit; ++descriptorIndex) {
        const auto& descriptor = descriptors[descriptorIndex];
        if (descriptor.Name == 0 && descriptor.FirstThunk == 0 &&
            descriptor.OriginalFirstThunk == 0) {
            terminated = true;
            break;
        }
        std::string_view moduleName;
        if (!boundedAsciiString(base, imageBytes, descriptor.Name,
                                moduleName)) {
            return failure(IatPatchStatus::InvalidImage,
                           "import module name is not bounded");
        }
        if (!allowedPollingModule(moduleName)) continue;
        if (descriptor.OriginalFirstThunk == 0 ||
            descriptor.FirstThunk == 0 ||
            !imageRange(descriptor.OriginalFirstThunk, sizeof(ThunkData),
                        imageBytes) ||
            !imageRange(descriptor.FirstThunk, sizeof(ThunkData),
                        imageBytes)) {
            return failure(IatPatchStatus::InvalidImage,
                           "polling import thunk metadata is invalid");
        }
        const std::size_t thunkLimit = imageBytes / sizeof(ThunkData);
        bool thunkTerminated = false;
        for (std::size_t thunkIndex = 0; thunkIndex < thunkLimit;
             ++thunkIndex) {
            const auto originalRva = static_cast<std::uint64_t>(
                descriptor.OriginalFirstThunk) +
                thunkIndex * sizeof(ThunkData);
            const auto firstRva = static_cast<std::uint64_t>(
                descriptor.FirstThunk) + thunkIndex * sizeof(ThunkData);
            if (originalRva > (std::numeric_limits<std::uint32_t>::max)() ||
                firstRva > (std::numeric_limits<std::uint32_t>::max)() ||
                !imageRange(static_cast<std::uint32_t>(originalRva),
                            sizeof(ThunkData), imageBytes) ||
                !imageRange(static_cast<std::uint32_t>(firstRva),
                            sizeof(ThunkData), imageBytes)) {
                return failure(IatPatchStatus::InvalidImage,
                               "polling import thunk is not bounded");
            }
            const auto* originalThunk = reinterpret_cast<const ThunkData*>(
                base + static_cast<std::uint32_t>(originalRva));
            if (originalThunk->u1.AddressOfData == 0) {
                thunkTerminated = true;
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal)) {
                // Ordinals cannot prove membership in the function allowlist.
                continue;
            }
            const auto nameRva64 = originalThunk->u1.AddressOfData +
                                   sizeof(WORD);
            if (nameRva64 > (std::numeric_limits<std::uint32_t>::max)()) {
                return failure(IatPatchStatus::InvalidImage,
                               "polling import name RVA is invalid");
            }
            std::string_view functionName;
            if (!boundedAsciiString(
                    base, imageBytes, static_cast<std::uint32_t>(nameRva64),
                    functionName)) {
                return failure(IatPatchStatus::InvalidImage,
                               "polling import name is not bounded");
            }
            const auto function = pollingFunction(functionName);
            if (!function) continue;
            const auto bit = pollingImportBit(*function);
            if ((requiredMask & bit) == 0) continue;
            if ((discoveredMask & bit) != 0) {
                auto report = failure(
                    IatPatchStatus::DuplicateImport,
                    "polling import occurs more than once");
                report.discoveredMask = discoveredMask;
                return report;
            }
            auto* slot = reinterpret_cast<std::uintptr_t*>(
                const_cast<std::byte*>(base) +
                static_cast<std::uint32_t>(firstRva));
            const auto index = static_cast<std::size_t>(*function);
            if (replacements[index] == 0 || *slot == 0) {
                return failure(IatPatchStatus::InvalidImage,
                               "polling import pointer is invalid");
            }
            slots.push_back(PollingIatSlot{
                *function, slot, *slot, replacements[index]});
            discoveredMask |= bit;
        }
        if (!thunkTerminated) {
            return failure(IatPatchStatus::InvalidImage,
                           "polling import thunk table is unterminated");
        }
    }
    if (!terminated) {
        return failure(IatPatchStatus::InvalidImage,
                       "import descriptor table is unterminated");
    }
    if (discoveredMask != requiredMask) {
        auto report = failure(IatPatchStatus::MissingImport,
                              "one or more required polling imports are missing");
        report.discoveredMask = discoveredMask;
        return report;
    }
    IatPatchReport report;
    report.status = IatPatchStatus::Success;
    report.discoveredMask = discoveredMask;
    return report;
#else
    (void)replacements;
    (void)requiredMask;
    return failure(IatPatchStatus::UnsupportedPlatform,
                   "Win32 IAT discovery is Windows-only");
#endif
}

IatPatchReport discoverCurrentProcessCursorFocusImports(
    const std::array<std::uintptr_t, kCursorFocusImportCount>& replacements,
    std::vector<CursorFocusIatSlot>& slots) {
    return discoverCurrentProcessCursorFocusImports(
        replacements, kCursorFocusImportMask, slots);
}

IatPatchReport discoverCurrentProcessCursorFocusImports(
    const std::array<std::uintptr_t, kCursorFocusImportCount>& replacements,
    std::uint32_t requiredMask,
    std::vector<CursorFocusIatSlot>& slots) {
    slots.clear();
    if (requiredMask == 0 || (requiredMask & ~kCursorFocusImportMask) != 0) {
        return failure(IatPatchStatus::InvalidImage,
                       "cursor/focus required mask is invalid");
    }
#ifdef _WIN32
    const HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable module is unavailable",
                       GetLastError());
    }
    const auto* base = reinterpret_cast<const std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<std::size_t>(dos->e_lfanew) > 4096u) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable DOS header is invalid");
    }
#if defined(_WIN64)
    using NtHeaders = IMAGE_NT_HEADERS64;
    using ThunkData = IMAGE_THUNK_DATA64;
    constexpr WORD kOptionalMagic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#else
    using NtHeaders = IMAGE_NT_HEADERS32;
    using ThunkData = IMAGE_THUNK_DATA32;
    constexpr WORD kOptionalMagic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#endif
    const auto* nt = reinterpret_cast<const NtHeaders*>(
        base + static_cast<std::size_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != kOptionalMagic ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(nt->OptionalHeader)) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable NT header is invalid");
    }
    const std::size_t imageBytes = nt->OptionalHeader.SizeOfImage;
    if (imageBytes < sizeof(IMAGE_DOS_HEADER) ||
        nt->OptionalHeader.NumberOfRvaAndSizes <=
            IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable image size is invalid");
    }
    const auto directory = nt->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0 ||
        !imageRange(directory.VirtualAddress, directory.Size, imageBytes)) {
        return failure(IatPatchStatus::MissingImport,
                       "current executable has no bounded import directory");
    }
    const auto* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    const std::size_t descriptorLimit =
        directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    bool terminated = false;
    std::uint32_t discoveredMask = 0;
    for (std::size_t descriptorIndex = 0;
         descriptorIndex < descriptorLimit; ++descriptorIndex) {
        const auto& descriptor = descriptors[descriptorIndex];
        if (descriptor.Name == 0 && descriptor.FirstThunk == 0 &&
            descriptor.OriginalFirstThunk == 0) {
            terminated = true;
            break;
        }
        std::string_view moduleName;
        if (!boundedAsciiString(base, imageBytes, descriptor.Name,
                                moduleName)) {
            return failure(IatPatchStatus::InvalidImage,
                           "import module name is not bounded");
        }
        if (!allowedPollingModule(moduleName)) continue;
        if (descriptor.OriginalFirstThunk == 0 ||
            descriptor.FirstThunk == 0 ||
            !imageRange(descriptor.OriginalFirstThunk, sizeof(ThunkData),
                        imageBytes) ||
            !imageRange(descriptor.FirstThunk, sizeof(ThunkData),
                        imageBytes)) {
            return failure(IatPatchStatus::InvalidImage,
                           "cursor/focus import thunk metadata is invalid");
        }
        const std::size_t thunkLimit = imageBytes / sizeof(ThunkData);
        bool thunkTerminated = false;
        for (std::size_t thunkIndex = 0; thunkIndex < thunkLimit;
             ++thunkIndex) {
            const auto originalRva = static_cast<std::uint64_t>(
                descriptor.OriginalFirstThunk) +
                thunkIndex * sizeof(ThunkData);
            const auto firstRva = static_cast<std::uint64_t>(
                descriptor.FirstThunk) + thunkIndex * sizeof(ThunkData);
            if (originalRva > (std::numeric_limits<std::uint32_t>::max)() ||
                firstRva > (std::numeric_limits<std::uint32_t>::max)() ||
                !imageRange(static_cast<std::uint32_t>(originalRva),
                            sizeof(ThunkData), imageBytes) ||
                !imageRange(static_cast<std::uint32_t>(firstRva),
                            sizeof(ThunkData), imageBytes)) {
                return failure(IatPatchStatus::InvalidImage,
                               "cursor/focus import thunk is not bounded");
            }
            const auto* originalThunk = reinterpret_cast<const ThunkData*>(
                base + static_cast<std::uint32_t>(originalRva));
            if (originalThunk->u1.AddressOfData == 0) {
                thunkTerminated = true;
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal)) continue;
            const auto nameRva64 = originalThunk->u1.AddressOfData +
                                   sizeof(WORD);
            if (nameRva64 > (std::numeric_limits<std::uint32_t>::max)()) {
                return failure(IatPatchStatus::InvalidImage,
                               "cursor/focus import name RVA is invalid");
            }
            std::string_view functionName;
            if (!boundedAsciiString(
                    base, imageBytes, static_cast<std::uint32_t>(nameRva64),
                    functionName)) {
                return failure(IatPatchStatus::InvalidImage,
                               "cursor/focus import name is not bounded");
            }
            const auto function = cursorFocusFunction(functionName);
            if (!function) continue;
            const auto bit = cursorFocusImportBit(*function);
            if ((requiredMask & bit) == 0) continue;
            if ((discoveredMask & bit) != 0) {
                auto report = failure(
                    IatPatchStatus::DuplicateImport,
                    "cursor/focus import occurs more than once");
                report.discoveredMask = discoveredMask;
                return report;
            }
            auto* slot = reinterpret_cast<std::uintptr_t*>(
                const_cast<std::byte*>(base) +
                static_cast<std::uint32_t>(firstRva));
            const auto index = static_cast<std::size_t>(*function);
            if (replacements[index] == 0 || *slot == 0) {
                return failure(IatPatchStatus::InvalidImage,
                               "cursor/focus import pointer is invalid");
            }
            slots.push_back(CursorFocusIatSlot{
                *function, slot, *slot, replacements[index]});
            discoveredMask |= bit;
        }
        if (!thunkTerminated) {
            return failure(IatPatchStatus::InvalidImage,
                           "cursor/focus import thunk table is unterminated");
        }
    }
    if (!terminated) {
        return failure(IatPatchStatus::InvalidImage,
                       "import descriptor table is unterminated");
    }
    if (discoveredMask != requiredMask) {
        auto report = failure(IatPatchStatus::MissingImport,
                              "one or more required cursor/focus imports are missing");
        report.discoveredMask = discoveredMask;
        return report;
    }
    IatPatchReport report;
    report.status = IatPatchStatus::Success;
    report.discoveredMask = discoveredMask;
    return report;
#else
    (void)replacements;
    (void)requiredMask;
    return failure(IatPatchStatus::UnsupportedPlatform,
                   "Win32 IAT discovery is Windows-only");
#endif
}

IatPatchReport discoverCurrentProcessRawInputImports(
    const std::array<std::uintptr_t, kRawInputImportCount>& replacements,
    std::vector<RawInputIatSlot>& slots) {
    return discoverCurrentProcessRawInputImports(
        replacements, kRawInputImportMask, slots);
}

IatPatchReport discoverCurrentProcessRawInputImports(
    const std::array<std::uintptr_t, kRawInputImportCount>& replacements,
    std::uint32_t requiredMask,
    std::vector<RawInputIatSlot>& slots) {
    slots.clear();
    if (requiredMask == 0 || (requiredMask & ~kRawInputImportMask) != 0) {
        return failure(IatPatchStatus::InvalidImage,
                       "Raw Input required mask is invalid");
    }
#ifdef _WIN32
    const HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable module is unavailable",
                       GetLastError());
    }
    const auto* base = reinterpret_cast<const std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<std::size_t>(dos->e_lfanew) > 4096u) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable DOS header is invalid");
    }
#if defined(_WIN64)
    using NtHeaders = IMAGE_NT_HEADERS64;
    using ThunkData = IMAGE_THUNK_DATA64;
    constexpr WORD kOptionalMagic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#else
    using NtHeaders = IMAGE_NT_HEADERS32;
    using ThunkData = IMAGE_THUNK_DATA32;
    constexpr WORD kOptionalMagic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#endif
    const auto* nt = reinterpret_cast<const NtHeaders*>(
        base + static_cast<std::size_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != kOptionalMagic ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(nt->OptionalHeader)) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable NT header is invalid");
    }
    const std::size_t imageBytes = nt->OptionalHeader.SizeOfImage;
    if (imageBytes < sizeof(IMAGE_DOS_HEADER) ||
        nt->OptionalHeader.NumberOfRvaAndSizes <=
            IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return failure(IatPatchStatus::InvalidImage,
                       "current executable image size is invalid");
    }
    const auto directory = nt->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0 ||
        !imageRange(directory.VirtualAddress, directory.Size, imageBytes)) {
        return failure(IatPatchStatus::MissingImport,
                       "current executable has no bounded import directory");
    }
    const auto* descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    const std::size_t descriptorLimit =
        directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    bool terminated = false;
    std::uint32_t discoveredMask = 0;
    for (std::size_t descriptorIndex = 0;
         descriptorIndex < descriptorLimit; ++descriptorIndex) {
        const auto& descriptor = descriptors[descriptorIndex];
        if (descriptor.Name == 0 && descriptor.FirstThunk == 0 &&
            descriptor.OriginalFirstThunk == 0) {
            terminated = true;
            break;
        }
        std::string_view moduleName;
        if (!boundedAsciiString(base, imageBytes, descriptor.Name,
                                moduleName)) {
            return failure(IatPatchStatus::InvalidImage,
                           "import module name is not bounded");
        }
        if (!allowedPollingModule(moduleName)) continue;
        if (descriptor.OriginalFirstThunk == 0 ||
            descriptor.FirstThunk == 0 ||
            !imageRange(descriptor.OriginalFirstThunk, sizeof(ThunkData),
                        imageBytes) ||
            !imageRange(descriptor.FirstThunk, sizeof(ThunkData),
                        imageBytes)) {
            return failure(IatPatchStatus::InvalidImage,
                           "Raw Input import thunk metadata is invalid");
        }
        const std::size_t thunkLimit = imageBytes / sizeof(ThunkData);
        bool thunkTerminated = false;
        for (std::size_t thunkIndex = 0; thunkIndex < thunkLimit;
             ++thunkIndex) {
            const auto originalRva = static_cast<std::uint64_t>(
                descriptor.OriginalFirstThunk) +
                thunkIndex * sizeof(ThunkData);
            const auto firstRva = static_cast<std::uint64_t>(
                descriptor.FirstThunk) + thunkIndex * sizeof(ThunkData);
            if (originalRva > (std::numeric_limits<std::uint32_t>::max)() ||
                firstRva > (std::numeric_limits<std::uint32_t>::max)() ||
                !imageRange(static_cast<std::uint32_t>(originalRva),
                            sizeof(ThunkData), imageBytes) ||
                !imageRange(static_cast<std::uint32_t>(firstRva),
                            sizeof(ThunkData), imageBytes)) {
                return failure(IatPatchStatus::InvalidImage,
                               "Raw Input import thunk is not bounded");
            }
            const auto* originalThunk = reinterpret_cast<const ThunkData*>(
                base + static_cast<std::uint32_t>(originalRva));
            if (originalThunk->u1.AddressOfData == 0) {
                thunkTerminated = true;
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal)) continue;
            const auto nameRva64 = originalThunk->u1.AddressOfData +
                                   sizeof(WORD);
            if (nameRva64 > (std::numeric_limits<std::uint32_t>::max)()) {
                return failure(IatPatchStatus::InvalidImage,
                               "Raw Input import name RVA is invalid");
            }
            std::string_view functionName;
            if (!boundedAsciiString(
                    base, imageBytes, static_cast<std::uint32_t>(nameRva64),
                    functionName)) {
                return failure(IatPatchStatus::InvalidImage,
                               "Raw Input import name is not bounded");
            }
            const auto function = rawInputFunction(functionName);
            if (!function) continue;
            const auto bit = rawInputImportBit(*function);
            if ((requiredMask & bit) == 0) continue;
            if ((discoveredMask & bit) != 0) {
                auto report = failure(
                    IatPatchStatus::DuplicateImport,
                    "Raw Input import occurs more than once");
                report.discoveredMask = discoveredMask;
                return report;
            }
            auto* slot = reinterpret_cast<std::uintptr_t*>(
                const_cast<std::byte*>(base) +
                static_cast<std::uint32_t>(firstRva));
            const auto index = static_cast<std::size_t>(*function);
            if (replacements[index] == 0 || *slot == 0) {
                return failure(IatPatchStatus::InvalidImage,
                               "Raw Input import pointer is invalid");
            }
            slots.push_back(RawInputIatSlot{
                *function, slot, *slot, replacements[index]});
            discoveredMask |= bit;
        }
        if (!thunkTerminated) {
            return failure(IatPatchStatus::InvalidImage,
                           "Raw Input import thunk table is unterminated");
        }
    }
    if (!terminated) {
        return failure(IatPatchStatus::InvalidImage,
                       "import descriptor table is unterminated");
    }
    if (discoveredMask != requiredMask) {
        auto report = failure(IatPatchStatus::MissingImport,
                              "one or more required Raw Input imports are missing");
        report.discoveredMask = discoveredMask;
        return report;
    }
    IatPatchReport report;
    report.status = IatPatchStatus::Success;
    report.discoveredMask = discoveredMask;
    return report;
#else
    (void)replacements;
    (void)requiredMask;
    return failure(IatPatchStatus::UnsupportedPlatform,
                   "Win32 IAT discovery is Windows-only");
#endif
}

} // namespace hydra::gatec
