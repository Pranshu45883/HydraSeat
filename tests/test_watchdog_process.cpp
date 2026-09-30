#include "hydra/recovery_process_attachment.hpp"
#include "hydra/rollback_registry.hpp"
#include "hydra/watchdog_protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

using namespace hydra::watchdog;

constexpr DWORD kTestWaitMilliseconds = 7'000;
constexpr DWORD kTargetSleepMilliseconds = 60'000;
constexpr DWORD kForcedHostExitCode = 0x4853544bu; // "HSTK".

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << " (win32=" << GetLastError()
                  << ")\n";
        std::exit(EXIT_FAILURE);
    }
}

class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) noexcept : m_handle(handle) {}
    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }

    HANDLE get() const noexcept { return m_handle; }
    bool valid() const noexcept {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept { return std::exchange(m_handle, nullptr); }
    void reset(HANDLE handle = nullptr) noexcept {
        if (valid()) CloseHandle(m_handle);
        m_handle = handle;
    }

private:
    HANDLE m_handle{nullptr};
};

class ChildProcess {
public:
    ChildProcess() = default;
    ChildProcess(HANDLE process, std::uint32_t processId) noexcept
        : m_process(process), m_processId(processId) {}
    ~ChildProcess() {
        if (running()) {
            TerminateProcess(m_process.get(), 0x54455354u); // "TEST".
            WaitForSingleObject(m_process.get(), 2'000);
        }
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) noexcept = default;
    ChildProcess& operator=(ChildProcess&&) noexcept = default;

    HANDLE handle() const noexcept { return m_process.get(); }
    std::uint32_t processId() const noexcept { return m_processId; }
    bool valid() const noexcept { return m_process.valid(); }

    bool running() const noexcept {
        return valid() && WaitForSingleObject(m_process.get(), 0) == WAIT_TIMEOUT;
    }

    bool wait(DWORD timeoutMilliseconds, DWORD* exitCode = nullptr) const {
        if (!valid() ||
            WaitForSingleObject(m_process.get(), timeoutMilliseconds) !=
                WAIT_OBJECT_0) {
            return false;
        }
        DWORD code = 0;
        if (GetExitCodeProcess(m_process.get(), &code) == FALSE) return false;
        if (exitCode != nullptr) *exitCode = code;
        return true;
    }

    bool terminate(DWORD exitCode) {
        if (!running()) return true;
        return TerminateProcess(m_process.get(), exitCode) != FALSE;
    }

private:
    ScopedHandle m_process;
    std::uint32_t m_processId{0};
};

std::wstring modulePath() {
    std::vector<wchar_t> buffer(32'768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    check(length != 0 &&
              static_cast<std::size_t>(length) < buffer.size(),
          "module path is available");
    return std::wstring(buffer.data(), length);
}

std::wstring siblingPath(std::wstring_view name) {
    auto path = modulePath();
    const auto separator = path.find_last_of(L"\\/");
    check(separator != std::wstring::npos, "module path has a directory");
    path.resize(separator + 1);
    path.append(name);
    return path;
}

std::wstring quote(std::wstring_view value) {
    std::wstring result = L"\"";
    result.append(value);
    result.push_back(L'\"');
    return result;
}

ChildProcess launch(const std::wstring& executable,
                    const std::wstring& arguments,
                    std::span<const HANDLE> inheritedHandles = {}) {
    std::wstring command = quote(executable);
    if (!arguments.empty()) {
        command.push_back(L' ');
        command.append(arguments);
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(STARTUPINFOW);
    std::vector<std::byte> attributeStorage;
    DWORD creationFlags = CREATE_NO_WINDOW;
    if (!inheritedHandles.empty()) {
        startup.StartupInfo.cb = sizeof(startup);
        SIZE_T attributeBytes = 0;
        check(InitializeProcThreadAttributeList(nullptr, 1, 0,
                                                &attributeBytes) == FALSE &&
                  GetLastError() == ERROR_INSUFFICIENT_BUFFER &&
                  attributeBytes != 0,
              "process attribute size is queryable");
        attributeStorage.resize(attributeBytes);
        startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
            attributeStorage.data());
        check(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                                &attributeBytes) != FALSE,
              "process attribute list initializes");
        check(UpdateProcThreadAttribute(
                  startup.lpAttributeList, 0,
                  PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                  const_cast<HANDLE*>(inheritedHandles.data()),
                  inheritedHandles.size_bytes(), nullptr, nullptr) != FALSE,
              "process inherits only explicit handles");
        creationFlags |= EXTENDED_STARTUPINFO_PRESENT;
    }

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), mutableCommand.data(), nullptr, nullptr,
        inheritedHandles.empty() ? FALSE : TRUE,
        creationFlags, nullptr, nullptr, &startup.StartupInfo, &process);
    const DWORD createError = created == FALSE ? GetLastError() : ERROR_SUCCESS;
    if (startup.lpAttributeList != nullptr) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    }
    if (created == FALSE) {
        SetLastError(createError);
        check(false, "child process launches");
    }
    CloseHandle(process.hThread);
    return ChildProcess(process.hProcess, process.dwProcessId);
}

SessionId makeSession(std::uint8_t seed) {
    SessionId value{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<std::uint8_t>(seed + index + 1);
    }
    return value;
}

std::wstring sessionHex(const SessionId& sessionId) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(sessionId.size() * 2);
    for (const auto byte : sessionId) {
        result.push_back(digits[(byte >> 4u) & 0x0fu]);
        result.push_back(digits[byte & 0x0fu]);
    }
    return result;
}

std::wstring number(std::uint64_t value) {
    return std::to_wstring(value);
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

bool sendFrame(HANDLE handle, const std::vector<std::byte>& frame) {
    if (frame.empty() || frame.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const auto frameBytes = static_cast<std::uint32_t>(frame.size());
    const std::array<std::byte, 4> prefix{
        static_cast<std::byte>(frameBytes & 0xffu),
        static_cast<std::byte>((frameBytes >> 8u) & 0xffu),
        static_cast<std::byte>((frameBytes >> 16u) & 0xffu),
        static_cast<std::byte>((frameBytes >> 24u) & 0xffu)};
    return writeAll(handle, prefix.data(), prefix.size()) &&
           writeAll(handle, frame.data(), frame.size());
}

std::uint32_t prefixLength(const std::array<std::byte, 4>& prefix) {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(prefix[0])) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(prefix[1])) << 8u) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(prefix[2])) << 16u) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(prefix[3])) << 24u);
}

bool waitStatus(HANDLE handle, WatchdogStatus& status,
                DWORD timeoutMilliseconds = kTestWaitMilliseconds) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMilliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        std::array<std::byte, 4> prefix{};
        DWORD peeked = 0;
        if (PeekNamedPipe(handle, prefix.data(), static_cast<DWORD>(prefix.size()),
                          &peeked, &available, nullptr) == FALSE) {
            return false;
        }
        if (available < static_cast<DWORD>(prefix.size()) ||
            peeked < static_cast<DWORD>(prefix.size())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const auto frameBytes = prefixLength(prefix);
        if (frameBytes < kWatchdogFrameHeaderBytes ||
            frameBytes > kWatchdogMaxFrameBytes) {
            return false;
        }
        const auto totalBytes = static_cast<std::uint64_t>(prefix.size()) +
            frameBytes;
        if (static_cast<std::uint64_t>(available) < totalBytes) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        std::vector<std::byte> combined(static_cast<std::size_t>(totalBytes));
        DWORD read = 0;
        if (ReadFile(handle, combined.data(), static_cast<DWORD>(combined.size()),
                     &read, nullptr) == FALSE ||
            static_cast<std::size_t>(read) != combined.size()) {
            return false;
        }
        const auto frame = std::span<const std::byte>(combined).subspan(4);
        const auto decoded = decodeWatchdogFrame(frame);
        if (!decoded) return false;
        std::string error;
        return decodeWatchdogStatus(*decoded.frame, status, &error);
    }
    return false;
}

ProcessIdentity processIdentity(std::uint32_t processId) {
    ProcessIdentity identity;
    std::uint32_t error = 0;
    check(queryProcessIdentity(processId, identity, &error),
          "process creation identity is queryable");
    return identity;
}

RollbackPlanManifest processPlan(const SessionId& sessionId,
                                 std::span<const ProcessIdentity> targets,
                                 std::uint32_t leaseTimeoutMilliseconds) {
    RollbackPlanManifest manifest;
    manifest.lease.sessionId = sessionId;
    manifest.lease.generation = 1;
    manifest.lease.timeoutMilliseconds = leaseTimeoutMilliseconds;
    manifest.rollbackTimeoutMilliseconds = 2'000;

    std::uint32_t ordinal = 1;
    for (const auto& target : targets) {
        RollbackActionDescriptor action;
        action.actionId = ordinal;
        action.kind = RollbackActionKind::TerminateOwnedProcess;
        action.activationOrdinal = ordinal;
        action.timeoutMilliseconds = 1'000;
        action.generation = 1;
        action.process = target;
        manifest.actions.push_back(action);
        ++ordinal;
    }
    return manifest;
}

struct WatchdogSession {
    ScopedHandle controlWrite;
    ScopedHandle statusRead;
    ChildProcess watchdog;
    RollbackPlanManifest manifest;
};

WatchdogSession startWatchdog(std::span<const ProcessIdentity> targets,
                              std::uint32_t leaseTimeoutMilliseconds,
                              std::uint8_t sessionSeed) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE rawControlRead = nullptr;
    HANDLE rawControlWrite = nullptr;
    check(CreatePipe(&rawControlRead, &rawControlWrite, &security, 0) != FALSE,
          "control pipe is created");
    ScopedHandle controlRead(rawControlRead);
    ScopedHandle controlWrite(rawControlWrite);
    check(SetHandleInformation(controlWrite.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
          "host control writer is not inherited");

    HANDLE rawStatusRead = nullptr;
    HANDLE rawStatusWrite = nullptr;
    check(CreatePipe(&rawStatusRead, &rawStatusWrite, &security, 0) != FALSE,
          "status pipe is created");
    ScopedHandle statusRead(rawStatusRead);
    ScopedHandle statusWrite(rawStatusWrite);
    check(SetHandleInformation(statusRead.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
          "host status reader is not inherited");

    const auto host = processIdentity(GetCurrentProcessId());
    const auto sessionId = makeSession(sessionSeed);
    const auto watchdogPath = siblingPath(L"hydra_watchdog.exe");
    std::wstring arguments = L"--control-handle " +
        number(reinterpret_cast<std::uintptr_t>(controlRead.get())) +
        L" --status-handle " +
        number(reinterpret_cast<std::uintptr_t>(statusWrite.get())) +
        L" --host-pid " + number(host.processId) +
        L" --host-created " + number(host.creationTime100ns) +
        L" --session " + sessionHex(sessionId);
    const std::array<HANDLE, 2> watchdogHandles{
        controlRead.get(), statusWrite.get()};
    auto watchdog = launch(watchdogPath, arguments, watchdogHandles);

    controlRead.reset();
    statusWrite.reset();

    auto manifest = processPlan(sessionId, targets, leaseTimeoutMilliseconds);
    check(sendFrame(controlWrite.get(), encodeRegisterPlan(1, manifest)),
          "rollback plan is sent to watchdog");
    WatchdogStatus status;
    check(waitStatus(statusRead.get(), status) &&
              status.state == WatchdogRunState::Armed &&
              status.sessionId == sessionId,
          "watchdog confirms armed state");

    return {std::move(controlWrite), std::move(statusRead),
            std::move(watchdog), std::move(manifest)};
}

WatchdogSession startWatchdog(const ProcessIdentity& target,
                              std::uint32_t leaseTimeoutMilliseconds,
                              std::uint8_t sessionSeed) {
    return startWatchdog(std::span<const ProcessIdentity>(&target, 1u),
                         leaseTimeoutMilliseconds, sessionSeed);
}

ChildProcess startTarget() {
    return launch(modulePath(), L"--target");
}

void testCleanDisarm() {
    auto target = startTarget();
    auto identity = processIdentity(target.processId());
    auto session = startWatchdog(identity, 2'000, 0x10);

    check(target.terminate(0), "host-side verified rollback terminates target");
    check(target.wait(2'000), "host-side rollback target exits");
    check(sendFrame(session.controlWrite.get(),
                    encodeDisarm(2, session.manifest.lease)),
          "clean disarm is sent");

    WatchdogStatus status;
    check(waitStatus(session.statusRead.get(), status) &&
              status.state == WatchdogRunState::Disarmed &&
              status.reason == WatchdogTriggerReason::CleanDisarm,
          "watchdog reports clean disarm");
    DWORD exitCode = 0;
    check(session.watchdog.wait(kTestWaitMilliseconds, &exitCode) && exitCode == 0,
          "clean disarm exits watchdog successfully");
}

void testDisarmBackstopCompletesRegisteredRollback() {
    auto target = startTarget();
    auto identity = processIdentity(target.processId());
    auto session = startWatchdog(identity, 2'000, 0x18);

    check(sendFrame(session.controlWrite.get(),
                    encodeDisarm(2, session.manifest.lease)),
          "disarm request is sent while target is still running");
    WatchdogStatus status;
    check(waitStatus(session.statusRead.get(), status) &&
              status.state == WatchdogRunState::Disarmed &&
              status.reason == WatchdogTriggerReason::CleanDisarm &&
              status.completedActions == 1,
          "watchdog completes registered rollback before disarming");
    DWORD exitCode = 0;
    check(session.watchdog.wait(kTestWaitMilliseconds, &exitCode) && exitCode == 0,
          "rollback-backed disarm exits watchdog successfully");
    check(target.wait(kTestWaitMilliseconds),
          "disarm cannot leave a registered owned target alive");
}

void testLeaseExpiryRollsBackOwnedTarget() {
    auto target = startTarget();
    auto identity = processIdentity(target.processId());
    auto session = startWatchdog(identity, 200, 0x20);

    hydra::recovery::RecoveryProcessAttachmentIdentity attachment;
    attachment.seatId = 1u;
    attachment.hostSessionId.bytes = makeSession(0x21u);
    attachment.sessionGeneration = 1u;
    attachment.seatGameGeneration = 1u;
    attachment.process = identity;
    attachment.recoveryEpoch = session.manifest.lease.generation;
    hydra::recovery::RecoveryProcessAttachmentRegistration registration;
    registration.identity = attachment;
    registration.manifest = session.manifest;
    hydra::recovery::RecoveryProcessAttachmentAuthority authority;
    check(authority.registerAttachment(registration).succeeded(),
          "real watchdog process plan registers against exact Seat/process attachment");

    WatchdogStatus status;
    check(waitStatus(session.statusRead.get(), status) &&
              status.state == WatchdogRunState::RollbackComplete &&
              status.reason == WatchdogTriggerReason::LeaseExpired &&
              status.completedActions == 1 &&
              status.sessionId == session.manifest.lease.sessionId &&
              status.generation == attachment.recoveryEpoch,
          "lease expiry preserves the exact registered recovery lease identity");
    DWORD exitCode = 0;
    check(session.watchdog.wait(kTestWaitMilliseconds, &exitCode) && exitCode == 10,
          "lease expiry exits with rollback-complete code");
    check(target.wait(kTestWaitMilliseconds),
          "lease expiry terminates the exact owned target");
    check(authority.verifyArmed(attachment, session.manifest.lease).succeeded(),
          "watchdog expiry does not silently discard host attachment ownership");
    check(authority.disarm(attachment, session.manifest.lease).succeeded(),
          "verified external cleanup permits exact attachment disarm");
}

void testProtocolViolationFailsClosedIntoRollback() {
    auto target = startTarget();
    auto identity = processIdentity(target.processId());
    auto session = startWatchdog(identity, 2'000, 0x30);

    check(sendFrame(session.controlWrite.get(),
                    encodeLeaseRenewal(1, session.manifest.lease)),
          "stale renewal frame is sent");
    WatchdogStatus status;
    check(waitStatus(session.statusRead.get(), status) &&
              status.state == WatchdogRunState::RollbackComplete &&
              status.reason == WatchdogTriggerReason::ProtocolViolation,
          "stale sequence fails closed into rollback");
    DWORD exitCode = 0;
    check(session.watchdog.wait(kTestWaitMilliseconds, &exitCode) && exitCode == 10,
          "protocol violation exits after successful rollback");
    check(target.wait(kTestWaitMilliseconds),
          "protocol violation does not leave controlled target alive");
}

void testProcessIdentityMismatchNeverKillsReusedPid() {
    auto target = startTarget();
    auto wrongIdentity = processIdentity(target.processId());
    ++wrongIdentity.creationTime100ns;
    auto session = startWatchdog(wrongIdentity, 200, 0x40);

    WatchdogStatus status;
    check(waitStatus(session.statusRead.get(), status) &&
              status.state == WatchdogRunState::RecoveryRequired &&
              status.reason == WatchdogTriggerReason::LeaseExpired &&
              status.failedActionId == 1,
          "identity mismatch becomes recovery-required");
    DWORD exitCode = 0;
    check(session.watchdog.wait(kTestWaitMilliseconds, &exitCode) && exitCode == 11,
          "identity mismatch returns recovery-required exit code");
    check(target.running(), "creation-time mismatch prevents process termination");
    check(target.terminate(0) && target.wait(2'000),
          "test cleans up intentionally preserved target");
}

void encodeReadyPayload(std::array<std::byte, 28>& payload,
                        const ProcessIdentity& first,
                        const ProcessIdentity& second,
                        std::uint32_t watchdogPid) {
    const auto put32 = [&payload](std::size_t offset, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            payload[offset + shift / 8] =
                static_cast<std::byte>((value >> shift) & 0xffu);
        }
    };
    const auto put64 = [&payload](std::size_t offset, std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            payload[offset + shift / 8] =
                static_cast<std::byte>((value >> shift) & 0xffu);
        }
    };
    put32(0, first.processId);
    put64(4, first.creationTime100ns);
    put32(12, second.processId);
    put64(16, second.creationTime100ns);
    put32(24, watchdogPid);
}

std::uint32_t get32(const std::array<std::byte, 28>& payload,
                    std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(payload[offset + shift / 8])) << shift;
    }
    return value;
}

std::uint64_t get64(const std::array<std::byte, 28>& payload,
                    std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(payload[offset + shift / 8])) << shift;
    }
    return value;
}

bool readReady(HANDLE handle, std::array<std::byte, 28>& payload) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kTestWaitMilliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
            return false;
        }
        if (available < payload.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        DWORD read = 0;
        return ReadFile(handle, payload.data(), static_cast<DWORD>(payload.size()),
                        &read, nullptr) != FALSE && read == payload.size();
    }
    return false;
}

int runHostKillStub(HANDLE readyHandle) {
    auto firstTarget = startTarget();
    auto secondTarget = startTarget();
    const std::array identities{
        processIdentity(firstTarget.processId()),
        processIdentity(secondTarget.processId())};
    auto session = startWatchdog(identities, 5'000, 0x50);

    std::array<std::byte, 28> payload{};
    encodeReadyPayload(payload, identities[0], identities[1],
                       session.watchdog.processId());
    if (!writeAll(readyHandle, payload.data(), payload.size())) return 21;
    CloseHandle(readyHandle);

    // The parent test forcibly terminates this host stub. If that does not
    // happen, leave enough time for the lease to expire and fail the test.
    Sleep(kTargetSleepMilliseconds);
    return 22;
}

void testHostDeathTriggersIndependentRollback() {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE rawReadyRead = nullptr;
    HANDLE rawReadyWrite = nullptr;
    check(CreatePipe(&rawReadyRead, &rawReadyWrite, &security, 0) != FALSE,
          "host-kill ready pipe is created");
    ScopedHandle readyRead(rawReadyRead);
    ScopedHandle readyWrite(rawReadyWrite);
    check(SetHandleInformation(readyRead.get(), HANDLE_FLAG_INHERIT, 0) != FALSE,
          "parent ready reader is not inherited");

    const std::wstring arguments = L"--host-kill-stub " +
        number(reinterpret_cast<std::uintptr_t>(readyWrite.get()));
    const std::array<HANDLE, 1> hostHandles{readyWrite.get()};
    auto host = launch(modulePath(), arguments, hostHandles);
    readyWrite.reset();

    std::array<std::byte, 28> payload{};
    check(readReady(readyRead.get(), payload),
          "host stub reports both target/watchdog identities");
    const auto firstTargetPid = get32(payload, 0);
    const auto firstTargetCreation = get64(payload, 4);
    const auto secondTargetPid = get32(payload, 12);
    const auto secondTargetCreation = get64(payload, 16);
    const auto watchdogPid = get32(payload, 24);
    check(firstTargetPid != 0 && firstTargetCreation != 0 &&
              secondTargetPid != 0 && secondTargetCreation != 0 &&
              firstTargetPid != secondTargetPid && watchdogPid != 0,
          "host stub reports two distinct nonzero process identities");

    const HANDLE rawFirstTarget = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, firstTargetPid);
    const HANDLE rawSecondTarget = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, secondTargetPid);
    const HANDLE rawWatchdog = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, watchdogPid);
    check(rawFirstTarget != nullptr && rawSecondTarget != nullptr &&
              rawWatchdog != nullptr,
          "parent obtains observation handles before host death");
    const auto observedFirst = processIdentity(firstTargetPid);
    const auto observedSecond = processIdentity(secondTargetPid);
    check(observedFirst.creationTime100ns == firstTargetCreation &&
              observedSecond.creationTime100ns == secondTargetCreation,
          "parent verifies both exact target creation identities before host death");
    ScopedHandle firstTarget(rawFirstTarget);
    ScopedHandle secondTarget(rawSecondTarget);
    ScopedHandle watchdog(rawWatchdog);

    check(host.terminate(kForcedHostExitCode) && host.wait(2'000),
          "parent forcibly terminates host stub");
    check(WaitForSingleObject(watchdog.get(), kTestWaitMilliseconds) == WAIT_OBJECT_0,
          "watchdog survives host death long enough to finish rollback");
    DWORD watchdogExit = 0;
    check(GetExitCodeProcess(watchdog.get(), &watchdogExit) != FALSE &&
              watchdogExit == 10,
          "host-death watchdog exits rollback-complete");
    check(WaitForSingleObject(firstTarget.get(), kTestWaitMilliseconds) ==
                  WAIT_OBJECT_0 &&
              WaitForSingleObject(secondTarget.get(), kTestWaitMilliseconds) ==
                  WAIT_OBJECT_0,
          "host-death watchdog terminates both exact targets without UI participation");
}

bool parseInheritedHandle(std::wstring_view text, HANDLE& handle) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const auto value = std::wcstoull(text.data(), &end, 0);
    if (end == text.data() ||
        static_cast<std::size_t>(end - text.data()) != text.size() ||
        value == 0 ||
        value > std::numeric_limits<std::uintptr_t>::max()) {
        return false;
    }
    handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--target") {
        Sleep(kTargetSleepMilliseconds);
        return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--host-kill-stub") {
        HANDLE readyHandle = INVALID_HANDLE_VALUE;
        if (!parseInheritedHandle(argv[2], readyHandle) ||
            GetFileType(readyHandle) != FILE_TYPE_PIPE) {
            return 20;
        }
        return runHostKillStub(readyHandle);
    }
    if (argc != 1) return 2;

    testCleanDisarm();
    testDisarmBackstopCompletesRegisteredRollback();
    testLeaseExpiryRollsBackOwnedTarget();
    testProtocolViolationFailsClosedIntoRollback();
    testProcessIdentityMismatchNeverKillsReusedPid();
    testHostDeathTriggersIndependentRollback();

    std::cout << "Watchdog Windows process/fault tests passed.\n";
    return EXIT_SUCCESS;
}
