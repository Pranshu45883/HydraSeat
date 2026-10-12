#include "hydra/xinput_iat_redirect.hpp"

#include "hydra/win32_iat_patch.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace hydra::controller {
namespace {

constexpr std::uint8_t kGetStateIndex = 0u;
constexpr std::uint8_t kSetStateIndex = 1u;
constexpr std::uint8_t kGetCapabilitiesIndex = 2u;
constexpr std::uint32_t kGetStateBit = 1u << kGetStateIndex;
constexpr std::uint32_t kSetStateBit = 1u << kSetStateIndex;
constexpr std::uint32_t kGetCapabilitiesBit = 1u << kGetCapabilitiesIndex;

XInputRedirectReport failure(
    XInputRedirectStatus status,
    std::string error,
    std::uint32_t systemError = 0) {
    XInputRedirectReport report;
    report.status = status;
    report.error = std::move(error);
    report.systemError = systemError;
    return report;
}

#if defined(_WIN32)

bool imageRange(
    std::uint32_t rva,
    std::size_t bytes,
    std::size_t imageBytes) noexcept {
    const auto start = static_cast<std::size_t>(rva);
    return start <= imageBytes && bytes <= imageBytes - start;
}

bool boundedAsciiString(
    const std::byte* base,
    std::size_t imageBytes,
    std::uint32_t rva,
    std::string_view& value) noexcept {
    if (!imageRange(rva, 1u, imageBytes)) return false;
    const char* text = reinterpret_cast<const char*>(base + rva);
    const std::size_t maximum =
        imageBytes - static_cast<std::size_t>(rva);
    const void* terminator = std::memchr(text, '\0', maximum);
    if (terminator == nullptr) return false;
    value = std::string_view(
        text,
        static_cast<const char*>(terminator) - text);
    return true;
}

char asciiLower(char value) noexcept {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

bool asciiEqual(
    std::string_view left,
    std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(
               left.begin(),
               left.end(),
               right.begin(),
               [](char a, char b) {
                   return asciiLower(a) == asciiLower(b);
               });
}

bool allowedXInputModule(std::string_view module) noexcept {
    constexpr std::array<std::string_view, 5> allowed{
        "xinput1_4.dll",
        "xinput1_3.dll",
        "xinput1_2.dll",
        "xinput1_1.dll",
        "xinput9_1_0.dll",
    };
    return std::any_of(
        allowed.begin(),
        allowed.end(),
        [&](std::string_view candidate) {
            return asciiEqual(module, candidate);
        });
}

std::optional<std::uint8_t> supportedFunction(
    std::string_view name) noexcept {
    if (name == "XInputGetState") return kGetStateIndex;
    if (name == "XInputSetState") return kSetStateIndex;
    if (name == "XInputGetCapabilities") {
        return kGetCapabilitiesIndex;
    }
    return std::nullopt;
}

std::uint32_t functionBit(std::uint8_t index) noexcept {
    return 1u << static_cast<std::uint32_t>(index);
}

std::uintptr_t replacementFor(
    const XInputReplacementSet& replacements,
    std::uint8_t index) noexcept {
    switch (index) {
    case kGetStateIndex: return replacements.getState;
    case kSetStateIndex: return replacements.setState;
    case kGetCapabilitiesIndex: return replacements.getCapabilities;
    default: return 0;
    }
}

#endif

} // namespace

XInputIatRedirect::~XInputIatRedirect() {
    if (installed_) {
        (void)uninstall();
    }
}

XInputRedirectReport XInputIatRedirect::install(
    const XInputReplacementSet& replacements) noexcept {
    if (installed_) {
        XInputRedirectReport report;
        report.status = XInputRedirectStatus::AlreadyInstalled;
        report.patchedSlotCount = slots_.size();
        for (const auto& slot : slots_) {
            report.discoveredFunctionMask |=
                functionBit(slot.functionIndex);
        }
        return report;
    }

    if (!replacements.valid()) {
        return failure(
            XInputRedirectStatus::InvalidImage,
            "XInput replacement export set is incomplete");
    }

#if defined(_WIN32)
    try {
        const HMODULE module = GetModuleHandleW(nullptr);
        if (module == nullptr) {
            return failure(
                XInputRedirectStatus::InvalidImage,
                "target executable module is unavailable",
                GetLastError());
        }

        const auto* base =
            reinterpret_cast<const std::byte*>(module);
        const auto* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew < 0 ||
            static_cast<std::size_t>(dos->e_lfanew) > 4096u) {
            return failure(
                XInputRedirectStatus::InvalidImage,
                "target executable DOS header is invalid");
        }

#if defined(_WIN64)
        using NtHeaders = IMAGE_NT_HEADERS64;
        using ThunkData = IMAGE_THUNK_DATA64;
        constexpr WORD kOptionalMagic =
            IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#else
        using NtHeaders = IMAGE_NT_HEADERS32;
        using ThunkData = IMAGE_THUNK_DATA32;
        constexpr WORD kOptionalMagic =
            IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#endif

        const auto* nt = reinterpret_cast<const NtHeaders*>(
            base + static_cast<std::size_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != kOptionalMagic ||
            nt->FileHeader.SizeOfOptionalHeader <
                sizeof(nt->OptionalHeader)) {
            return failure(
                XInputRedirectStatus::InvalidImage,
                "target executable NT header is invalid");
        }

        const std::size_t imageBytes =
            nt->OptionalHeader.SizeOfImage;
        if (imageBytes < sizeof(IMAGE_DOS_HEADER) ||
            nt->OptionalHeader.NumberOfRvaAndSizes <=
                IMAGE_DIRECTORY_ENTRY_IMPORT) {
            return failure(
                XInputRedirectStatus::InvalidImage,
                "target executable image size is invalid");
        }

        const auto directory =
            nt->OptionalHeader.DataDirectory[
                IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress == 0 ||
            directory.Size == 0 ||
            !imageRange(
                directory.VirtualAddress,
                directory.Size,
                imageBytes)) {
            return failure(
                XInputRedirectStatus::MissingImport,
                "target executable has no bounded static import directory");
        }

        const auto* descriptors =
            reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                base + directory.VirtualAddress);
        const std::size_t descriptorLimit =
            directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);

        std::vector<Slot> discovered;
        bool descriptorTerminated = false;
        bool sawXInputModule = false;
        std::uint32_t discoveredMask = 0u;

        for (std::size_t descriptorIndex = 0;
             descriptorIndex < descriptorLimit;
             ++descriptorIndex) {
            const auto& descriptor =
                descriptors[descriptorIndex];
            if (descriptor.Name == 0 &&
                descriptor.FirstThunk == 0 &&
                descriptor.OriginalFirstThunk == 0) {
                descriptorTerminated = true;
                break;
            }

            std::string_view moduleName;
            if (!boundedAsciiString(
                    base,
                    imageBytes,
                    descriptor.Name,
                    moduleName)) {
                return failure(
                    XInputRedirectStatus::InvalidImage,
                    "target import module name is not bounded");
            }
            if (!allowedXInputModule(moduleName)) continue;
            sawXInputModule = true;

            if (descriptor.OriginalFirstThunk == 0 ||
                descriptor.FirstThunk == 0 ||
                !imageRange(
                    descriptor.OriginalFirstThunk,
                    sizeof(ThunkData),
                    imageBytes) ||
                !imageRange(
                    descriptor.FirstThunk,
                    sizeof(ThunkData),
                    imageBytes)) {
                return failure(
                    XInputRedirectStatus::InvalidImage,
                    "XInput import thunk metadata is invalid");
            }

            const std::size_t thunkLimit =
                imageBytes / sizeof(ThunkData);
            bool thunkTerminated = false;
            for (std::size_t thunkIndex = 0;
                 thunkIndex < thunkLimit;
                 ++thunkIndex) {
                const auto originalRva =
                    static_cast<std::uint64_t>(
                        descriptor.OriginalFirstThunk) +
                    thunkIndex * sizeof(ThunkData);
                const auto firstRva =
                    static_cast<std::uint64_t>(
                        descriptor.FirstThunk) +
                    thunkIndex * sizeof(ThunkData);
                if (originalRva >
                        (std::numeric_limits<std::uint32_t>::max)() ||
                    firstRva >
                        (std::numeric_limits<std::uint32_t>::max)() ||
                    !imageRange(
                        static_cast<std::uint32_t>(originalRva),
                        sizeof(ThunkData),
                        imageBytes) ||
                    !imageRange(
                        static_cast<std::uint32_t>(firstRva),
                        sizeof(ThunkData),
                        imageBytes)) {
                    return failure(
                        XInputRedirectStatus::InvalidImage,
                        "XInput import thunk is not bounded");
                }

                const auto* originalThunk =
                    reinterpret_cast<const ThunkData*>(
                        base +
                        static_cast<std::uint32_t>(
                            originalRva));
                if (originalThunk->u1.AddressOfData == 0) {
                    thunkTerminated = true;
                    break;
                }
                if (IMAGE_SNAP_BY_ORDINAL(
                        originalThunk->u1.Ordinal)) {
                    return failure(
                        XInputRedirectStatus::UnsupportedImport,
                        "ordinal XInput imports are not supported by the v1 redirect");
                }

                const auto nameRva64 =
                    originalThunk->u1.AddressOfData +
                    sizeof(WORD);
                if (nameRva64 >
                    (std::numeric_limits<std::uint32_t>::max)()) {
                    return failure(
                        XInputRedirectStatus::InvalidImage,
                        "XInput import name RVA is invalid");
                }

                std::string_view functionName;
                if (!boundedAsciiString(
                        base,
                        imageBytes,
                        static_cast<std::uint32_t>(
                            nameRva64),
                        functionName)) {
                    return failure(
                        XInputRedirectStatus::InvalidImage,
                        "XInput import name is not bounded");
                }

                const auto function =
                    supportedFunction(functionName);
                if (!function) {
                    return failure(
                        XInputRedirectStatus::UnsupportedImport,
                        "target imports an XInput API outside the reviewed v1 redirect set: " +
                            std::string(functionName));
                }

                auto* slot =
                    reinterpret_cast<std::uintptr_t*>(
                        const_cast<std::byte*>(base) +
                        static_cast<std::uint32_t>(
                            firstRva));
                const auto replacement =
                    replacementFor(
                        replacements,
                        *function);
                if (*slot == 0 || replacement == 0) {
                    return failure(
                        XInputRedirectStatus::InvalidImage,
                        "XInput import pointer is invalid");
                }
                if (*slot == replacement) {
                    return failure(
                        XInputRedirectStatus::PatchFailure,
                        "XInput import is already redirected by an unknown owner");
                }

                discovered.push_back(
                    Slot{
                        slot,
                        *slot,
                        replacement,
                        *function});
                discoveredMask |=
                    functionBit(*function);
            }

            if (!thunkTerminated) {
                return failure(
                    XInputRedirectStatus::InvalidImage,
                    "XInput import thunk table is unterminated");
            }
        }

        if (!descriptorTerminated) {
            return failure(
                XInputRedirectStatus::InvalidImage,
                "target import descriptor table is unterminated");
        }
        if (!sawXInputModule || discovered.empty()) {
            return failure(
                XInputRedirectStatus::MissingImport,
                "target has no supported statically imported XInput API");
        }
        if ((discoveredMask & kGetStateBit) == 0u) {
            return failure(
                XInputRedirectStatus::UnsupportedImport,
                "target XInput contract does not statically import XInputGetState");
        }

        std::vector<Slot> applied;
        applied.reserve(discovered.size());
        XInputRedirectReport report;
        report.status = XInputRedirectStatus::Success;
        report.discoveredFunctionMask = discoveredMask;

        for (const auto& slot : discovered) {
            if (*slot.address != slot.original) {
                report.status =
                    XInputRedirectStatus::PatchFailure;
                report.error =
                    "XInput import changed after discovery";
                break;
            }

            const auto write =
                gatec::writeProcessIatSlot(
                    slot.address,
                    slot.original,
                    slot.replacement,
                    nullptr);
            if (!write.success) {
                report.status =
                    XInputRedirectStatus::PatchFailure;
                report.systemError = write.systemError;
                report.rollbackComplete =
                    write.valueRestored &&
                    write.protectionRestored;
                report.error =
                    "failed to redirect an XInput import";
                if (!write.valueRestored) {
                    applied.push_back(slot);
                }
                break;
            }

            applied.push_back(slot);
            ++report.patchedSlotCount;
        }

        if (report.status !=
            XInputRedirectStatus::Success) {
            for (auto it = applied.rbegin();
                 it != applied.rend();
                 ++it) {
                if (*it->address == it->original) {
                    continue;
                }
                const auto restored =
                    gatec::writeProcessIatSlot(
                        it->address,
                        it->replacement,
                        it->original,
                        nullptr);
                if (!restored.success) {
                    report.rollbackComplete = false;
                    if (report.systemError == 0) {
                        report.systemError =
                            restored.systemError;
                    }
                }
            }
            if (!report.rollbackComplete) {
                slots_ = std::move(applied);
                installed_ = !slots_.empty();
                report.status =
                    XInputRedirectStatus::RollbackFailure;
            }
            report.patchedSlotCount =
                installed_ ? slots_.size() : 0u;
            return report;
        }

        slots_ = std::move(discovered);
        installed_ = true;
        report.patchedSlotCount = slots_.size();
        return report;
    } catch (...) {
        return failure(
            XInputRedirectStatus::InvalidImage,
            "XInput redirect allocation failed");
    }
#else
    (void)replacements;
    return failure(
        XInputRedirectStatus::UnsupportedPlatform,
        "XInput IAT redirect is Windows-only");
#endif
}

XInputRedirectReport XInputIatRedirect::uninstall() noexcept {
    XInputRedirectReport report;
    report.status = XInputRedirectStatus::Success;
    report.rollbackComplete = true;

    for (const auto& slot : slots_) {
        report.discoveredFunctionMask |=
            functionBit(slot.functionIndex);
    }

    if (!installed_ || slots_.empty()) {
        slots_.clear();
        installed_ = false;
        return report;
    }

#if defined(_WIN32)
    try {
        std::vector<Slot> remaining;
        remaining.reserve(slots_.size());

        for (auto it = slots_.rbegin();
             it != slots_.rend();
             ++it) {
            if (*it->address == it->original) {
                continue;
            }
            if (*it->address != it->replacement) {
                remaining.push_back(*it);
                report.rollbackComplete = false;
                report.error =
                    "XInput import changed while HydraSeat redirect was active";
                continue;
            }

            const auto restored =
                gatec::writeProcessIatSlot(
                    it->address,
                    it->replacement,
                    it->original,
                    nullptr);
            if (!restored.success) {
                remaining.push_back(*it);
                report.rollbackComplete = false;
                if (report.systemError == 0) {
                    report.systemError =
                        restored.systemError;
                }
            }
        }

        if (!remaining.empty()) {
            std::reverse(
                remaining.begin(),
                remaining.end());
            slots_ = std::move(remaining);
            installed_ = true;
            report.status =
                XInputRedirectStatus::RollbackFailure;
            report.patchedSlotCount = slots_.size();
            if (report.error.empty()) {
                report.error =
                    "one or more XInput imports could not be restored";
            }
            return report;
        }

        slots_.clear();
        installed_ = false;
        return report;
    } catch (...) {
        report.status =
            XInputRedirectStatus::RollbackFailure;
        report.rollbackComplete = false;
        report.error =
            "XInput redirect rollback allocation failed";
        report.patchedSlotCount = slots_.size();
        return report;
    }
#else
    report.status =
        XInputRedirectStatus::UnsupportedPlatform;
    report.rollbackComplete = false;
    report.error =
        "XInput IAT redirect is Windows-only";
    return report;
#endif
}

} // namespace hydra::controller
