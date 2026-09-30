#include "hydra/gate_c_architecture.hpp"
#include "hydra/gate_c_external_profile.hpp"
#include "hydra/gate_c_protocol.hpp"
#include "hydra/gate_c_transport.hpp"
#include "hydra/virtual_input_state.hpp"

#ifdef _WIN32

#include <windows.h>

#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t kHandshakeTimeoutMs = 10000;
constexpr std::uint32_t kIoTimeoutMs = 5000;
constexpr std::uint32_t kWindowTimeoutMs = 10000;
constexpr std::uint32_t kExitTimeoutMs = 5000;

struct Options {
    std::filesystem::path target;
    std::filesystem::path bridge;
    std::filesystem::path outputDirectory;
    bool showHelp{false};
};

struct WindowSearch {
    DWORD processId{0};
    HWND found{nullptr};
};

BOOL CALLBACK findWindowCallback(HWND window, LPARAM opaque) {
    auto& search = *reinterpret_cast<WindowSearch*>(opaque);
    DWORD processId = 0;
    if (GetWindowThreadProcessId(window, &processId) == 0 ||
        processId != search.processId || IsWindowVisible(window) == FALSE ||
        GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    search.found = window;
    return FALSE;
}

HWND findVisibleWindow(DWORD processId, std::uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        WindowSearch search{processId, nullptr};
        (void)EnumWindows(findWindowCallback,
                          reinterpret_cast<LPARAM>(&search));
        if (search.found != nullptr) return search.found;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return nullptr;
}

std::wstring quoted(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

std::optional<Options> parseOptions(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view arg{argv[index]};
        const auto requireValue = [&](std::filesystem::path& out) -> bool {
            if (index + 1 >= argc) return false;
            out = argv[++index];
            return true;
        };
        if (arg == L"--target") {
            if (!requireValue(options.target)) return std::nullopt;
        } else if (arg == L"--bridge") {
            if (!requireValue(options.bridge)) return std::nullopt;
        } else if (arg == L"--output-dir") {
            if (!requireValue(options.outputDirectory)) return std::nullopt;
        } else if (arg == L"--help" || arg == L"-h") {
            options.showHelp = true;
        } else {
            return std::nullopt;
        }
    }
    if (options.showHelp) return options;
    if (options.target.empty() || options.bridge.empty() ||
        options.outputDirectory.empty()) {
        return std::nullopt;
    }
    return options;
}

void printUsage() {
    std::cout
        << "HydraSeat P3-E-01 external open-source application harness\n\n"
        << "Usage:\n"
        << "  hydra_gate_c_external_harness --target <glfw cursor.exe>\n"
        << "      --bridge <hydra_gate_c_external_bridge.dll>\n"
        << "      --output-dir <evidence-dir>\n\n"
        << "The harness only loads the fixed HydraSeat bridge into processes it\n"
        << "creates suspended itself. It has no attach-to-existing-process mode.\n";
}

std::uint64_t processCreationTime100ns(HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user) == FALSE) {
        return 0;
    }
    ULARGE_INTEGER value{};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    return value.QuadPart;
}

struct OwnedTarget {
    std::uint32_t seatId{0};
    PROCESS_INFORMATION process{};
    HANDLE mapping{nullptr};
    HANDLE containmentJob{nullptr};
    hydra::gatec::PipeChannel channel;
    hydra::gatec::SessionToken token{};
    std::uint64_t sequence{1};
    std::uint64_t creationTime100ns{0};
    std::filesystem::path stdoutPath;
    std::filesystem::path stderrPath;
    HWND window{nullptr};

    OwnedTarget() = default;
    OwnedTarget(const OwnedTarget&) = delete;
    OwnedTarget& operator=(const OwnedTarget&) = delete;
    OwnedTarget(OwnedTarget&&) = delete;
    OwnedTarget& operator=(OwnedTarget&&) = delete;

    ~OwnedTarget() {
        channel.close();
        if (mapping != nullptr) CloseHandle(mapping);
        if (containmentJob != nullptr) {
            CloseHandle(containmentJob);
            containmentJob = nullptr;
        }
        // This object exclusively owns the process handle returned by the
        // CREATE_SUSPENDED launch. Closing a successfully assigned kill-on-close
        // job normally tears a failed target down; this exact-handle fallback also
        // covers failures that occur before job assignment completes.
        if (process.hProcess != nullptr &&
            WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process.hProcess, 0x50334546u); // "P3EF"
            (void)WaitForSingleObject(process.hProcess, 2000);
        }
        if (process.hThread != nullptr) CloseHandle(process.hThread);
        if (process.hProcess != nullptr) CloseHandle(process.hProcess);
    }
};

bool createOutputHandle(const std::filesystem::path& path, HANDLE& handle) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                         &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle != INVALID_HANDLE_VALUE;
}

bool createInputHandle(HANDLE& handle) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    handle = CreateFileW(L"NUL", GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle != INVALID_HANDLE_VALUE;
}

bool containOwnedTarget(OwnedTarget& target) {
    target.containmentJob = CreateJobObjectW(nullptr, nullptr);
    if (target.containmentJob == nullptr) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(target.containmentJob,
                                JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits)) == FALSE) {
        return false;
    }
    return AssignProcessToJobObject(target.containmentJob,
                                    target.process.hProcess) != FALSE;
}

bool createBridgeMapping(OwnedTarget& target,
                         const std::wstring& pipeName) {
    const auto mappingName = hydra::gatec::externalBridgeMappingName(
        target.process.dwProcessId);
    target.mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(hydra::gatec::ExternalBridgeConfigV1)),
        mappingName.c_str());
    if (target.mapping == nullptr) return false;
    void* view = MapViewOfFile(target.mapping, FILE_MAP_WRITE, 0, 0,
                               sizeof(hydra::gatec::ExternalBridgeConfigV1));
    if (view == nullptr) return false;
    hydra::gatec::ExternalBridgeConfigV1 config{};
    config.seatId = target.seatId;
    config.requiredApiMask = hydra::gatec::kP3EGlfwRequiredApiMask;
    config.token = target.token;
    if (pipeName.size() >= hydra::gatec::kExternalBridgePipeNameChars) {
        UnmapViewOfFile(view);
        return false;
    }
    std::copy(pipeName.begin(), pipeName.end(), config.pipeName);
    config.pipeName[pipeName.size()] = L'\0';
    std::memcpy(view, &config, sizeof(config));
    FlushViewOfFile(view, sizeof(config));
    UnmapViewOfFile(view);
    return true;
}

bool injectOwnedLibrary(const OwnedTarget& target,
                        const std::filesystem::path& libraryPath) {
    const std::wstring library = libraryPath.wstring();
    const SIZE_T bytes = (library.size() + 1u) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(target.process.hProcess, nullptr, bytes,
                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (remote == nullptr) return false;
    SIZE_T written = 0;
    const bool wrote = WriteProcessMemory(target.process.hProcess, remote,
                                          library.c_str(), bytes, &written) !=
                       FALSE && written == bytes;
    if (!wrote) {
        VirtualFreeEx(target.process.hProcess, remote, 0, MEM_RELEASE);
        return false;
    }
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        kernel32 != nullptr ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr);
    if (loadLibrary == nullptr) {
        VirtualFreeEx(target.process.hProcess, remote, 0, MEM_RELEASE);
        return false;
    }
    HANDLE thread = CreateRemoteThread(target.process.hProcess, nullptr, 0,
                                       loadLibrary, remote, 0, nullptr);
    if (thread == nullptr) {
        VirtualFreeEx(target.process.hProcess, remote, 0, MEM_RELEASE);
        return false;
    }
    const DWORD wait = WaitForSingleObject(thread, kHandshakeTimeoutMs);
    DWORD result = 0;
    const bool loaded = wait == WAIT_OBJECT_0 &&
                        GetExitCodeThread(thread, &result) != FALSE &&
                        result != 0;
    CloseHandle(thread);
    VirtualFreeEx(target.process.hProcess, remote, 0, MEM_RELEASE);
    return loaded;
}

bool handshake(OwnedTarget& target, hydra::gatec::PipeChannel server) {
    std::string error;
    if (!hydra::gatec::waitForGateCClient(server, kHandshakeTimeoutMs,
                                          &error)) {
        return false;
    }
    const auto frame = server.readFrame(kHandshakeTimeoutMs);
    hydra::gatec::HelloMessage hello{};
    if (!frame || !frame.frame || frame.frame->sequence != 1 ||
        !hydra::gatec::decodeHello(*frame.frame, hello, &error) ||
        hello.token != target.token || hello.seatId != target.seatId ||
        hello.processId != target.process.dwProcessId ||
        hello.architectureBits != 64 || hello.targetWindow == 0) {
        return false;
    }
    DWORD owner = 0;
    if (GetWindowThreadProcessId(
            reinterpret_cast<HWND>(hello.targetWindow), &owner) == 0 ||
        owner != target.process.dwProcessId) {
        return false;
    }
    hydra::gatec::HelloAckMessage ack{};
    ack.accepted = true;
    ack.serverProcessId = GetCurrentProcessId();
    ack.grantedCapabilities = hydra::gatec::testCapabilityBits(
        hydra::gatec::kControlledTargetCapabilities |
        hydra::gatec::TestCapability::PollingApiShim |
        hydra::gatec::TestCapability::CursorFocusApiShim |
        hydra::gatec::TestCapability::RawInputApiShim);
    if (!server.writeFrame(hydra::gatec::encodeHelloAck(1, ack),
                           kIoTimeoutMs, &error)) {
        return false;
    }
    target.channel = std::move(server);
    return true;
}

bool launchTarget(const Options& options, std::uint32_t seatId,
                  OwnedTarget& target) {
    target.seatId = seatId;
    const auto token = hydra::gatec::generateSessionToken();
    if (!token) return false;
    target.token = *token;
    const auto pipeName = hydra::gatec::makeGateCPipeName(
        GetCurrentProcessId(), seatId, target.token);
    std::string error;
    auto server = hydra::gatec::createGateCServerPipe(pipeName, &error);
    if (!server.valid()) return false;

    target.stdoutPath = options.outputDirectory /
        ("seat-" + std::to_string(seatId) + ".stdout.txt");
    target.stderrPath = options.outputDirectory /
        ("seat-" + std::to_string(seatId) + ".stderr.txt");
    HANDLE stdoutHandle = INVALID_HANDLE_VALUE;
    HANDLE stderrHandle = INVALID_HANDLE_VALUE;
    HANDLE stdinHandle = INVALID_HANDLE_VALUE;
    if (!createOutputHandle(target.stdoutPath, stdoutHandle) ||
        !createOutputHandle(target.stderrPath, stderrHandle) ||
        !createInputHandle(stdinHandle)) {
        if (stdoutHandle != INVALID_HANDLE_VALUE) CloseHandle(stdoutHandle);
        if (stderrHandle != INVALID_HANDLE_VALUE) CloseHandle(stderrHandle);
        if (stdinHandle != INVALID_HANDLE_VALUE) CloseHandle(stdinHandle);
        return false;
    }

    SIZE_T attributeBytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    if (attributeBytes == 0) {
        CloseHandle(stdoutHandle);
        CloseHandle(stderrHandle);
        CloseHandle(stdinHandle);
        return false;
    }
    std::vector<unsigned char> attributeStorage(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attributeStorage.data());
    if (InitializeProcThreadAttributeList(attributeList, 1, 0,
                                          &attributeBytes) == FALSE) {
        CloseHandle(stdoutHandle);
        CloseHandle(stderrHandle);
        CloseHandle(stdinHandle);
        return false;
    }
    HANDLE inheritedHandles[] = {stdinHandle, stdoutHandle, stderrHandle};
    if (UpdateProcThreadAttribute(
            attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr) ==
        FALSE) {
        DeleteProcThreadAttributeList(attributeList);
        CloseHandle(stdoutHandle);
        CloseHandle(stderrHandle);
        CloseHandle(stdinHandle);
        return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = stdoutHandle;
    startup.StartupInfo.hStdError = stderrHandle;
    startup.StartupInfo.hStdInput = stdinHandle;
    startup.lpAttributeList = attributeList;
    std::wstring commandLine = quoted(options.target);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    const auto workingDirectory = options.target.parent_path().wstring();
    const BOOL created = CreateProcessW(
        options.target.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
        workingDirectory.c_str(), &startup.StartupInfo, &target.process);
    DeleteProcThreadAttributeList(attributeList);
    CloseHandle(stdoutHandle);
    CloseHandle(stderrHandle);
    CloseHandle(stdinHandle);
    if (created == FALSE) return false;
    target.creationTime100ns = processCreationTime100ns(target.process.hProcess);
    if (target.creationTime100ns == 0 || !containOwnedTarget(target)) return false;

    const auto runtimeArchitecture =
        hydra::gatec::detectProcessArchitecture(target.process.hProcess);
    if (!runtimeArchitecture || runtimeArchitecture.architecture !=
                                    hydra::gatec::ProcessArchitecture::X64) {
        return false;
    }
    const auto artifactDirectory = options.bridge.parent_path();
    const auto adapterPath = artifactDirectory / L"hydra_gate_c_adapter.dll";
    const auto shimPath = artifactDirectory / L"hydra_gate_c_shim.dll";
    if (!createBridgeMapping(target, pipeName) ||
        !injectOwnedLibrary(target, adapterPath) ||
        !injectOwnedLibrary(target, shimPath) ||
        !injectOwnedLibrary(target, options.bridge) ||
        !handshake(target, std::move(server))) {
        return false;
    }
    if (ResumeThread(target.process.hThread) == static_cast<DWORD>(-1)) {
        return false;
    }
    target.window = findVisibleWindow(target.process.dwProcessId,
                                      kWindowTimeoutMs);
    return target.window != nullptr;
}

bool postKey(HWND window, UINT virtualKey) {
    const UINT scan = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    const LPARAM down = static_cast<LPARAM>(scan << 16u);
    const LPARAM up = down | (static_cast<LPARAM>(1) << 30u) |
                      (static_cast<LPARAM>(1) << 31u);
    return PostMessageW(window, WM_KEYDOWN, virtualKey, down) != FALSE &&
           PostMessageW(window, WM_KEYUP, virtualKey, up) != FALSE;
}

bool writeControl(OwnedTarget& target, std::int32_t x, std::int32_t y) {
    hydra::gatec::ControlStateMessage control{};
    control.cursorX = x;
    control.cursorY = y;
    control.virtualForeground = true;
    control.virtualCapture = true;
    std::string error;
    return target.channel.writeFrame(
        hydra::gatec::encodeControlState(++target.sequence, control),
        kIoTimeoutMs, &error);
}

bool writeKeyTransition(OwnedTarget& target, std::uint32_t vkey,
                        hydra::gatec::KeyTransition transition) {
    hydra::gatec::InputEventMessage input{};
    input.kind = hydra::gatec::InputKind::Keyboard;
    input.keyTransition = transition;
    input.timestampMicros = target.sequence * 1000u;
    input.vkey = vkey;
    std::string error;
    return target.channel.writeFrame(
        hydra::gatec::encodeInputEvent(++target.sequence, input),
        kIoTimeoutMs, &error);
}

bool writeMouse(OwnedTarget& target, std::int32_t dx, std::int32_t dy) {
    hydra::gatec::InputEventMessage input{};
    input.kind = hydra::gatec::InputKind::Mouse;
    input.timestampMicros = target.sequence * 1000u;
    input.deltaX = dx;
    input.deltaY = dy;
    std::string error;
    return target.channel.writeFrame(
        hydra::gatec::encodeInputEvent(++target.sequence, input),
        kIoTimeoutMs, &error);
}

std::optional<hydra::gatec::StateSnapshotMessage> querySnapshot(
    OwnedTarget& target, std::uint16_t probeVkey) {
    hydra::gatec::QuerySnapshotMessage query{};
    query.probeVkey = probeVkey;
    const std::uint64_t sequence = ++target.sequence;
    std::string error;
    if (!target.channel.writeFrame(
            hydra::gatec::encodeQuerySnapshot(sequence, query),
            kIoTimeoutMs, &error)) {
        return std::nullopt;
    }
    const auto frame = target.channel.readFrame(kIoTimeoutMs);
    hydra::gatec::StateSnapshotMessage snapshot{};
    if (!frame || !frame.frame || frame.frame->sequence != sequence ||
        !hydra::gatec::decodeStateSnapshot(*frame.frame, snapshot, &error)) {
        return std::nullopt;
    }
    return snapshot;
}

bool shutdownTarget(OwnedTarget& target) {
    std::string error;
    if (target.channel.valid()) {
        (void)target.channel.writeFrame(
            hydra::gatec::encodeShutdown(++target.sequence),
            kIoTimeoutMs, &error);
    }
    const DWORD wait = WaitForSingleObject(target.process.hProcess,
                                           kExitTimeoutMs);
    target.channel.close();
    return wait == WAIT_OBJECT_0;
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::size_t countOccurrences(std::string_view text, std::string_view pattern) {
    if (pattern.empty()) return 0;
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(pattern, offset)) != std::string_view::npos) {
        ++count;
        offset += pattern.size();
    }
    return count;
}

bool forcedGuardCleanup(const Options& options, std::uint32_t& processId,
                        std::uint64_t& creationTime100ns) {
    OwnedTarget faultTarget;
    if (!launchTarget(options, 3, faultTarget) ||
        faultTarget.containmentJob == nullptr) {
        return false;
    }
    processId = faultTarget.process.dwProcessId;
    creationTime100ns = faultTarget.creationTime100ns;
    HANDLE job = faultTarget.containmentJob;
    faultTarget.containmentJob = nullptr;
    const bool closed = CloseHandle(job) != FALSE;
    const DWORD wait = WaitForSingleObject(faultTarget.process.hProcess,
                                           kExitTimeoutMs);
    faultTarget.channel.close();
    return closed && wait == WAIT_OBJECT_0;
}

bool nativeRelaunch(const Options& options) {
    PROCESS_INFORMATION process{};
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    std::wstring commandLine = quoted(options.target);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    if (CreateProcessW(options.target.c_str(), mutableCommand.data(), nullptr,
                       nullptr, FALSE, 0, nullptr,
                       options.target.parent_path().c_str(), &startup,
                       &process) == FALSE) {
        return false;
    }
    HWND window = findVisibleWindow(process.dwProcessId, kWindowTimeoutMs);
    bool result = window != nullptr;
    if (window != nullptr) (void)PostMessageW(window, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process.hProcess, kExitTimeoutMs) != WAIT_OBJECT_0) {
        result = false;
        TerminateProcess(process.hProcess, 91);
        WaitForSingleObject(process.hProcess, 1000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

bool writeReport(const Options& options, const OwnedTarget& seat1,
                 const OwnedTarget& seat2, std::size_t seat1ExpectedCount,
                 std::size_t seat2ExpectedCount, std::size_t seat1CrossCount,
                 std::size_t seat2CrossCount, bool stateSeparated,
                 bool forcedGuardPass, std::uint32_t forcedGuardProcessId,
                 std::uint64_t forcedGuardCreationTime100ns, bool nativePass) {
    const bool seat1Expected = seat1ExpectedCount == 4;
    const bool seat2Expected = seat2ExpectedCount == 4;
    const bool seat1Cross = seat1CrossCount != 0;
    const bool seat2Cross = seat2CrossCount != 0;
    std::ofstream report(options.outputDirectory / "p3-e-01-report.json",
                         std::ios::binary | std::ios::trunc);
    if (!report) return false;
    report << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"profile_id\": \"" << hydra::gatec::kP3EGlfwProfileId
           << "\",\n"
           << "  \"glfw_version\": \"" << hydra::gatec::kP3EGlfwVersion
           << "\",\n"
           << "  \"glfw_commit\": \"" << hydra::gatec::kP3EGlfwCommit
           << "\",\n"
           << "  \"required_api_mask\": \"0x0000b93a\",\n"
           << "  \"kernel_job_kill_on_close\": true,\n"
           << "  \"seat1_pid\": " << seat1.process.dwProcessId << ",\n"
           << "  \"seat1_creation_time_100ns\": "
           << seat1.creationTime100ns << ",\n"
           << "  \"seat2_pid\": " << seat2.process.dwProcessId << ",\n"
           << "  \"seat2_creation_time_100ns\": "
           << seat2.creationTime100ns << ",\n"
           << "  \"declared_events_per_seat\": 4,\n"
           << "  \"seat1_expected_event_count\": " << seat1ExpectedCount << ",\n"
           << "  \"seat2_expected_event_count\": " << seat2ExpectedCount << ",\n"
           << "  \"seat1_cross_event_count\": " << seat1CrossCount << ",\n"
           << "  \"seat2_cross_event_count\": " << seat2CrossCount << ",\n"
           << "  \"receiver_verified_events\": "
           << (seat1ExpectedCount + seat2ExpectedCount) << ",\n"
           << "  \"seat1_expected_pattern_seen\": "
           << (seat1Expected ? "true" : "false") << ",\n"
           << "  \"seat2_expected_pattern_seen\": "
           << (seat2Expected ? "true" : "false") << ",\n"
           << "  \"seat1_cross_pattern_seen\": "
           << (seat1Cross ? "true" : "false") << ",\n"
           << "  \"seat2_cross_pattern_seen\": "
           << (seat2Cross ? "true" : "false") << ",\n"
           << "  \"adapter_state_separated\": "
           << (stateSeparated ? "true" : "false") << ",\n"
           << "  \"forced_guard_cleanup_pass\": "
           << (forcedGuardPass ? "true" : "false") << ",\n"
           << "  \"forced_guard_pid\": " << forcedGuardProcessId << ",\n"
           << "  \"forced_guard_creation_time_100ns\": "
           << forcedGuardCreationTime100ns << ",\n"
           << "  \"native_relaunch_pass\": "
           << (nativePass ? "true" : "false") << ",\n"
           << "  \"pass\": "
           << ((seat1Expected && seat2Expected && !seat1Cross && !seat2Cross &&
                stateSeparated && forcedGuardPass && nativePass) ? "true" : "false") << "\n"
           << "}\n";
    return static_cast<bool>(report);
}

int run(const Options& options) {
    std::error_code ec;
    std::filesystem::create_directories(options.outputDirectory, ec);
    const auto artifactDirectory = options.bridge.parent_path();
    const auto adapterPath = artifactDirectory / L"hydra_gate_c_adapter.dll";
    const auto shimPath = artifactDirectory / L"hydra_gate_c_shim.dll";
    if (ec || !std::filesystem::is_regular_file(options.target) ||
        options.bridge.filename() != L"hydra_gate_c_external_bridge.dll" ||
        !std::filesystem::is_regular_file(options.bridge) ||
        !std::filesystem::is_regular_file(adapterPath) ||
        !std::filesystem::is_regular_file(shimPath)) {
        return 20;
    }
    const auto targetArchitecture =
        hydra::gatec::detectPortableExecutableArchitecture(options.target);
    const auto bridgeArchitecture =
        hydra::gatec::detectPortableExecutableArchitecture(options.bridge);
    const auto adapterArchitecture =
        hydra::gatec::detectPortableExecutableArchitecture(adapterPath);
    const auto shimArchitecture =
        hydra::gatec::detectPortableExecutableArchitecture(shimPath);
    if (!targetArchitecture || !bridgeArchitecture || !adapterArchitecture ||
        !shimArchitecture ||
        targetArchitecture.architecture != hydra::gatec::ProcessArchitecture::X64 ||
        bridgeArchitecture.architecture != targetArchitecture.architecture ||
        adapterArchitecture.architecture != targetArchitecture.architecture ||
        shimArchitecture.architecture != targetArchitecture.architecture) {
        return 21;
    }

    OwnedTarget seat1;
    OwnedTarget seat2;
    if (!launchTarget(options, 1, seat1)) return 22;
    if (!launchTarget(options, 2, seat2)) {
        (void)shutdownTarget(seat1);
        return 23;
    }

    // Upstream GLFW cursor test: D enters disabled mode and R enables raw mouse.
    if (!postKey(seat1.window, 'D') || !postKey(seat2.window, 'D')) return 24;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (!postKey(seat1.window, 'R') || !postKey(seat2.window, 'R')) return 25;
    // A real desktop has only one OS foreground window. P3-E establishes the
    // already-declared Seat-local logical focus for each controlled process by
    // delivering the same process-local focus lifecycle GLFW consumes. No
    // global foreground mutation is performed.
    (void)PostMessageW(seat1.window, WM_ACTIVATE, WA_ACTIVE, 0);
    (void)PostMessageW(seat1.window, WM_SETFOCUS, 0, 0);
    (void)PostMessageW(seat2.window, WM_ACTIVATE, WA_ACTIVE, 0);
    (void)PostMessageW(seat2.window, WM_SETFOCUS, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    if (!writeControl(seat1, 100, 100) || !writeControl(seat2, 500, 500) ||
        !writeKeyTransition(seat1, 'A', hydra::gatec::KeyTransition::Down) ||
        !writeKeyTransition(seat2, 'B', hydra::gatec::KeyTransition::Down)) {
        return 26;
    }
    const auto seat1A = querySnapshot(seat1, 'A');
    const auto seat1B = querySnapshot(seat1, 'B');
    const auto seat2A = querySnapshot(seat2, 'A');
    const auto seat2B = querySnapshot(seat2, 'B');
    const bool stateSeparated = seat1A && seat1B && seat2A && seat2B &&
        hydra::gatec::snapshotKeyDown(*seat1A, 'A') &&
        !hydra::gatec::snapshotKeyDown(*seat1B, 'B') &&
        !hydra::gatec::snapshotKeyDown(*seat2A, 'A') &&
        hydra::gatec::snapshotKeyDown(*seat2B, 'B');
    if (!writeKeyTransition(seat1, 'A', hydra::gatec::KeyTransition::Up) ||
        !writeKeyTransition(seat2, 'B', hydra::gatec::KeyTransition::Up)) {
        return 26;
    }

    // GLFW's disabled/raw cursor transition may consume the first relative sample
    // while establishing its internal virtual cursor baseline. Keep that expected
    // implementation detail outside the measured window: send one distinct warm-up
    // sample per Seat, then require all four declared samples below to arrive.
    if (!writeMouse(seat1, 1, 1) || !writeMouse(seat2, -1, 1)) {
        return 27;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (int index = 0; index < 4; ++index) {
        if (!writeMouse(seat1, 11, 3) || !writeMouse(seat2, -7, 5)) {
            return 27;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(125));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const bool clean1 = shutdownTarget(seat1);
    const bool clean2 = shutdownTarget(seat2);
    if (!clean1 || !clean2) return 28;

    const std::string output1 = readText(seat1.stdoutPath);
    const std::string output2 = readText(seat2.stdoutPath);
    const std::string pattern1 = "+11.000000 +3.000000";
    const std::string pattern2 = "-7.000000 +5.000000";
    const std::size_t seat1ExpectedCount = countOccurrences(output1, pattern1);
    const std::size_t seat2ExpectedCount = countOccurrences(output2, pattern2);
    const std::size_t seat1CrossCount = countOccurrences(output1, pattern2);
    const std::size_t seat2CrossCount = countOccurrences(output2, pattern1);
    std::uint32_t forcedGuardProcessId = 0;
    std::uint64_t forcedGuardCreationTime100ns = 0;
    const bool forcedGuardPass = forcedGuardCleanup(
        options, forcedGuardProcessId, forcedGuardCreationTime100ns);
    const bool nativePass = nativeRelaunch(options);
    if (!writeReport(options, seat1, seat2, seat1ExpectedCount,
                     seat2ExpectedCount, seat1CrossCount, seat2CrossCount,
                     stateSeparated, forcedGuardPass, forcedGuardProcessId,
                     forcedGuardCreationTime100ns, nativePass)) {
        return 29;
    }
    const bool passed = seat1ExpectedCount == 4 && seat2ExpectedCount == 4 &&
        seat1CrossCount == 0 && seat2CrossCount == 0 && stateSeparated &&
        forcedGuardPass && nativePass;
    std::cout << "P3E_RESULT seat1_expected=" << seat1ExpectedCount
              << " seat2_expected=" << seat2ExpectedCount
              << " seat1_cross=" << seat1CrossCount
              << " seat2_cross=" << seat2CrossCount
              << " adapter_state_separated=" << (stateSeparated ? 1 : 0)
              << " forced_guard_cleanup=" << (forcedGuardPass ? 1 : 0)
              << " native_relaunch=" << (nativePass ? 1 : 0)
              << " pass=" << (passed ? 1 : 0) << '\n';
    return passed ? EXIT_SUCCESS : 30;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const auto options = parseOptions(argc, argv);
    if (!options) {
        printUsage();
        return 2;
    }
    if (options->showHelp) {
        printUsage();
        return 0;
    }
    return run(*options);
}

#else

#include <iostream>
int main() {
    std::cerr << "P3-E-01 external application harness is Windows-only.\n";
    return 2;
}

#endif
