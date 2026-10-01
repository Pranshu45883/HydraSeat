#include "hydra/gate_c_adapter.h"
#include "hydra/gate_c_architecture.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using hydra::gatec::ArchitectureDetectionStatus;
using hydra::gatec::ArtifactSelectionStatus;
using hydra::gatec::GateCArtifactKind;
using hydra::gatec::ProcessArchitecture;

static_assert(sizeof(HydraGateCAdapterInputEventV1) ==
              HYDRA_GATE_C_ADAPTER_INPUT_EVENT_V1_BYTES);
static_assert(sizeof(HydraGateCAdapterControlStateV1) ==
              HYDRA_GATE_C_ADAPTER_CONTROL_STATE_V1_BYTES);
static_assert(sizeof(HydraGateCAdapterSnapshotV1) ==
              HYDRA_GATE_C_ADAPTER_SNAPSHOT_V1_BYTES);
static_assert(sizeof(HydraGateCAdapterClipRectV2) ==
              HYDRA_GATE_C_ADAPTER_CLIP_RECT_V2_BYTES);
static_assert(sizeof(HydraGateCAdapterWindowStateV2) ==
              HYDRA_GATE_C_ADAPTER_WINDOW_STATE_V2_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputSourceV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputMappingV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_MAPPING_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputSourceStateV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_STATE_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputStateV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_STATE_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputSourceCapabilitiesV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_CAPABILITIES_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputCapabilitiesV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_CAPABILITIES_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputSourceBatteryV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_SOURCE_BATTERY_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputBatteryV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_BATTERY_V4_BYTES);
static_assert(sizeof(HydraGateCAdapterXInputVibrationV4) ==
              HYDRA_GATE_C_ADAPTER_XINPUT_VIBRATION_V4_BYTES);

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<std::byte> peHeader(ProcessArchitecture architecture) {
    std::vector<std::byte> bytes(128, std::byte{0});
    bytes[0] = static_cast<std::byte>('M');
    bytes[1] = static_cast<std::byte>('Z');
    bytes[0x3c] = std::byte{0x40};
    bytes[0x40] = static_cast<std::byte>('P');
    bytes[0x41] = static_cast<std::byte>('E');
    const std::uint16_t machine = architecture == ProcessArchitecture::X86
                                      ? 0x014cu
                                      : 0x8664u;
    bytes[0x44] = static_cast<std::byte>(machine & 0xffu);
    bytes[0x45] = static_cast<std::byte>((machine >> 8) & 0xffu);
    return bytes;
}

void writeBytes(const std::filesystem::path& path,
                const std::vector<std::byte>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(output), "synthetic PE fixture is written");
}

void testProcessArchitectureResolution() {
    using hydra::gatec::ProcessArchitectureObservation;
    using hydra::gatec::resolveProcessArchitecture;

    ProcessArchitectureObservation modernWow64;
    modernWow64.modernApiAvailable = true;
    modernWow64.modernApiSucceeded = true;
    modernWow64.processMachine = 0x014c;
    modernWow64.nativeMachine = 0x8664;
    auto result = resolveProcessArchitecture(modernWow64);
    check(result && result.architecture == ProcessArchitecture::X86 &&
              !result.usedLegacyFallback,
          "IsWow64Process2 maps a WOW64 process to x86");

    ProcessArchitectureObservation modernNative = modernWow64;
    modernNative.processMachine = 0;
    result = resolveProcessArchitecture(modernNative);
    check(result && result.architecture == ProcessArchitecture::X64,
          "IsWow64Process2 maps a native AMD64 process to x64");

    ProcessArchitectureObservation fallbackWow64;
    fallbackWow64.legacyApiSucceeded = true;
    fallbackWow64.legacyWow64 = true;
    fallbackWow64.nativeProcessorArchitecture = 9;
    result = resolveProcessArchitecture(fallbackWow64);
    check(result && result.architecture == ProcessArchitecture::X86 &&
              result.usedLegacyFallback,
          "bounded IsWow64Process fallback maps WOW64 to x86");

    ProcessArchitectureObservation fallbackNative;
    fallbackNative.legacyApiSucceeded = true;
    fallbackNative.nativeProcessorArchitecture = 9;
    result = resolveProcessArchitecture(fallbackNative);
    check(result && result.architecture == ProcessArchitecture::X64 &&
              result.usedLegacyFallback,
          "bounded fallback maps native AMD64 to x64");

    ProcessArchitectureObservation modernFailure;
    modernFailure.modernApiAvailable = true;
    modernFailure.systemError = 5;
    result = resolveProcessArchitecture(modernFailure);
    check(!result && result.status == ArchitectureDetectionStatus::SystemError &&
              !result.usedLegacyFallback,
          "a failing available modern API does not silently downgrade");

    ProcessArchitectureObservation unsupported;
    unsupported.modernApiAvailable = true;
    unsupported.modernApiSucceeded = true;
    unsupported.nativeMachine = 0xaa64;
    result = resolveProcessArchitecture(unsupported);
    check(!result && result.status == ArchitectureDetectionStatus::Unsupported,
          "unknown modern machine types are unsupported");

    ProcessArchitectureObservation failedFallback;
    failedFallback.systemError = 87;
    result = resolveProcessArchitecture(failedFallback);
    check(!result && result.status == ArchitectureDetectionStatus::SystemError &&
              result.usedLegacyFallback,
          "failed legacy detection is a visible system error");
}

void testPortableExecutableDetection() {
    auto x86 = hydra::gatec::detectPortableExecutableArchitecture(
        peHeader(ProcessArchitecture::X86));
    check(x86 && x86.architecture == ProcessArchitecture::X86,
          "PE I386 machine is detected as x86");
    auto x64 = hydra::gatec::detectPortableExecutableArchitecture(
        peHeader(ProcessArchitecture::X64));
    check(x64 && x64.architecture == ProcessArchitecture::X64,
          "PE AMD64 machine is detected as x64");

    auto malformed = peHeader(ProcessArchitecture::X64);
    malformed[0] = std::byte{0};
    check(hydra::gatec::detectPortableExecutableArchitecture(malformed).status ==
              ArchitectureDetectionStatus::MalformedImage,
          "malformed DOS headers are rejected");
    malformed = peHeader(ProcessArchitecture::X64);
    malformed[0x3c] = std::byte{0xff};
    malformed[0x3d] = std::byte{0xff};
    check(hydra::gatec::detectPortableExecutableArchitecture(malformed).status ==
              ArchitectureDetectionStatus::MalformedImage,
          "out-of-bounds PE offsets are rejected");
    auto unknown = peHeader(ProcessArchitecture::X64);
    unknown[0x44] = std::byte{0x64};
    unknown[0x45] = std::byte{0xaa};
    check(hydra::gatec::detectPortableExecutableArchitecture(unknown).status ==
              ArchitectureDetectionStatus::Unsupported,
          "unknown PE machine types are rejected");
}

void testManifestValidationAndSelection() {
    using hydra::gatec::defaultGateCArtifactManifest;
    using hydra::gatec::selectGateCArtifacts;
    auto manifest = defaultGateCArtifactManifest();
    auto selected = selectGateCArtifacts(
        manifest, ProcessArchitecture::X86,
        GateCArtifactKind::ControlledTarget);
    check(selected &&
              selected.selection->executablePath.generic_string() ==
                  "x86/hydra_gate_c_target.exe" &&
              selected.selection->adapterPath.generic_string() ==
                  "x86/hydra_gate_c_adapter.dll",
          "canonical x86 target and adapter selection is deterministic");

    selected = selectGateCArtifacts(
        manifest, ProcessArchitecture::X64, GateCArtifactKind::ApiProbe);
    check(selected &&
              selected.selection->executablePath.generic_string() ==
                  "x64/hydra_gate_c_api_probe.exe" &&
              selected.selection->shimPath.generic_string() ==
                  "x64/hydra_gate_c_shim.dll",
          "canonical x64 API probe and polling shim selection is deterministic");

    auto future = manifest;
    future.schemaVersion = 2;
    check(selectGateCArtifacts(future, ProcessArchitecture::X86,
                               GateCArtifactKind::ControlledTarget).status ==
              ArtifactSelectionStatus::InvalidManifest,
          "future manifest versions are rejected");

    auto duplicate = manifest;
    duplicate.entries.insert(duplicate.entries.begin() + 1,
                             duplicate.entries.front());
    check(selectGateCArtifacts(duplicate, ProcessArchitecture::X86,
                               GateCArtifactKind::ControlledTarget).status ==
              ArtifactSelectionStatus::InvalidManifest,
          "duplicate manifest entries are rejected");

    auto overLimit = manifest;
    while (overLimit.entries.size() <=
           hydra::gatec::kMaximumGateCArtifactEntries) {
        overLimit.entries.push_back(overLimit.entries.back());
    }
    check(selectGateCArtifacts(overLimit, ProcessArchitecture::X86,
                               GateCArtifactKind::ControlledTarget).status ==
              ArtifactSelectionStatus::InvalidManifest,
          "over-limit manifest entry counts are rejected");

    auto unknownArchitecture = manifest;
    unknownArchitecture.entries[0].architecture =
        ProcessArchitecture::Unknown;
    check(selectGateCArtifacts(unknownArchitecture, ProcessArchitecture::X86,
                               GateCArtifactKind::ControlledTarget).status ==
              ArtifactSelectionStatus::InvalidManifest,
          "unknown manifest architectures are rejected");

    auto unknownKind = manifest;
    unknownKind.entries[0].kind = static_cast<GateCArtifactKind>(99);
    check(selectGateCArtifacts(unknownKind, ProcessArchitecture::X86,
                               GateCArtifactKind::ControlledTarget).status ==
              ArtifactSelectionStatus::InvalidManifest,
          "unknown artifact kinds are rejected");

    auto traversal = manifest;
    traversal.entries[0].relativePath = "x86/../escape.exe";
    check(selectGateCArtifacts(traversal, ProcessArchitecture::X86,
                               GateCArtifactKind::ControlledTarget).status ==
              ArtifactSelectionStatus::InvalidManifest,
          "artifact path traversal is rejected");

    auto absolute = manifest;
    absolute.entries[0].relativePath = "C:/escape.exe";
    check(selectGateCArtifacts(absolute, ProcessArchitecture::X86,
                               GateCArtifactKind::ControlledTarget).status ==
              ArtifactSelectionStatus::InvalidManifest,
          "absolute artifact paths are rejected");

    auto oversizedPath = manifest;
    oversizedPath.entries[0].relativePath =
        std::string(hydra::gatec::kMaximumGateCArtifactPathBytes + 1, 'a');
    check(selectGateCArtifacts(oversizedPath, ProcessArchitecture::X86,
                               GateCArtifactKind::ControlledTarget).status ==
              ArtifactSelectionStatus::InvalidManifest,
          "oversized artifact paths are rejected");

    auto nonCanonical = manifest;
    std::swap(nonCanonical.entries[0], nonCanonical.entries[1]);
    check(selectGateCArtifacts(nonCanonical, ProcessArchitecture::X86,
                               GateCArtifactKind::ControlledTarget).status ==
              ArtifactSelectionStatus::InvalidManifest,
          "non-canonical manifest ordering is rejected");

    auto x64Only = manifest;
    x64Only.entries.erase(x64Only.entries.begin(),
                          x64Only.entries.begin() + 4);
    check(selectGateCArtifacts(x64Only, ProcessArchitecture::X86,
                               GateCArtifactKind::ControlledTarget).status ==
              ArtifactSelectionStatus::Unsupported,
          "an absent architecture reports Unsupported");

    auto missingAdapter = manifest;
    missingAdapter.entries.erase(missingAdapter.entries.begin() + 1);
    check(selectGateCArtifacts(missingAdapter, ProcessArchitecture::X86,
                               GateCArtifactKind::ControlledTarget).status ==
              ArtifactSelectionStatus::MissingArtifact,
          "an incomplete architecture reports a missing artifact");

    auto missingShim = manifest;
    missingShim.entries.erase(missingShim.entries.begin() + 3);
    check(selectGateCArtifacts(missingShim, ProcessArchitecture::X86,
                               GateCArtifactKind::ApiProbe).status ==
              ArtifactSelectionStatus::MissingArtifact,
          "an API probe manifest without its polling shim is rejected");
}

void testArtifactPreflight() {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
        ("hydra_gate_c_architecture_" + unique);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    auto manifest = hydra::gatec::defaultGateCArtifactManifest();
    auto missing = hydra::gatec::resolveGateCArtifacts(
        root, manifest, ProcessArchitecture::X86,
        GateCArtifactKind::ControlledTarget);
    check(missing.status == ArtifactSelectionStatus::MissingArtifact,
          "missing selected artifacts are rejected before launch");

    const auto target = root / "x86/hydra_gate_c_target.exe";
    const auto adapter = root / "x86/hydra_gate_c_adapter.dll";
    writeBytes(target, peHeader(ProcessArchitecture::X86));
    writeBytes(adapter, peHeader(ProcessArchitecture::X86));
    auto selected = hydra::gatec::resolveGateCArtifacts(
        root, manifest, ProcessArchitecture::X86,
        GateCArtifactKind::ControlledTarget);
    check(selected && selected.selection->architecture ==
                          ProcessArchitecture::X86 &&
              selected.selection->executablePath.is_absolute() &&
              selected.selection->adapterPath.is_absolute(),
          "matching PE artifacts resolve to bounded absolute paths");

    writeBytes(adapter, peHeader(ProcessArchitecture::X64));
    auto wrongArchitecture = hydra::gatec::resolveGateCArtifacts(
        root, manifest, ProcessArchitecture::X86,
        GateCArtifactKind::ControlledTarget);
    check(wrongArchitecture.status ==
              ArtifactSelectionStatus::ArchitectureMismatch,
          "a target/adapter architecture mismatch is rejected");

    writeBytes(adapter, peHeader(ProcessArchitecture::X86));
    writeBytes(target, std::vector<std::byte>(8, std::byte{0}));
    auto malformed = hydra::gatec::resolveGateCArtifacts(
        root, manifest, ProcessArchitecture::X86,
        GateCArtifactKind::ControlledTarget);
    check(malformed.status == ArtifactSelectionStatus::MalformedImage,
          "malformed selected binaries are rejected before launch");

    writeBytes(root / "x86/hydra_gate_c_api_probe.exe",
               peHeader(ProcessArchitecture::X86));
    writeBytes(root / "x86/hydra_gate_c_shim.dll",
               peHeader(ProcessArchitecture::X64));
    auto wrongShim = hydra::gatec::resolveGateCArtifacts(
        root, manifest, ProcessArchitecture::X86,
        GateCArtifactKind::ApiProbe);
    check(wrongShim.status == ArtifactSelectionStatus::ArchitectureMismatch,
          "a wrong-architecture polling shim is rejected before launch");

    std::filesystem::remove_all(root, ignored);
}

#ifdef _WIN32
void runWindowsRuntimeSelfTest(int argc, char** argv) {
    check(argc == 6, "runtime self-test receives expected arguments");
    const auto expected = hydra::gatec::parseProcessArchitecture(argv[2]);
    check(expected.has_value(), "runtime expected architecture parses");
    const auto process = hydra::gatec::detectProcessArchitecture(
        GetCurrentProcess());
    if (!process || process.architecture != *expected) {
        std::cerr << "[DIAG] expected="
                  << hydra::gatec::processArchitectureName(*expected)
                  << " detected="
                  << hydra::gatec::processArchitectureName(process.architecture)
                  << " status="
                  << hydra::gatec::architectureDetectionStatusName(process.status)
                  << " systemError=" << process.systemError
                  << " error='" << process.error << "'\n";
        using IsWow64Process2Function = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
        const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        const auto modern = kernel32 == nullptr
            ? nullptr
            : reinterpret_cast<IsWow64Process2Function>(
                  GetProcAddress(kernel32, "IsWow64Process2"));
        if (modern != nullptr) {
            USHORT processMachine = 0;
            USHORT nativeMachine = 0;
            const BOOL ok = modern(
                GetCurrentProcess(), &processMachine, &nativeMachine);
            std::cerr << "[DIAG] IsWow64Process2 ok=" << (ok != FALSE)
                      << " processMachine=0x" << std::hex << processMachine
                      << " nativeMachine=0x" << nativeMachine << std::dec
                      << " lastError=" << (ok != FALSE ? 0 : GetLastError())
                      << '\n';
        }
        const auto selfImage = hydra::gatec::detectPortableExecutableArchitecture(
            std::filesystem::path(argv[0]));
        std::cerr << "[DIAG] self PE architecture="
                  << hydra::gatec::processArchitectureName(selfImage.architecture)
                  << " status="
                  << hydra::gatec::architectureDetectionStatusName(selfImage.status)
                  << " error='" << selfImage.error << "'\n";
    }
    check(process && process.architecture == *expected,
          "Win32 process detection matches the configured architecture");
    const auto target = hydra::gatec::detectPortableExecutableArchitecture(
        std::filesystem::path(argv[3]));
    const auto adapter = hydra::gatec::detectPortableExecutableArchitecture(
        std::filesystem::path(argv[4]));
    const auto shim = hydra::gatec::detectPortableExecutableArchitecture(
        std::filesystem::path(argv[5]));
    check(target && adapter && shim && target.architecture == *expected &&
              adapter.architecture == *expected &&
              shim.architecture == *expected,
          "built target, adapter, and shim PE machines match the configured architecture");
}
#endif

} // namespace

int main(int argc, char** argv) {
    testProcessArchitectureResolution();
    testPortableExecutableDetection();
    testManifestValidationAndSelection();
    testArtifactPreflight();
#ifdef _WIN32
    if (argc > 1 && std::string_view(argv[1]) == "--runtime-self-test") {
        runWindowsRuntimeSelfTest(argc, argv);
    }
#else
    (void)argc;
    (void)argv;
#endif
    std::cout << "Gate C architecture and artifact-selection tests passed.\n";
    return EXIT_SUCCESS;
}
