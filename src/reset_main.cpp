#include "hydra/hidhide_session_recovery.hpp"
#include "hydra/reset_actions.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitRecoveryRequired = 2;
constexpr int kExitUsage = 3;
constexpr int kExitOperationFailed = 4;

struct Options {
    std::string command;
    std::vector<std::string> commandArguments;
    std::optional<std::filesystem::path> recoveryDirectory;
    bool json{false};
};

std::string jsonEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8u);
    for (const char ch : value) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20u) {
                result += '?';
            } else {
                result += ch;
            }
            break;
        }
    }
    return result;
}

std::string actionKindName(hydra::watchdog::RollbackActionKind kind) {
    using hydra::watchdog::RollbackActionKind;
    switch (kind) {
    case RollbackActionKind::TerminateOwnedProcess: return "terminate-owned-process";
    case RollbackActionKind::CloseOwnedSession: return "close-owned-session";
    case RollbackActionKind::ClearOptionalBackendState: return "clear-optional-backend-state";
    case RollbackActionKind::ReleaseOverlayState: return "release-overlay-state";
    case RollbackActionKind::RestoreSnapshotState: return "restore-snapshot-state";
    case RollbackActionKind::WriteSafeModeResult: return "write-safe-mode-result";
    }
    return "unknown";
}

std::vector<const hydra::watchdog::RollbackActionDescriptor*> orderedRollbackActions(
    const hydra::watchdog::RollbackPlanManifest& manifest) {
    std::vector<const hydra::watchdog::RollbackActionDescriptor*> ordered;
    ordered.reserve(manifest.actions.size());
    for (const auto& action : manifest.actions) ordered.push_back(&action);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->activationOrdinal > right->activationOrdinal;
    });
    return ordered;
}

void printUsage() {
    std::cout
        << "HydraSeat emergency reset CLI\n\n"
        << "Usage:\n"
        << "  hydra_reset status [--json] [--recovery-dir <path>]\n"
        << "  hydra_reset dry-run [--json] [--recovery-dir <path>]\n"
        << "  hydra_reset session <32-hex-session-id> [--json] [--recovery-dir <path>]\n"
        << "  hydra_reset all --confirm [--json] [--recovery-dir <path>]\n"
        << "  hydra_reset safe-mode on|off [--json] [--recovery-dir <path>]\n"
        << "  hydra_reset export-diagnostics [--recovery-dir <path>]\n\n"
        << "The reset tool never performs process-name kills or arbitrary file/registry deletion.\n";
}

std::optional<Options> parseOptions(int argc, char** argv) {
    if (argc < 2) return std::nullopt;
    Options options;
    options.command = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string_view arg{argv[index]};
        if (arg == "--json") {
            options.json = true;
        } else if (arg == "--recovery-dir") {
            if (index + 1 >= argc) return std::nullopt;
            options.recoveryDirectory = std::filesystem::path(argv[++index]);
        } else {
            options.commandArguments.emplace_back(arg);
        }
    }
    return options;
}

std::string safeModeText(const hydra::reset::ResetInspection& inspection) {
    if (!inspection.safeMode) return "off";
    return std::string(hydra::recovery::safeModeReasonName(inspection.safeMode->reason));
}

void printInspectionHuman(const hydra::reset::ResetInspection& inspection) {
    std::cout << "state=" << hydra::reset::resetStateName(inspection.state) << '\n';
    if (inspection.journal) {
        std::cout << "session=" << hydra::reset::sessionIdHex(inspection.journal->sessionId)
                  << "\ngeneration=" << inspection.journal->runtimeGeneration
                  << "\nphase=" << hydra::recovery::crashJournalPhaseName(inspection.journal->phase)
                  << '\n';
    } else {
        std::cout << "session=none\ngeneration=0\nphase=none\n";
    }
    std::cout << "safe_mode=" << safeModeText(inspection) << '\n'
              << "runtime_registration="
              << (inspection.registration ? "present" : "absent") << '\n'
              << "diagnostic=" << inspection.diagnostic << '\n';
}

void printInspectionJson(const hydra::reset::ResetInspection& inspection) {
    std::cout << "{"
              << "\"schema_version\":1,"
              << "\"state\":\"" << hydra::reset::resetStateName(inspection.state) << "\","
              << "\"diagnostic\":\"" << jsonEscape(inspection.diagnostic) << "\","
              << "\"safe_mode\":\"" << jsonEscape(safeModeText(inspection)) << "\","
              << "\"runtime_registration\":"
              << (inspection.registration ? "true" : "false");
    if (inspection.journal) {
        std::cout << ",\"session\":\""
                  << hydra::reset::sessionIdHex(inspection.journal->sessionId) << "\""
                  << ",\"runtime_generation\":" << inspection.journal->runtimeGeneration
                  << ",\"phase\":\""
                  << hydra::recovery::crashJournalPhaseName(inspection.journal->phase)
                  << "\"";
    } else {
        std::cout << ",\"session\":null,\"runtime_generation\":0,\"phase\":null";
    }
    std::cout << "}\n";
}

void printDryRunHuman(const hydra::reset::ResetInspection& inspection) {
    printInspectionHuman(inspection);
    if (!inspection.registration) {
        std::cout << "actions=none\n";
        return;
    }
    const auto& registration = *inspection.registration;
    std::cout << "owner_exact_pid=" << registration.ownerProcess.processId
              << " owner_creation_time_100ns="
              << registration.ownerProcess.creationTime100ns << '\n';
    for (const auto* action : orderedRollbackActions(registration.manifest)) {
        std::cout << "action id=" << action->actionId
                  << " kind=" << actionKindName(action->kind)
                  << " ordinal=" << action->activationOrdinal
                  << " generation=" << action->generation;
        if (action->process.processId != 0) {
            std::cout << " pid=" << action->process.processId
                      << " creation_time_100ns=" << action->process.creationTime100ns;
        } else if (action->resourceId != 0) {
            std::cout << " resource_id=" << action->resourceId;
        }
        std::cout << '\n';
    }
}

void printDryRunJson(const hydra::reset::ResetInspection& inspection) {
    std::cout << "{\"schema_version\":1,\"state\":\""
              << hydra::reset::resetStateName(inspection.state)
              << "\",\"diagnostic\":\"" << jsonEscape(inspection.diagnostic)
              << "\",\"actions\":[";
    bool first = true;
    if (inspection.registration) {
        const auto& owner = inspection.registration->ownerProcess;
        std::cout << "{\"role\":\"runtime-owner\",\"kind\":\"terminate-owned-process\","
                  << "\"pid\":" << owner.processId
                  << ",\"creation_time_100ns\":" << owner.creationTime100ns << "}";
        first = false;
        for (const auto* action : orderedRollbackActions(
                 inspection.registration->manifest)) {
            if (!first) std::cout << ',';
            first = false;
            std::cout << "{\"role\":\"rollback\",\"action_id\":" << action->actionId
                      << ",\"kind\":\"" << actionKindName(action->kind) << "\""
                      << ",\"activation_ordinal\":" << action->activationOrdinal
                      << ",\"generation\":" << action->generation
                      << ",\"pid\":" << action->process.processId
                      << ",\"creation_time_100ns\":" << action->process.creationTime100ns
                      << ",\"resource_id\":" << action->resourceId << "}";
        }
    }
    std::cout << "]}\n";
}

void printExecutionHuman(const hydra::reset::ResetExecutionReport& report) {
    std::cout << "success=" << (report.success ? "true" : "false") << '\n'
              << "no_op=" << (report.noOp ? "true" : "false") << '\n'
              << "owner_satisfied=" << (report.ownerSatisfied ? "true" : "false") << '\n'
              << "rollback_satisfied=" << (report.rollbackSatisfied ? "true" : "false") << '\n'
              << "journal_clean=" << (report.journalClean ? "true" : "false") << '\n'
              << "safe_mode_cleared=" << (report.safeModeCleared ? "true" : "false") << '\n'
              << "registration_cleared=" << (report.registrationCleared ? "true" : "false") << '\n'
              << "diagnostic=" << report.diagnostic << '\n';
    if (report.ownerOutcome) {
        std::cout << "owner_result id=" << report.ownerOutcome->actionId
                  << " kind=" << actionKindName(report.ownerOutcome->kind)
                  << " result="
                  << hydra::watchdog::rollbackActionResultName(report.ownerOutcome->result)
                  << " system_error=" << report.ownerOutcome->systemError << '\n';
    }
    for (const auto& outcome : report.rollback.outcomes) {
        std::cout << "action_result id=" << outcome.actionId
                  << " kind=" << actionKindName(outcome.kind)
                  << " result=" << hydra::watchdog::rollbackActionResultName(outcome.result)
                  << " system_error=" << outcome.systemError << '\n';
    }
}

void printExecutionJson(const hydra::reset::ResetExecutionReport& report) {
    std::cout << "{\"schema_version\":1,"
              << "\"success\":" << (report.success ? "true" : "false") << ','
              << "\"no_op\":" << (report.noOp ? "true" : "false") << ','
              << "\"owner_satisfied\":" << (report.ownerSatisfied ? "true" : "false") << ','
              << "\"rollback_satisfied\":" << (report.rollbackSatisfied ? "true" : "false") << ','
              << "\"journal_clean\":" << (report.journalClean ? "true" : "false") << ','
              << "\"safe_mode_cleared\":" << (report.safeModeCleared ? "true" : "false") << ','
              << "\"registration_cleared\":" << (report.registrationCleared ? "true" : "false") << ','
              << "\"diagnostic\":\"" << jsonEscape(report.diagnostic) << "\","
              << "\"before_state\":\"" << hydra::reset::resetStateName(report.before.state) << "\","
              << "\"after_state\":\"" << hydra::reset::resetStateName(report.after.state) << "\","
              << "\"owner_action\":";
    if (report.ownerOutcome) {
        std::cout << "{\"action_id\":" << report.ownerOutcome->actionId
                  << ",\"kind\":\"" << actionKindName(report.ownerOutcome->kind) << "\""
                  << ",\"result\":\""
                  << hydra::watchdog::rollbackActionResultName(report.ownerOutcome->result) << "\""
                  << ",\"system_error\":" << report.ownerOutcome->systemError << "}";
    } else {
        std::cout << "null";
    }
    std::cout << ",\"actions\":[";
    bool first = true;
    for (const auto& outcome : report.rollback.outcomes) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"action_id\":" << outcome.actionId
                  << ",\"kind\":\"" << actionKindName(outcome.kind) << "\""
                  << ",\"result\":\""
                  << hydra::watchdog::rollbackActionResultName(outcome.result) << "\""
                  << ",\"system_error\":" << outcome.systemError << "}";
    }
    std::cout << "]}\n";
}

int stateExitCode(hydra::reset::ResetState state) {
    return state == hydra::reset::ResetState::RecoveryRequired
        ? kExitRecoveryRequired
        : kExitOk;
}

} // namespace

int main(int argc, char** argv) {
    const auto parsed = parseOptions(argc, argv);
    if (!parsed || parsed->command == "--help" || parsed->command == "-h" ||
        parsed->command == "help") {
        printUsage();
        return parsed ? kExitOk : kExitUsage;
    }
    const Options& options = *parsed;

    std::filesystem::path recoveryDirectory;
    if (options.recoveryDirectory) {
        recoveryDirectory = *options.recoveryDirectory;
    } else {
        std::uint32_t systemError = 0;
        const auto defaultDirectory =
            hydra::recovery::defaultCrashJournalDirectory(&systemError);
        if (!defaultDirectory) {
            std::cerr << "Unable to resolve the HydraSeat recovery directory; system_error="
                      << systemError << '\n';
            return kExitOperationFailed;
        }
        recoveryDirectory = *defaultDirectory;
    }

    hydra::recovery::NativeCrashJournalStorage storage(recoveryDirectory);
    hydra::recovery::CrashJournalStore journalStore(storage);
    hydra::reset::RuntimeResetRegistrationStore registrationStore(recoveryDirectory);

    if (options.command == "status") {
        if (!options.commandArguments.empty()) {
            printUsage();
            return kExitUsage;
        }
        const auto inspection =
            hydra::reset::inspectResetState(journalStore, registrationStore);
        if (options.json) printInspectionJson(inspection);
        else printInspectionHuman(inspection);
        return stateExitCode(inspection.state);
    }

    if (options.command == "dry-run") {
        if (!options.commandArguments.empty()) {
            printUsage();
            return kExitUsage;
        }
        const auto inspection =
            hydra::reset::inspectResetState(journalStore, registrationStore);
        if (options.json) printDryRunJson(inspection);
        else printDryRunHuman(inspection);
        return stateExitCode(inspection.state);
    }

    if (options.command == "export-diagnostics") {
        if (!options.commandArguments.empty()) {
            printUsage();
            return kExitUsage;
        }
        const auto inspection =
            hydra::reset::inspectResetState(journalStore, registrationStore);
        printDryRunJson(inspection);
        return stateExitCode(inspection.state);
    }

    if (options.command == "safe-mode") {
        if (options.commandArguments.size() != 1u ||
            (options.commandArguments[0] != "on" &&
             options.commandArguments[0] != "off")) {
            printUsage();
            return kExitUsage;
        }
        std::string error;
        const bool enabled = options.commandArguments[0] == "on";
        const bool ok = enabled
            ? hydra::reset::enableManualSafeMode(journalStore, &error)
            : hydra::reset::disableManualSafeMode(storage, journalStore, &error);
        const auto inspection =
            hydra::reset::inspectResetState(journalStore, registrationStore);
        if (options.json) {
            std::cout << "{\"schema_version\":1,\"success\":"
                      << (ok ? "true" : "false")
                      << ",\"operation\":\"safe-mode-"
                      << (enabled ? "on" : "off") << "\",\"diagnostic\":\""
                      << jsonEscape(ok ? inspection.diagnostic : error) << "\"}\n";
        } else if (ok) {
            printInspectionHuman(inspection);
        } else {
            std::cerr << "safe-mode operation failed: " << error << '\n';
        }
        return ok ? kExitOk : kExitOperationFailed;
    }

    std::optional<hydra::watchdog::SessionId> expectedSession;
    if (options.command == "session") {
        if (options.commandArguments.size() != 1u) {
            printUsage();
            return kExitUsage;
        }
        std::string error;
        expectedSession = hydra::reset::parseSessionIdHex(
            options.commandArguments.front(), &error);
        if (!expectedSession) {
            std::cerr << "Invalid session id: " << error << '\n';
            return kExitUsage;
        }
    } else if (options.command == "all") {
        if (options.commandArguments.size() != 1u ||
            options.commandArguments.front() != "--confirm") {
            std::cerr << "The all command requires the explicit --confirm switch.\n";
            return kExitUsage;
        }
    } else {
        printUsage();
        return kExitUsage;
    }

    hydra::HidHideSessionRollbackExecutor executor(
        recoveryDirectory, hydra::makeNativeHidHideSessionPlatform());
    const auto report = hydra::reset::executeVerifiedReset(
        journalStore, registrationStore, executor, expectedSession);
    if (options.json) printExecutionJson(report);
    else printExecutionHuman(report);
    return report.success ? kExitOk : kExitRecoveryRequired;
}
