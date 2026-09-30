#include "hydra/reset_actions.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace hydra;

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

std::wstring quote(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

class OwnedProcess {
public:
    OwnedProcess() = default;
    OwnedProcess(const OwnedProcess&) = delete;
    OwnedProcess& operator=(const OwnedProcess&) = delete;
    OwnedProcess(OwnedProcess&&) = delete;
    OwnedProcess& operator=(OwnedProcess&&) = delete;

    ~OwnedProcess() {
        if (m_process != nullptr) {
            if (WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) {
                (void)TerminateProcess(m_process, 0x52535454u); // "RSTT".
                (void)WaitForSingleObject(m_process, 2'000);
            }
            CloseHandle(m_process);
        }
    }

    bool launch(const std::filesystem::path& executable,
                std::wstring arguments = {}) {
        std::wstring command = quote(executable);
        if (!arguments.empty()) {
            command.push_back(L' ');
            command += arguments;
        }
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(),
            &startup, &process);
        if (created == FALSE) return false;
        CloseHandle(process.hThread);
        m_process = process.hProcess;
        m_processId = process.dwProcessId;
        return true;
    }

    bool running() const {
        return m_process != nullptr &&
               WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
    }

    bool wait(std::uint32_t timeoutMilliseconds, DWORD* exitCode = nullptr) const {
        if (m_process == nullptr) return false;
        if (WaitForSingleObject(m_process, timeoutMilliseconds) != WAIT_OBJECT_0) {
            return false;
        }
        if (exitCode != nullptr && GetExitCodeProcess(m_process, exitCode) == FALSE) {
            return false;
        }
        return true;
    }

    bool terminate(UINT exitCode) const {
        if (m_process == nullptr) return false;
        if (WaitForSingleObject(m_process, 0) == WAIT_OBJECT_0) return true;
        return TerminateProcess(m_process, exitCode) != FALSE && wait(2'000);
    }

    std::uint32_t processId() const noexcept { return m_processId; }

    watchdog::ProcessIdentity identity() const {
        watchdog::ProcessIdentity result;
        std::uint32_t systemError = 0;
        require(watchdog::queryProcessIdentity(m_processId, result, &systemError),
                "child process identity must be queryable");
        return result;
    }

private:
    HANDLE m_process{nullptr};
    std::uint32_t m_processId{0};
};

class TempDirectory {
public:
    explicit TempDirectory(std::string_view label) {
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        m_path = std::filesystem::temp_directory_path() /
            ("hydra-reset-process-" + std::string(label) + "-" +
             std::to_string(GetCurrentProcessId()) + "-" + std::to_string(tick));
        std::filesystem::create_directories(m_path);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

std::filesystem::path selfPath() {
    std::wstring buffer(32'768, L'\0');
    const DWORD count = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(count != 0 && count < buffer.size(),
            "reset process test executable path must resolve");
    buffer.resize(count);
    return std::filesystem::path(buffer);
}

watchdog::SessionId session(std::uint8_t base) {
    watchdog::SessionId result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(base + index);
    }
    return result;
}

watchdog::RollbackPlanManifest manifestFor(
    const watchdog::SessionId& sessionId,
    const watchdog::ProcessIdentity& target) {
    watchdog::RollbackPlanManifest manifest;
    manifest.lease.sessionId = sessionId;
    manifest.lease.generation = 3;
    manifest.lease.timeoutMilliseconds = 2'000;
    manifest.rollbackTimeoutMilliseconds = 5'000;
    watchdog::RollbackActionDescriptor action;
    action.actionId = 1;
    action.kind = watchdog::RollbackActionKind::TerminateOwnedProcess;
    action.activationOrdinal = 1;
    action.timeoutMilliseconds = 2'000;
    action.generation = 3;
    action.process = target;
    manifest.actions.push_back(action);
    return manifest;
}

void persistRecord(recovery::CrashJournalStore& store,
                   recovery::CrashJournalState& state,
                   const watchdog::RollbackPlanManifest& manifest,
                   recovery::CrashJournalRecordKind kind,
                   std::uint32_t actionId,
                   std::uint64_t generation) {
    std::string error;
    require(recovery::appendCrashJournalRecord(
                state, manifest, kind, actionId, generation, &error),
            "process-test journal record must append");
    require(store.persistTransition(state, &error),
            "process-test journal record must persist");
}

void seedActiveRecovery(const std::filesystem::path& root,
                        const watchdog::ProcessIdentity& owner,
                        const watchdog::ProcessIdentity& target,
                        const watchdog::SessionId& sessionId,
                        bool enterSafeMode) {
    recovery::NativeCrashJournalStorage storage(root);
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(root);
    const auto manifest = manifestFor(sessionId, target);
    recovery::RecoveryProcessAttachmentIdentity attachment;
    attachment.seatId = 1u;
    attachment.hostSessionId.bytes = sessionId;
    attachment.sessionGeneration = manifest.lease.generation;
    attachment.seatGameGeneration = 1u;
    attachment.process = target;
    attachment.recoveryEpoch = manifest.lease.generation;
    std::string error;
    const auto attachmentSnapshot =
        recovery::makeRecoveryProcessAttachmentSnapshot(attachment, &error);
    require(attachmentSnapshot.has_value(),
            "process-test exact recovery attachment snapshot must be valid");
    const std::array snapshots{*attachmentSnapshot};
    auto state = recovery::makeInitialCrashJournal(
        manifest, manifest.lease.generation, snapshots, &error);
    require(state.has_value(), "process-test initial journal must be valid");
    require(journalStore.beginActivation(*state, &error),
            "process-test journal activation must begin");
    persistRecord(journalStore, *state, manifest,
                  recovery::CrashJournalRecordKind::ActionPrepared,
                  1, 3);
    persistRecord(journalStore, *state, manifest,
                  recovery::CrashJournalRecordKind::ActionApplied,
                  1, 3);
    persistRecord(journalStore, *state, manifest,
                  recovery::CrashJournalRecordKind::ActionVerified,
                  1, 3);
    persistRecord(journalStore, *state, manifest,
                  recovery::CrashJournalRecordKind::ActivationCommitted,
                  0, 3);

    reset::RuntimeResetRegistration registration;
    registration.ownerProcess = owner;
    registration.manifest = manifest;
    registration.attachment = attachment;
    require(registrationStore.write(registration, &error),
            "process-test runtime registration must write");

    if (enterSafeMode) {
        const auto assessment = journalStore.assessStartupAndEnterSafeMode();
        require(assessment.state == recovery::StartupRecoveryState::RecoverableIncomplete,
                "process-test active journal must enter recoverable safe mode");
    }
}

DWORD runReset(const std::filesystem::path& resetExecutable,
               const std::filesystem::path& recoveryDirectory,
               std::wstring_view command) {
    OwnedProcess process;
    std::wstring arguments(command);
    arguments += L" --recovery-dir ";
    arguments += quote(recoveryDirectory);
    arguments += L" --json";
    require(process.launch(resetExecutable, arguments),
            "hydra_reset executable must launch independently");
    DWORD exitCode = 0;
    require(process.wait(20'000, &exitCode),
            "hydra_reset executable must exit within the bounded timeout");
    return exitCode;
}

void verifyClean(const std::filesystem::path& root) {
    recovery::NativeCrashJournalStorage storage(root);
    recovery::CrashJournalStore journalStore(storage);
    reset::RuntimeResetRegistrationStore registrationStore(root);
    std::string error;
    const auto current = journalStore.loadCurrent(&error);
    require(current && current->phase == recovery::CrashJournalPhase::Clean &&
                current->finalResult == recovery::CrashJournalFinalResult::Clean,
            "hydra_reset must leave a verified clean journal");
    require(!journalStore.loadSafeMode(&error),
            "hydra_reset must clear correlated safe mode");
    require(registrationStore.load().status ==
                reset::RuntimeRegistrationReadStatus::Missing,
            "hydra_reset must remove runtime registration after verified cleanup");
}

void testLiveOwnerAndSameNameSentinel(const std::filesystem::path& self,
                                      const std::filesystem::path& resetExecutable) {
    TempDirectory temp("live-owner");
    OwnedProcess owner;
    OwnedProcess target;
    OwnedProcess sentinel;
    require(owner.launch(self, L"--child") &&
                target.launch(self, L"--child") &&
                sentinel.launch(self, L"--child"),
            "live-owner process fixtures must launch");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(owner.running() && target.running() && sentinel.running(),
            "all live-owner process fixtures must remain alive before reset");
    const auto ownerIdentity = owner.identity();
    const auto targetIdentity = target.identity();
    const auto sentinelIdentity = sentinel.identity();
    seedActiveRecovery(temp.path(), ownerIdentity, targetIdentity, session(1), false);

    require(runReset(resetExecutable, temp.path(), L"dry-run") == 0,
            "hydra_reset dry-run must inspect recoverable exact actions without mutation");
    require(owner.running() && target.running() && sentinel.running(),
            "dry-run must not terminate any process");

    require(runReset(resetExecutable, temp.path(), L"all --confirm") == 0,
            "hydra_reset all --confirm must recover live exact owner/target");
    require(owner.wait(3'000) && target.wait(3'000),
            "verified reset must terminate exact runtime owner and target");
    require(sentinel.running() && sentinel.identity() == sentinelIdentity,
            "unrelated same-name sentinel exact identity must survive reset");
    verifyClean(temp.path());
    require(runReset(resetExecutable, temp.path(), L"all --confirm") == 0,
            "repeated hydra_reset after verified cleanup must be idempotent");
    require(sentinel.running() && sentinel.identity() == sentinelIdentity,
            "repeated reset must still preserve unrelated same-name sentinel");
}

void testIdentityMismatchFailsClosed(const std::filesystem::path& self,
                                     const std::filesystem::path& resetExecutable) {
    TempDirectory temp("identity-mismatch");
    OwnedProcess owner;
    OwnedProcess target;
    OwnedProcess sentinel;
    require(owner.launch(self, L"--child") &&
                target.launch(self, L"--child") &&
                sentinel.launch(self, L"--child"),
            "identity-mismatch process fixtures must launch");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto realOwnerIdentity = owner.identity();
    const auto targetIdentity = target.identity();
    const auto sentinelIdentity = sentinel.identity();
    auto staleOwnerIdentity = realOwnerIdentity;
    staleOwnerIdentity.creationTime100ns ^= 1u;
    require(staleOwnerIdentity.creationTime100ns != 0 &&
                staleOwnerIdentity != realOwnerIdentity,
            "identity-mismatch fixture must use a distinct nonzero creation identity");
    seedActiveRecovery(temp.path(), staleOwnerIdentity, targetIdentity,
                       session(30), false);

    require(runReset(resetExecutable, temp.path(), L"all --confirm") == 2,
            "hydra_reset must fail closed on an exact owner identity mismatch");
    require(owner.running() && owner.identity() == realOwnerIdentity,
            "identity mismatch must not terminate the PID with different creation time");
    require(target.running() && target.identity() == targetIdentity,
            "owner identity failure must stop before rollback target mutation");
    require(sentinel.running() && sentinel.identity() == sentinelIdentity,
            "identity mismatch must preserve unrelated same-name sentinel");

    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    std::string error;
    const auto marker = journalStore.loadSafeMode(&error);
    require(marker && marker->reason == recovery::SafeModeReason::RecoveryRequired,
            "identity mismatch must leave durable RecoveryRequired evidence");
}

void testAttachmentSessionMismatchFailsClosed(
    const std::filesystem::path& self,
    const std::filesystem::path& resetExecutable) {
    TempDirectory temp("attachment-session-mismatch");
    OwnedProcess owner;
    OwnedProcess target;
    OwnedProcess sentinel;
    require(owner.launch(self, L"--child") &&
                target.launch(self, L"--child") &&
                sentinel.launch(self, L"--child"),
            "attachment-session mismatch process fixtures must launch");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto ownerIdentity = owner.identity();
    const auto targetIdentity = target.identity();
    const auto sentinelIdentity = sentinel.identity();
    seedActiveRecovery(temp.path(), ownerIdentity, targetIdentity, session(40), false);

    reset::RuntimeResetRegistrationStore registrationStore(temp.path());
    auto loaded = registrationStore.load();
    require(loaded.status == reset::RuntimeRegistrationReadStatus::Success &&
                loaded.registration && loaded.registration->attachment,
            "exact attachment reset registration must load before mismatch injection");
    auto mismatched = *loaded.registration;
    mismatched.attachment->hostSessionId.bytes[0] ^= 1u;
    std::string error;
    require(registrationStore.write(mismatched, &error),
            "individually valid wrong-host-session attachment persists for adversarial reset test");

    require(runReset(resetExecutable, temp.path(), L"all --confirm") == 2,
            "hydra_reset must fail closed on crash-journal/attachment Host-session mismatch");
    require(owner.running() && owner.identity() == ownerIdentity,
            "Host-session mismatch must not terminate exact runtime owner");
    require(target.running() && target.identity() == targetIdentity,
            "Host-session mismatch must stop before exact target rollback mutation");
    require(sentinel.running() && sentinel.identity() == sentinelIdentity,
            "Host-session mismatch must preserve unrelated same-name sentinel");

    recovery::NativeCrashJournalStorage storage(temp.path());
    recovery::CrashJournalStore journalStore(storage);
    const auto marker = journalStore.loadSafeMode(&error);
    require(marker && marker->reason == recovery::SafeModeReason::RecoveryRequired,
            "Host-session mismatch must leave durable RecoveryRequired evidence");
}

void testDeadOwnerAndStaleJournal(const std::filesystem::path& self,
                                  const std::filesystem::path& resetExecutable) {
    TempDirectory temp("dead-owner");
    OwnedProcess owner;
    OwnedProcess target;
    OwnedProcess sentinel;
    require(owner.launch(self, L"--child") &&
                target.launch(self, L"--child") &&
                sentinel.launch(self, L"--child"),
            "dead-owner process fixtures must launch");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto ownerIdentity = owner.identity();
    const auto targetIdentity = target.identity();
    const auto sentinelIdentity = sentinel.identity();
    seedActiveRecovery(temp.path(), ownerIdentity, targetIdentity, session(50), false);
    require(owner.terminate(0x484f5354u),
            "dead-owner scenario must terminate only the exact owner fixture");

    {
        recovery::NativeCrashJournalStorage storage(temp.path());
        recovery::CrashJournalStore journalStore(storage);
        const auto assessment = journalStore.assessStartupAndEnterSafeMode();
        require(assessment.state == recovery::StartupRecoveryState::RecoverableIncomplete &&
                    assessment.safeMode.has_value(),
                "dead-owner stale journal must produce a safe-mode marker before reset");
    }

    require(runReset(resetExecutable, temp.path(), L"all --confirm") == 0,
            "hydra_reset must recover stale journal after owner death");
    require(target.wait(3'000),
            "dead-owner reset must terminate exact remaining target");
    require(sentinel.running() && sentinel.identity() == sentinelIdentity,
            "dead-owner reset must preserve unrelated same-name sentinel");
    verifyClean(temp.path());
    require(runReset(resetExecutable, temp.path(), L"status") == 0,
            "hydra_reset status must report verified clean state after recovery");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--child") {
        for (;;) {
            Sleep(250);
        }
    }

    try {
        const auto self = selfPath();
        const auto resetExecutable = self.parent_path() / L"hydra_reset.exe";
        require(std::filesystem::exists(resetExecutable),
                "hydra_reset.exe must be staged beside reset_process_tests.exe");
        testLiveOwnerAndSameNameSentinel(self, resetExecutable);
        testIdentityMismatchFailsClosed(self, resetExecutable);
        testAttachmentSessionMismatchFailsClosed(self, resetExecutable);
        testDeadOwnerAndStaleJournal(self, resetExecutable);
        std::cout << "Reset process tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
