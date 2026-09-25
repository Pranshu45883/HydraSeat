#include "hydra/rollback_registry.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hydra::watchdog {
namespace {

RollbackActionOutcome makeOutcome(const RollbackActionDescriptor& action,
                                  RollbackActionResult result,
                                  std::uint32_t systemError = 0) {
    return {action.actionId, action.kind, result, systemError};
}

bool satisfied(RollbackActionResult result) noexcept {
    return result == RollbackActionResult::Success ||
           result == RollbackActionResult::AlreadySatisfied;
}

RollbackActionOutcome unsupported(const RollbackActionDescriptor& action) {
    return makeOutcome(action, RollbackActionResult::Unsupported);
}

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

std::uint64_t fileTimeValue(const FILETIME& value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32u) |
           static_cast<std::uint64_t>(value.dwLowDateTime);
}

bool readProcessIdentity(HANDLE process,
                         std::uint32_t processId,
                         ProcessIdentity& identity,
                         std::uint32_t* systemError) noexcept {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user) == FALSE) {
        const DWORD error = GetLastError();
        if (systemError != nullptr) *systemError = error;
        return false;
    }
    identity.processId = processId;
    identity.creationTime100ns = fileTimeValue(creation);
    if (systemError != nullptr) *systemError = 0;
    return identity.creationTime100ns != 0;
}

#endif

} // namespace

bool RollbackRegistry::registerPlan(const RollbackPlanManifest& manifest,
                                    std::string* error) {
    if (!validateRollbackPlan(manifest, error)) {
        return false;
    }
    if (m_manifest.has_value()) {
        if (*m_manifest == manifest) {
            return true;
        }
        if (error != nullptr) {
            *error = "rollback registry is already armed with a different plan";
        }
        return false;
    }
    m_manifest = manifest;
    m_completedActionIds.clear();
    return true;
}

RollbackExecutionSummary RollbackRegistry::execute(RollbackExecutor& executor) {
    RollbackExecutionSummary summary;
    if (!m_manifest) {
        summary.recoveryRequired = true;
        return summary;
    }

    std::vector<const RollbackActionDescriptor*> ordered;
    ordered.reserve(m_manifest->actions.size());
    for (const auto& action : m_manifest->actions) {
        ordered.push_back(&action);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto* left, const auto* right) {
                  return left->activationOrdinal > right->activationOrdinal;
              });

    const auto started = std::chrono::steady_clock::now();
    const auto totalBudget = std::chrono::milliseconds(
        m_manifest->rollbackTimeoutMilliseconds);
    summary.outcomes.reserve(ordered.size());

    for (const auto* action : ordered) {
        if (m_completedActionIds.contains(action->actionId)) {
            summary.outcomes.push_back(
                makeOutcome(*action, RollbackActionResult::AlreadySatisfied));
            continue;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        if (elapsed >= totalBudget) {
            const auto outcome =
                makeOutcome(*action, RollbackActionResult::TimedOut);
            summary.outcomes.push_back(outcome);
            summary.recoveryRequired = true;
            if (summary.firstFailedActionId == 0) {
                summary.firstFailedActionId = action->actionId;
            }
            continue;
        }

        const auto remaining = totalBudget - elapsed;
        const auto remainingCount = remaining.count();
        const auto boundedRemaining = remainingCount > 0
            ? static_cast<std::uint64_t>(remainingCount)
            : std::uint64_t{0};
        const auto timeoutMilliseconds = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(action->timeoutMilliseconds,
                                    boundedRemaining));
        if (timeoutMilliseconds == 0) {
            const auto outcome =
                makeOutcome(*action, RollbackActionResult::TimedOut);
            summary.outcomes.push_back(outcome);
            summary.recoveryRequired = true;
            if (summary.firstFailedActionId == 0) {
                summary.firstFailedActionId = action->actionId;
            }
            continue;
        }

        auto outcome = executeOne(executor, *action, timeoutMilliseconds);
        summary.outcomes.push_back(outcome);
        if (satisfied(outcome.result)) {
            m_completedActionIds.insert(action->actionId);
        } else {
            summary.recoveryRequired = true;
            if (summary.firstFailedActionId == 0) {
                summary.firstFailedActionId = action->actionId;
                summary.firstSystemError = outcome.systemError;
            }
        }
    }

    summary.allSatisfied = !summary.recoveryRequired &&
        m_completedActionIds.size() == m_manifest->actions.size();
    return summary;
}

void RollbackRegistry::clear() noexcept {
    m_manifest.reset();
    m_completedActionIds.clear();
}

RollbackActionOutcome RollbackRegistry::executeOne(
    RollbackExecutor& executor,
    const RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    switch (action.kind) {
    case RollbackActionKind::TerminateOwnedProcess:
        return executor.terminateOwnedProcess(action, timeoutMilliseconds);
    case RollbackActionKind::CloseOwnedSession:
        return executor.closeOwnedSession(action, timeoutMilliseconds);
    case RollbackActionKind::ClearOptionalBackendState:
        return executor.clearOptionalBackendState(action, timeoutMilliseconds);
    case RollbackActionKind::ReleaseOverlayState:
        return executor.releaseOverlayState(action, timeoutMilliseconds);
    case RollbackActionKind::RestoreSnapshotState:
        return executor.restoreSnapshotState(action, timeoutMilliseconds);
    case RollbackActionKind::WriteSafeModeResult:
        return executor.writeSafeModeResult(action, timeoutMilliseconds);
    }
    return makeOutcome(action, RollbackActionResult::InvalidAction);
}

DefaultRollbackExecutor::~DefaultRollbackExecutor() {
    clearPreparedOwnedProcesses();
}

bool DefaultRollbackExecutor::prepareOwnedProcesses(
    std::span<const RollbackActionDescriptor> actions,
    std::string* error) {
    clearPreparedOwnedProcesses();
    if (error != nullptr) error->clear();
#if defined(_WIN32)
    for (const auto& action : actions) {
        if (action.kind != RollbackActionKind::TerminateOwnedProcess) continue;
        if (action.actionId == 0 || action.process.processId == 0 ||
            action.process.creationTime100ns == 0) {
            if (error != nullptr) {
                *error = "invalid terminate-owned-process action in reset preflight";
            }
            clearPreparedOwnedProcesses();
            return false;
        }

        const HANDLE rawProcess = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
            FALSE, action.process.processId);
        if (rawProcess == nullptr) {
            const DWORD systemError = GetLastError();
            if (systemError == ERROR_INVALID_PARAMETER) {
                m_preparedOwnedProcesses.push_back(
                    {action.actionId, action.process, 0, true});
                continue;
            }
            if (error != nullptr) {
                *error = "failed to acquire exact rollback process before owner stop: " +
                         std::to_string(systemError);
            }
            clearPreparedOwnedProcesses();
            return false;
        }

        ProcessIdentity observed;
        std::uint32_t identityError = 0;
        if (!readProcessIdentity(rawProcess, action.process.processId,
                                 observed, &identityError)) {
            CloseHandle(rawProcess);
            if (error != nullptr) {
                *error = "failed to verify rollback process identity before owner stop: " +
                         std::to_string(identityError);
            }
            clearPreparedOwnedProcesses();
            return false;
        }
        if (observed != action.process) {
            CloseHandle(rawProcess);
            if (error != nullptr) {
                *error = "rollback process identity changed before owner stop";
            }
            clearPreparedOwnedProcesses();
            return false;
        }

        const DWORD before = WaitForSingleObject(rawProcess, 0);
        if (before == WAIT_FAILED) {
            const DWORD systemError = GetLastError();
            CloseHandle(rawProcess);
            if (error != nullptr) {
                *error = "failed to inspect rollback process before owner stop: " +
                         std::to_string(systemError);
            }
            clearPreparedOwnedProcesses();
            return false;
        }
        m_preparedOwnedProcesses.push_back({
            action.actionId,
            action.process,
            reinterpret_cast<std::uintptr_t>(rawProcess),
            before == WAIT_OBJECT_0,
        });
    }
#else
    (void)actions;
#endif
    return true;
}

void DefaultRollbackExecutor::clearPreparedOwnedProcesses() noexcept {
#if defined(_WIN32)
    for (auto& prepared : m_preparedOwnedProcesses) {
        if (prepared.nativeHandle != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(prepared.nativeHandle));
            prepared.nativeHandle = 0;
        }
    }
#endif
    m_preparedOwnedProcesses.clear();
}

RollbackActionOutcome DefaultRollbackExecutor::terminateOwnedProcess(
    const RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    if (action.kind != RollbackActionKind::TerminateOwnedProcess ||
        action.process.processId == 0 || action.process.creationTime100ns == 0) {
        return makeOutcome(action, RollbackActionResult::InvalidAction);
    }

#if defined(_WIN32)
    HANDLE processHandle = nullptr;
    std::optional<ScopedHandle> openedProcess;
    const auto prepared = std::find_if(
        m_preparedOwnedProcesses.begin(), m_preparedOwnedProcesses.end(),
        [&](const PreparedOwnedProcess& candidate) {
            return candidate.actionId == action.actionId;
        });
    if (prepared != m_preparedOwnedProcesses.end()) {
        if (prepared->process != action.process) {
            return makeOutcome(action, RollbackActionResult::IdentityMismatch);
        }
        if (prepared->alreadySatisfied) {
            return makeOutcome(action, RollbackActionResult::AlreadySatisfied);
        }
        processHandle = reinterpret_cast<HANDLE>(prepared->nativeHandle);
        if (processHandle == nullptr || processHandle == INVALID_HANDLE_VALUE) {
            return makeOutcome(action, RollbackActionResult::Failed,
                               ERROR_INVALID_HANDLE);
        }
    } else {
        const HANDLE rawProcess = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
            FALSE, action.process.processId);
        if (rawProcess == nullptr) {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) {
                return makeOutcome(action, RollbackActionResult::AlreadySatisfied);
            }
            return makeOutcome(action, RollbackActionResult::Failed, error);
        }
        openedProcess.emplace(rawProcess);
        processHandle = rawProcess;
    }

    ProcessIdentity observed;
    std::uint32_t identityError = 0;
    if (!readProcessIdentity(processHandle, action.process.processId,
                             observed, &identityError)) {
        return makeOutcome(action, RollbackActionResult::Failed, identityError);
    }
    if (observed != action.process) {
        return makeOutcome(action, RollbackActionResult::IdentityMismatch);
    }

    const DWORD before = WaitForSingleObject(processHandle, 0);
    if (before == WAIT_OBJECT_0) {
        return makeOutcome(action, RollbackActionResult::AlreadySatisfied);
    }
    if (before == WAIT_FAILED) {
        return makeOutcome(action, RollbackActionResult::Failed, GetLastError());
    }

    constexpr UINT kWatchdogRollbackExitCode = 0x48594452u; // "HYDR".
    if (TerminateProcess(processHandle, kWatchdogRollbackExitCode) == FALSE) {
        const DWORD error = GetLastError();
        const DWORD concurrentWait = WaitForSingleObject(
            processHandle,
            error == ERROR_ACCESS_DENIED ? timeoutMilliseconds : 0u);
        if (concurrentWait == WAIT_OBJECT_0) {
            return makeOutcome(action, RollbackActionResult::AlreadySatisfied);
        }
        if (concurrentWait == WAIT_FAILED) {
            return makeOutcome(action, RollbackActionResult::Failed,
                               GetLastError());
        }
        return makeOutcome(action, RollbackActionResult::Failed, error);
    }

    const DWORD wait = WaitForSingleObject(processHandle, timeoutMilliseconds);
    if (wait == WAIT_OBJECT_0) {
        return makeOutcome(action, RollbackActionResult::Success);
    }
    if (wait == WAIT_TIMEOUT) {
        return makeOutcome(action, RollbackActionResult::TimedOut);
    }
    return makeOutcome(action, RollbackActionResult::Failed, GetLastError());
#else
    (void)timeoutMilliseconds;
    return unsupported(action);
#endif
}

RollbackActionOutcome DefaultRollbackExecutor::closeOwnedSession(
    const RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    (void)timeoutMilliseconds;
    return unsupported(action);
}

RollbackActionOutcome DefaultRollbackExecutor::clearOptionalBackendState(
    const RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    (void)timeoutMilliseconds;
    return unsupported(action);
}

RollbackActionOutcome DefaultRollbackExecutor::releaseOverlayState(
    const RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    (void)timeoutMilliseconds;
    return unsupported(action);
}

RollbackActionOutcome DefaultRollbackExecutor::restoreSnapshotState(
    const RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    (void)timeoutMilliseconds;
    return unsupported(action);
}

RollbackActionOutcome DefaultRollbackExecutor::writeSafeModeResult(
    const RollbackActionDescriptor& action,
    std::uint32_t timeoutMilliseconds) {
    (void)timeoutMilliseconds;
    return unsupported(action);
}

bool queryProcessIdentity(std::uint32_t processId,
                          ProcessIdentity& identity,
                          std::uint32_t* systemError) noexcept {
#if defined(_WIN32)
    if (processId == 0) {
        if (systemError != nullptr) *systemError = ERROR_INVALID_PARAMETER;
        return false;
    }
    const HANDLE rawProcess = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId);
    if (rawProcess == nullptr) {
        const DWORD error = GetLastError();
        if (systemError != nullptr) *systemError = error;
        return false;
    }
    ScopedHandle process(rawProcess);
    return readProcessIdentity(process.get(), processId, identity, systemError);
#else
    (void)processId;
    identity = {};
    if (systemError != nullptr) *systemError = 0;
    return false;
#endif
}

std::string_view rollbackActionResultName(RollbackActionResult result) noexcept {
    switch (result) {
    case RollbackActionResult::Success:
        return "success";
    case RollbackActionResult::AlreadySatisfied:
        return "already-satisfied";
    case RollbackActionResult::InvalidAction:
        return "invalid-action";
    case RollbackActionResult::IdentityMismatch:
        return "identity-mismatch";
    case RollbackActionResult::Unsupported:
        return "unsupported";
    case RollbackActionResult::TimedOut:
        return "timed-out";
    case RollbackActionResult::Failed:
        return "failed";
    }
    return "unknown";
}

} // namespace hydra::watchdog
