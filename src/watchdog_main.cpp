#include "hydra/hidhide_session_recovery.hpp"
#include "hydra/rollback_registry.hpp"
#include "hydra/watchdog_protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace hydra::watchdog;

constexpr std::uint32_t kStartupTimeoutMilliseconds = 5'000;
constexpr std::uint32_t kPollMilliseconds = 10;
constexpr int kExitCleanDisarm = 0;
constexpr int kExitInvalidArguments = 2;
constexpr int kExitStartupFailure = 3;
constexpr int kExitRollbackComplete = 10;
constexpr int kExitRecoveryRequired = 11;

#if defined(_WIN32)

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : m_handle(handle) {}
    ~ScopedHandle() {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const noexcept { return m_handle; }

private:
    HANDLE m_handle;
};

struct Options {
    HANDLE controlHandle{INVALID_HANDLE_VALUE};
    HANDLE statusHandle{INVALID_HANDLE_VALUE};
    ProcessIdentity host{};
    SessionId sessionId{};
    std::filesystem::path recoveryDirectory;
};

bool parseUnsigned(std::wstring_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const auto parsed = std::wcstoull(text.data(), &end, 0);
    if (errno != 0 || end == text.data() ||
        static_cast<std::size_t>(end - text.data()) != text.size()) {
        return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

int hexDigit(wchar_t value) noexcept {
    if (value >= L'0' && value <= L'9') return value - L'0';
    if (value >= L'a' && value <= L'f') return value - L'a' + 10;
    if (value >= L'A' && value <= L'F') return value - L'A' + 10;
    return -1;
}

bool parseSession(std::wstring_view text, SessionId& sessionId) {
    if (text.size() != sessionId.size() * 2) return false;
    for (std::size_t index = 0; index < sessionId.size(); ++index) {
        const int high = hexDigit(text[index * 2]);
        const int low = hexDigit(text[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        sessionId[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return !isZeroSessionId(sessionId);
}

bool parseHandle(std::wstring_view text, HANDLE& handle) {
    std::uint64_t value = 0;
    if (!parseUnsigned(text, value) || value == 0 ||
        value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::uintptr_t>::max())) {
        return false;
    }
    handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

bool parseOptions(int argc, wchar_t** argv, Options& options) {
    if (argc != 11 && argc != 13) return false;
    std::wstring_view controlValue;
    std::wstring_view statusValue;
    std::wstring_view hostPidValue;
    std::wstring_view hostCreatedValue;
    std::wstring_view sessionValue;
    std::wstring_view recoveryDirectoryValue;

    for (int index = 1; index + 1 < argc; index += 2) {
        const std::wstring_view name(argv[index]);
        const std::wstring_view value(argv[index + 1]);
        if (name == L"--control-handle") {
            controlValue = value;
        } else if (name == L"--status-handle") {
            statusValue = value;
        } else if (name == L"--host-pid") {
            hostPidValue = value;
        } else if (name == L"--host-created") {
            hostCreatedValue = value;
        } else if (name == L"--session") {
            sessionValue = value;
        } else if (name == L"--recovery-dir") {
            recoveryDirectoryValue = value;
        } else {
            return false;
        }
    }

    if (controlValue.empty() || statusValue.empty() || hostPidValue.empty() ||
        hostCreatedValue.empty() || sessionValue.empty()) {
        return false;
    }
    if (!parseHandle(controlValue, options.controlHandle) ||
        !parseHandle(statusValue, options.statusHandle) ||
        !parseSession(sessionValue, options.sessionId)) {
        return false;
    }

    std::uint64_t pid = 0;
    if (!parseUnsigned(hostPidValue, pid) || pid == 0 ||
        pid > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    options.host.processId = static_cast<std::uint32_t>(pid);
    if (!parseUnsigned(hostCreatedValue, options.host.creationTime100ns) ||
        options.host.creationTime100ns == 0) {
        return false;
    }
    if (argc == 13) {
        if (recoveryDirectoryValue.empty()) return false;
        options.recoveryDirectory = std::filesystem::path(recoveryDirectoryValue);
    }
    return true;
}

bool writeAll(HANDLE handle, const void* bytes, std::size_t size) {
    const auto* cursor = static_cast<const std::byte*>(bytes);
    std::size_t remaining = size;
    while (remaining != 0) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining,
                                  std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (WriteFile(handle, cursor, chunk, &written, nullptr) == FALSE ||
            written == 0) {
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool writeStatusFrame(HANDLE handle,
                      std::uint64_t sequence,
                      const WatchdogStatus& status) {
    const auto frame = encodeWatchdogStatus(sequence, status);
    if (frame.empty() || frame.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const auto frameBytes = static_cast<std::uint32_t>(frame.size());
    std::array<std::byte, 4> prefix{
        static_cast<std::byte>(frameBytes & 0xffu),
        static_cast<std::byte>((frameBytes >> 8u) & 0xffu),
        static_cast<std::byte>((frameBytes >> 16u) & 0xffu),
        static_cast<std::byte>((frameBytes >> 24u) & 0xffu)};
    return writeAll(handle, prefix.data(), prefix.size()) &&
           writeAll(handle, frame.data(), frame.size());
}

enum class PipeReadState {
    NoFrame,
    Frame,
    Closed,
    Error,
    ProtocolError
};

std::uint32_t decodeLengthPrefix(const std::array<std::byte, 4>& prefix) {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[0])) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(prefix[1])) << 8u) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(prefix[2])) << 16u) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(prefix[3])) << 24u);
}

PipeReadState tryReadFrame(HANDLE handle,
                           std::vector<std::byte>& frame,
                           std::uint32_t& systemError) {
    DWORD available = 0;
    std::array<std::byte, 4> prefix{};
    DWORD peeked = 0;
    if (PeekNamedPipe(handle, prefix.data(), static_cast<DWORD>(prefix.size()),
                      &peeked, &available, nullptr) == FALSE) {
        const DWORD error = GetLastError();
        systemError = error;
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
            return PipeReadState::Closed;
        }
        return PipeReadState::Error;
    }
    if (available == 0) return PipeReadState::NoFrame;
    if (available < prefix.size() || peeked < prefix.size()) {
        return PipeReadState::NoFrame;
    }

    const std::uint32_t frameBytes = decodeLengthPrefix(prefix);
    if (frameBytes < kWatchdogFrameHeaderBytes ||
        frameBytes > kWatchdogMaxFrameBytes) {
        return PipeReadState::ProtocolError;
    }
    const std::uint64_t totalBytes =
        static_cast<std::uint64_t>(prefix.size()) + frameBytes;
    if (available < totalBytes) return PipeReadState::NoFrame;

    std::vector<std::byte> combined(static_cast<std::size_t>(totalBytes));
    DWORD bytesRead = 0;
    if (ReadFile(handle, combined.data(), static_cast<DWORD>(combined.size()),
                 &bytesRead, nullptr) == FALSE ||
        bytesRead != combined.size()) {
        systemError = GetLastError();
        return PipeReadState::Error;
    }
    frame.assign(combined.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                 combined.end());
    return PipeReadState::Frame;
}

void printStatus(const WatchdogStatus& status) {
    std::cout << "watchdog_state=" << watchdogRunStateName(status.state)
              << " trigger=" << watchdogTriggerReasonName(status.reason)
              << " completed=" << status.completedActions
              << " total=" << status.totalActions
              << " failed_action=" << status.failedActionId
              << " system_error=" << status.systemError << '\n';
}

RollbackExecutionSummary executeRegisteredRollback(
    RollbackRegistry& registry,
    const std::filesystem::path& recoveryDirectory) {
    if (recoveryDirectory.empty()) {
        DefaultRollbackExecutor executor;
        return registry.execute(executor);
    }
    auto platform = hydra::makeNativeHidHideSessionPlatform();
    hydra::HidHideSessionRollbackExecutor executor(
        recoveryDirectory, std::move(platform));
    return registry.execute(executor);
}

int executeRollback(RollbackRegistry& registry,
                    HANDLE statusHandle,
                    std::uint64_t statusSequence,
                    WatchdogTriggerReason triggerReason,
                    const std::filesystem::path& recoveryDirectory) {
    const auto summary = executeRegisteredRollback(registry, recoveryDirectory);
    const auto* manifest = registry.manifest();
    if (manifest == nullptr) return kExitStartupFailure;

    WatchdogStatus status;
    status.sessionId = manifest->lease.sessionId;
    status.generation = manifest->lease.generation;
    status.state = summary.recoveryRequired
        ? WatchdogRunState::RecoveryRequired
        : WatchdogRunState::RollbackComplete;
    status.reason = triggerReason;
    status.totalActions = static_cast<std::uint16_t>(manifest->actions.size());
    status.completedActions = static_cast<std::uint16_t>(
        std::count_if(summary.outcomes.begin(), summary.outcomes.end(),
                      [](const RollbackActionOutcome& outcome) {
                          return outcome.result == RollbackActionResult::Success ||
                                 outcome.result ==
                                     RollbackActionResult::AlreadySatisfied;
                      }));
    status.failedActionId = summary.firstFailedActionId;
    status.systemError = summary.firstSystemError;
    (void)writeStatusFrame(statusHandle, statusSequence, status);
    printStatus(status);
    return summary.recoveryRequired ? kExitRecoveryRequired
                                    : kExitRollbackComplete;
}

int runWatchdog(const Options& options) {
    if (GetFileType(options.controlHandle) != FILE_TYPE_PIPE ||
        GetFileType(options.statusHandle) != FILE_TYPE_PIPE) {
        return kExitInvalidArguments;
    }

    ProcessIdentity observedHost;
    std::uint32_t hostIdentityError = 0;
    if (!queryProcessIdentity(options.host.processId, observedHost,
                              &hostIdentityError) ||
        observedHost != options.host) {
        std::cerr << "watchdog host identity mismatch: "
                  << hostIdentityError << '\n';
        return kExitStartupFailure;
    }

    const HANDLE rawHost = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, options.host.processId);
    if (rawHost == nullptr) {
        std::cerr << "watchdog could not open host: " << GetLastError() << '\n';
        return kExitStartupFailure;
    }
    ScopedHandle host(rawHost);

    RollbackRegistry registry;
    std::uint64_t lastSequence = 0;
    auto lastRenewal = std::chrono::steady_clock::now();
    const auto startupDeadline = lastRenewal +
        std::chrono::milliseconds(kStartupTimeoutMilliseconds);

    while (!registry.armed()) {
        if (WaitForSingleObject(host.get(), 0) == WAIT_OBJECT_0) {
            return kExitStartupFailure;
        }
        if (std::chrono::steady_clock::now() >= startupDeadline) {
            return kExitStartupFailure;
        }

        std::vector<std::byte> bytes;
        std::uint32_t systemError = 0;
        const auto readState = tryReadFrame(options.controlHandle, bytes,
                                            systemError);
        if (readState == PipeReadState::NoFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMilliseconds));
            continue;
        }
        if (readState != PipeReadState::Frame) {
            return kExitStartupFailure;
        }
        const auto decoded = decodeWatchdogFrame(bytes);
        if (!decoded || decoded.frame->type != WatchdogMessageType::RegisterPlan) {
            return kExitStartupFailure;
        }
        RollbackPlanManifest manifest;
        std::string error;
        if (!decodeRegisterPlan(*decoded.frame, manifest, &error) ||
            manifest.lease.sessionId != options.sessionId) {
            std::cerr << "watchdog rejected rollback plan: " << error << '\n';
            return kExitStartupFailure;
        }
        const auto invalidProcessTarget = std::find_if(
            manifest.actions.begin(), manifest.actions.end(),
            [&options](const RollbackActionDescriptor& action) {
                return action.kind == RollbackActionKind::TerminateOwnedProcess &&
                       (action.process.processId == options.host.processId ||
                        action.process.processId == GetCurrentProcessId());
            });
        if (invalidProcessTarget != manifest.actions.end()) {
            std::cerr << "watchdog rejected rollback plan: host/self process target\n";
            return kExitStartupFailure;
        }
        if (!registry.registerPlan(manifest, &error)) {
            std::cerr << "watchdog rejected rollback plan: " << error << '\n';
            return kExitStartupFailure;
        }
        lastSequence = decoded.frame->sequence;
        lastRenewal = std::chrono::steady_clock::now();
    }

    const auto* manifest = registry.manifest();
    if (manifest == nullptr) return kExitStartupFailure;

    WatchdogStatus armedStatus;
    armedStatus.sessionId = manifest->lease.sessionId;
    armedStatus.generation = manifest->lease.generation;
    armedStatus.state = WatchdogRunState::Armed;
    armedStatus.reason = WatchdogTriggerReason::None;
    armedStatus.totalActions = static_cast<std::uint16_t>(manifest->actions.size());
    (void)writeStatusFrame(options.statusHandle, lastSequence + 1, armedStatus);

    while (true) {
        const DWORD hostState = WaitForSingleObject(host.get(), 0);
        if (hostState == WAIT_OBJECT_0) {
            return executeRollback(registry, options.statusHandle,
                                   lastSequence + 1,
                                   WatchdogTriggerReason::HostExited, options.recoveryDirectory);
        }
        if (hostState == WAIT_FAILED) {
            return executeRollback(registry, options.statusHandle,
                                   lastSequence + 1,
                                   WatchdogTriggerReason::ProtocolViolation, options.recoveryDirectory);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastRenewal >=
            std::chrono::milliseconds(manifest->lease.timeoutMilliseconds)) {
            return executeRollback(registry, options.statusHandle,
                                   lastSequence + 1,
                                   WatchdogTriggerReason::LeaseExpired, options.recoveryDirectory);
        }

        std::vector<std::byte> bytes;
        std::uint32_t systemError = 0;
        const auto readState = tryReadFrame(options.controlHandle, bytes,
                                            systemError);
        if (readState == PipeReadState::NoFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMilliseconds));
            continue;
        }
        if (readState == PipeReadState::Closed) {
            return executeRollback(registry, options.statusHandle,
                                   lastSequence + 1,
                                   WatchdogTriggerReason::ControlChannelClosed, options.recoveryDirectory);
        }
        if (readState != PipeReadState::Frame) {
            return executeRollback(registry, options.statusHandle,
                                   lastSequence + 1,
                                   WatchdogTriggerReason::ProtocolViolation, options.recoveryDirectory);
        }

        const auto decoded = decodeWatchdogFrame(bytes);
        if (!decoded || decoded.frame->sequence <= lastSequence) {
            return executeRollback(registry, options.statusHandle,
                                   lastSequence + 1,
                                   WatchdogTriggerReason::ProtocolViolation, options.recoveryDirectory);
        }

        WatchdogLease lease;
        std::string error;
        if (decoded.frame->type == WatchdogMessageType::RenewLease) {
            if (!decodeLeaseRenewal(*decoded.frame, lease, &error) ||
                lease != manifest->lease) {
                return executeRollback(registry, options.statusHandle,
                                       decoded.frame->sequence + 1,
                                       WatchdogTriggerReason::ProtocolViolation, options.recoveryDirectory);
            }
            lastSequence = decoded.frame->sequence;
            lastRenewal = std::chrono::steady_clock::now();
            continue;
        }
        if (decoded.frame->type == WatchdogMessageType::Disarm) {
            if (!decodeDisarm(*decoded.frame, lease, &error) ||
                lease != manifest->lease) {
                return executeRollback(registry, options.statusHandle,
                                       decoded.frame->sequence + 1,
                                       WatchdogTriggerReason::ProtocolViolation, options.recoveryDirectory);
            }

            // A host request alone is not sufficient proof of rollback. Re-run
            // the registered idempotent plan as a safety backstop before the
            // watchdog releases its lease. Already-restored state is reported
            // as AlreadySatisfied; any unresolved action keeps recovery armed.
            const auto summary = executeRegisteredRollback(
                registry, options.recoveryDirectory);
            WatchdogStatus status;
            status.sessionId = manifest->lease.sessionId;
            status.generation = manifest->lease.generation;
            status.totalActions = static_cast<std::uint16_t>(
                manifest->actions.size());
            status.completedActions = static_cast<std::uint16_t>(
                std::count_if(summary.outcomes.begin(), summary.outcomes.end(),
                              [](const RollbackActionOutcome& outcome) {
                                  return outcome.result ==
                                             RollbackActionResult::Success ||
                                         outcome.result ==
                                             RollbackActionResult::AlreadySatisfied;
                              }));
            status.failedActionId = summary.firstFailedActionId;
            status.systemError = summary.firstSystemError;
            if (summary.recoveryRequired) {
                status.state = WatchdogRunState::RecoveryRequired;
                status.reason = WatchdogTriggerReason::RollbackFailure;
                (void)writeStatusFrame(options.statusHandle,
                                       decoded.frame->sequence + 1, status);
                printStatus(status);
                return kExitRecoveryRequired;
            }

            status.state = WatchdogRunState::Disarmed;
            status.reason = WatchdogTriggerReason::CleanDisarm;
            (void)writeStatusFrame(options.statusHandle,
                                   decoded.frame->sequence + 1, status);
            printStatus(status);
            return kExitCleanDisarm;
        }

        return executeRollback(registry, options.statusHandle,
                               decoded.frame->sequence + 1,
                               WatchdogTriggerReason::ProtocolViolation, options.recoveryDirectory);
    }
}

#endif

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        std::cerr << "Usage: hydra_watchdog --control-handle <inherited> "
                     "--status-handle <inherited> --host-pid <pid> "
                     "--host-created <100ns> --session <32-hex>\n";
        return kExitInvalidArguments;
    }
    return runWatchdog(options);
}
#else
int main() {
    std::cerr << "hydra_watchdog is available only on Windows.\n";
    return kExitStartupFailure;
}
#endif
