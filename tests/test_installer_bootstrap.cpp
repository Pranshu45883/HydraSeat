#include "hydra/installer_bootstrap.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

hydra::installer::BootstrapPackageFacts validFacts() {
    using namespace hydra::installer;
    BootstrapPackageFacts facts;
    facts.architectureDirectoryName = L"x64";
    facts.packageRoot = BootstrapPathType::Directory;
    facts.architectureDirectory = BootstrapPathType::Directory;
    facts.setupExecutable = BootstrapPathType::RegularFile;
    facts.installerScript = BootstrapPathType::RegularFile;
    facts.signingProvenance = BootstrapPathType::RegularFile;
    facts.signingProvenanceSignature = BootstrapPathType::RegularFile;
    facts.packageRootEntries = expectedBootstrapPackageRootEntries();
    facts.architectureEntries = expectedBootstrapArchitectureFiles();
    return facts;
}

void testFixedOperationMapping() {
    using namespace hydra::installer;
    check(bootstrapPowerShellMode(BootstrapOperation::Install) == L"Install",
          "Install maps to one fixed PowerShell mode");
    check(bootstrapPowerShellMode(BootstrapOperation::Repair) == L"Repair",
          "Repair maps to one fixed PowerShell mode");
    check(bootstrapPowerShellMode(BootstrapOperation::Uninstall) == L"Uninstall",
          "Uninstall maps to one fixed PowerShell mode");
    check(bootstrapPowerShellMode(BootstrapOperation::Validate) == L"Validate",
          "Validate maps to one fixed read-only PowerShell mode");
}

void testWindowsArgumentQuoting() {
    using hydra::installer::quoteWindowsCommandLineArgument;
    check(quoteWindowsCommandLineArgument(L"simple") == L"simple",
          "simple command argument does not gain unnecessary quoting");
    check(quoteWindowsCommandLineArgument(L"") == L"\"\"",
          "empty argument is represented explicitly");
    check(quoteWindowsCommandLineArgument(L"C:\\Hydra Seat\\pkg") ==
              L"\"C:\\Hydra Seat\\pkg\"",
          "path spaces are quoted without shell concatenation");
    check(quoteWindowsCommandLineArgument(L"C:\\path with space\\") ==
              L"\"C:\\path with space\\\\\"",
          "trailing backslash is doubled before the closing quote");
    const auto embeddedQuote = quoteWindowsCommandLineArgument(L"a\\\"b");
    const std::wstring expectedEmbeddedQuote =
        L"\"a" + std::wstring(3, L'\\') + L"\"b\"";
    check(embeddedQuote == expectedEmbeddedQuote,
          "embedded quote receives the Windows backslash escaping rule");
}

void testTypedPowerShellInvocation() {
    using namespace hydra::installer;
#ifdef _WIN32
    const std::filesystem::path powershell =
        L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    const std::filesystem::path script =
        L"C:\\Hydra Seat Package\\x64\\install_hydraseat.ps1";
    const std::filesystem::path packageRoot = L"C:\\Hydra Seat Package";
#else
    const std::filesystem::path powershell =
        "/Windows/System32/WindowsPowerShell/v1.0/powershell.exe";
    const std::filesystem::path script =
        "/Hydra Seat Package/x64/install_hydraseat.ps1";
    const std::filesystem::path packageRoot = "/Hydra Seat Package";
#endif

    const BootstrapReleaseIdentity expectedRelease{L"1.2.3-test", 42};
    BootstrapPowerShellInvocation invocation;
    std::wstring error;
    check(makeBootstrapPowerShellInvocation(
              BootstrapOperation::Install, powershell, script, packageRoot,
              expectedRelease, false, invocation, &error),
          "Install invocation is constructible from typed reviewed inputs");
    check(invocation.requestElevation,
          "Install explicitly requests elevation only at mutation time");
    check(invocation.executable == powershell,
          "invocation launches the exact supplied system PowerShell executable");
    const std::wstring expectedInstall =
        L"-NoLogo -NoProfile -NonInteractive -ExecutionPolicy AllSigned -File " +
        quoteWindowsCommandLineArgument(script.wstring()) + L" -Mode Install -PackageRoot " +
        quoteWindowsCommandLineArgument(packageRoot.wstring()) +
        L" -ExpectedReleaseVersion 1.2.3-test -ExpectedReleaseRevision 42";
    check(invocation.parameters == expectedInstall,
          "Install arguments contain only the fixed signed-script contract");
    check(invocation.parameters.find(L"cmd.exe") == std::wstring::npos &&
              invocation.parameters.find(L"-Command") == std::wstring::npos,
          "bootstrap invocation cannot become a general command runner");

    check(makeBootstrapPowerShellInvocation(
              BootstrapOperation::Validate, powershell, script, packageRoot,
              std::nullopt, false, invocation, &error) && !invocation.requestElevation &&
              invocation.parameters.find(L"-Mode Validate") != std::wstring::npos,
          "Validate uses the same fixed script without UAC");

    check(makeBootstrapPowerShellInvocation(
              BootstrapOperation::Uninstall, powershell, script, std::nullopt,
              std::nullopt, true, invocation, &error) && invocation.requestElevation &&
              invocation.parameters.find(L"-Mode Uninstall") != std::wstring::npos &&
              invocation.parameters.find(L"-RemoveHydraSeatUserData") != std::wstring::npos &&
              invocation.parameters.find(L"-PackageRoot") == std::wstring::npos,
          "Uninstall has a fixed optional owned-user-data flag and needs no package root");

    check(!makeBootstrapPowerShellInvocation(
              BootstrapOperation::Repair, powershell, script, std::nullopt,
              expectedRelease, false, invocation, &error),
          "Repair fails closed without an exact package root");
    check(!makeBootstrapPowerShellInvocation(
              BootstrapOperation::Install, powershell, script, packageRoot,
              expectedRelease, true, invocation, &error),
          "Install cannot smuggle the uninstall-only user-data mutation flag");
    check(!makeBootstrapPowerShellInvocation(
              BootstrapOperation::Install, powershell, script, packageRoot,
              std::nullopt, false, invocation, &error),
          "Install cannot proceed without the exact user-confirmed release identity");
    const BootstrapReleaseIdentity unsafeRelease{L"1.2 bad", 42};
    check(!makeBootstrapPowerShellInvocation(
              BootstrapOperation::Install, powershell, script, packageRoot,
              unsafeRelease, false, invocation, &error),
          "unsafe expected-release version text is rejected before command construction");
    check(!makeBootstrapPowerShellInvocation(
              BootstrapOperation::Install, L"powershell.exe", script, packageRoot,
              expectedRelease, false, invocation, &error),
          "PATH-resolved PowerShell is rejected in favor of an exact absolute executable");
}

void testExactPackageLayoutContract() {
    using namespace hydra::installer;
    auto facts = validFacts();
    const auto valid = assessBootstrapPackageFacts(facts);
    check(valid.valid(), "exact reviewed package layout is accepted");

    auto missingScript = facts;
    missingScript.installerScript = BootstrapPathType::Missing;
    check(assessBootstrapPackageFacts(missingScript).status ==
              BootstrapPackageStatus::MissingRequiredPath,
          "missing adjacent installer script is rejected");

    auto reparseScript = facts;
    reparseScript.installerScript = BootstrapPathType::ReparsePoint;
    check(assessBootstrapPackageFacts(reparseScript).status ==
              BootstrapPackageStatus::ReparsePointRejected,
          "reparse-point installer script is rejected before execution");

    auto wrongDirectory = facts;
    wrongDirectory.architectureDirectoryName = L"Release";
    check(assessBootstrapPackageFacts(wrongDirectory).status ==
              BootstrapPackageStatus::NotReleaseLayout,
          "setup refuses an unexpected package-directory shape");

    auto unexpectedArchitectureFile = facts;
    unexpectedArchitectureFile.architectureEntries.push_back(L"debug-helper.exe");
    check(assessBootstrapPackageFacts(unexpectedArchitectureFile).status ==
              BootstrapPackageStatus::UnexpectedLayout,
          "extra architecture payload is rejected");

    auto missingBootstrap = facts;
    auto& files = missingBootstrap.architectureEntries;
    files.erase(std::find(files.begin(), files.end(), L"HydraSeatSetup.exe"));
    check(assessBootstrapPackageFacts(missingBootstrap).status ==
              BootstrapPackageStatus::UnexpectedLayout,
          "package missing the reviewed setup bootstrapper is rejected");

    auto unexpectedRoot = facts;
    unexpectedRoot.packageRootEntries.push_back(L"unsigned-notes.txt");
    check(assessBootstrapPackageFacts(unexpectedRoot).status ==
              BootstrapPackageStatus::UnexpectedLayout,
          "package root refuses unreviewed extra payload");
}

void testResultMapping() {
    using namespace hydra::installer;
    check(mapBootstrapProcessResult(0) == BootstrapProcessResult::Success,
          "zero installer exit maps to success");
    check(mapBootstrapProcessResult(1223) == BootstrapProcessResult::Cancelled,
          "Windows UAC cancellation maps to cancelled");
    check(mapBootstrapProcessResult(1602) == BootstrapProcessResult::Cancelled,
          "standard installer user-exit code maps to cancelled");
    check(mapBootstrapProcessResult(1) == BootstrapProcessResult::Failed &&
              mapBootstrapProcessResult(5) == BootstrapProcessResult::Failed,
          "nonzero failure exits never become success");
}

} // namespace

int main() {
    testFixedOperationMapping();
    testWindowsArgumentQuoting();
    testTypedPowerShellInvocation();
    testExactPackageLayoutContract();
    testResultMapping();
    std::cout << "Installer bootstrap contract tests passed.\n";
    return EXIT_SUCCESS;
}
