#include "hydra/acceptance_probe.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testReportEncodingBindsInstalledIdentity() {
    hydra::acceptance::AcceptanceProbeReport report;
    report.releaseVersion = "1.2.3";
    report.releaseRevision = 17u;
    report.commitSha = std::string(40u, 'a');
    report.architecture = "x64";
    report.installStateSha256 = std::string(64u, 'b');
    report.allOwnedFilesVerified = true;
    const auto json = hydra::acceptance::encodeAcceptanceProbeJson(report);
    check(json.find("\"release_version\":\"1.2.3\"") != std::string::npos &&
              json.find("\"release_revision\":17") != std::string::npos &&
              json.find("\"commit_sha\":\"" + std::string(40u, 'a') + "\"") !=
                  std::string::npos &&
              json.find("\"architecture\":\"x64\"") != std::string::npos &&
              json.find("\"install_state_sha256\":\"" + std::string(64u, 'b') + "\"") !=
                  std::string::npos,
          "encoded probe evidence carries exact installed release identity");
}

#if defined(_WIN32)

struct Process {
    PROCESS_INFORMATION info{};
    ~Process() {
        if (info.hProcess) {
            (void)TerminateProcess(info.hProcess, 0u);
            (void)WaitForSingleObject(info.hProcess, 5000u);
            CloseHandle(info.hProcess);
        }
        if (info.hThread) CloseHandle(info.hThread);
    }
};

bool launch(const std::filesystem::path& executable, Process& process) {
    std::wstring command = L"\"" + executable.wstring() +
                           L"\" --depth 0 --sleep-ms 15000";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    return CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr,
                          FALSE, CREATE_NO_WINDOW, nullptr,
                          executable.parent_path().c_str(), &startup,
                          &process.info) != FALSE;
}

void writeState(const std::filesystem::path& path,
                const std::filesystem::path& installRoot,
                std::string_view hash,
                bool unknownField = false) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "{\"schemaVersion\":1,\"releaseVersion\":\"1.0.0\","
              "\"releaseRevision\":1,\"commitSha\":\""
           << std::string(40u, 'a') << "\",\"architecture\":\"x64\","
              "\"installRoot\":\"";
    auto root = installRoot.generic_u8string();
    for (const auto ch : root) {
        if (ch == static_cast<char8_t>('\\') || ch == static_cast<char8_t>('/')) output << "\\\\";
        else output << static_cast<char>(ch);
    }
    output << "\",\"startupMode\":\"Manual\",\"ownedFiles\":[{"
              "\"fileName\":\"owned.exe\",\"sha256\":\""
           << hash << "\"}]";
    if (unknownField) output << ",\"unexpected\":true";
    output << '}';
}

void testExactPathAndCreationIdentity(const std::filesystem::path& child) {
    const auto root = std::filesystem::temp_directory_path() /
        ("hydraseat-probe-process-" + std::to_string(GetCurrentProcessId()));
    const auto installed = root / "installed";
    const auto unrelated = root / "unrelated";
    std::error_code error;
    std::filesystem::create_directories(installed, error);
    std::filesystem::create_directories(unrelated, error);
    const auto owned = installed / "owned.exe";
    const auto decoy = unrelated / "owned.exe";
    std::filesystem::copy_file(child, owned,
                               std::filesystem::copy_options::overwrite_existing, error);
    std::filesystem::copy_file(child, decoy,
                               std::filesystem::copy_options::overwrite_existing, error);
    const auto hash = hydra::acceptance::sha256FileHex(owned);
    const auto state = root / "install-state.json";
    writeState(state, installed, hash);

    Process ownedProcess;
    Process decoyProcess;
    check(launch(owned, ownedProcess) && launch(decoy, decoyProcess),
          "controlled owned and same-name unrelated processes launch");
    (void)WaitForInputIdle(ownedProcess.info.hProcess, 100u);

    hydra::acceptance::InstalledReleaseClaim claim;
    std::string stateHash;
    check(hydra::acceptance::loadInstalledReleaseClaim(state, claim, &stateHash).succeeded(),
          "strict installed-state claim loads");
    hydra::acceptance::AcceptanceProbeReport report;
    const auto result = hydra::acceptance::runAcceptanceProbe(
        claim, stateHash, true, 2u, 20u, report);
    check(result.succeeded(), "read-only development probe succeeds");
    check(report.releaseVersion == claim.releaseVersion &&
              report.releaseRevision == claim.releaseRevision &&
              report.commitSha == claim.commitSha &&
              report.architecture == claim.architecture &&
              report.installStateSha256 == stateHash,
          "probe evidence binds exact installed release revision, architecture, commit, and state hash");
    check(report.developmentUnsignedAllowed && report.allOwnedFilesVerified,
          "development exception remains explicit and exact-hash verified");
    check(report.runningOwnedProcesses.size() == 1u,
          "same-name process outside exact install path is excluded");
    if (report.runningOwnedProcesses.size() == 1u) {
        check(report.runningOwnedProcesses[0].processId == ownedProcess.info.dwProcessId &&
                  report.runningOwnedProcesses[0].creationTime100ns != 0u &&
                  report.runningOwnedProcesses[0].samples >= 1u,
              "owned process is recorded by PID plus creation identity and bounded samples");
    }

    const auto prior = claim;
    writeState(state, installed, hash, true);
    check(hydra::acceptance::loadInstalledReleaseClaim(state, claim).code ==
              hydra::acceptance::ProbeCode::StateDecodeFailed && claim == prior,
          "unknown install-state field fails transactionally");
}

#endif

} // namespace

int main(int argc, char** argv) {
    testReportEncodingBindsInstalledIdentity();
#if defined(_WIN32)
    if (argc != 2) {
        std::cerr << "usage: acceptance_probe_process_tests <controlled-child.exe>\n";
        return EXIT_FAILURE;
    }
    testExactPathAndCreationIdentity(std::filesystem::path(argv[1]));
#else
    (void)argc; (void)argv;
    hydra::acceptance::InstalledReleaseClaim claim;
    check(hydra::acceptance::loadInstalledReleaseClaim("missing", claim).code ==
              hydra::acceptance::ProbeCode::UnsupportedPlatform,
          "non-Windows probe fails closed");
#endif
    if (failures != 0) {
        std::cerr << failures << " acceptance probe process test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Acceptance probe process tests passed.\n";
    return EXIT_SUCCESS;
}
