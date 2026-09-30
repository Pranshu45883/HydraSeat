#include "hydra/setup_package.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace hydra;
using namespace hydra::portable;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

profile::CompatibilityReference compatibility() {
    return {"compat-portable", "fixture", 2u};
}

profile::GameRecord game() {
    profile::GameRecord value;
    value.gameId = "game:portable";
    value.providerId = "fake";
    value.providerAppId = "300";
    value.title = L"Portable Fixture";
    value.installRoot = L"C:\\Games\\Portable";
    value.executableCandidates = {L"C:\\Games\\Portable\\game.exe"};
    value.compatibility = compatibility();
    value.origin = profile::GameOrigin::Discovered;
    return value;
}

profile::TwoPlayerSetup setup() {
    profile::TwoPlayerSetup value;
    value.setupId = "setup-portable";
    value.gameId = "game:portable";
    value.displayName = L"Portable two player";
    value.compatibility = compatibility();
    value.instances = {
        {{L"--seat=1", L"--profile", L"alpha"},
         L"C:\\FixtureSource\\게임1", L"C:\\FixtureSource\\데이터1"},
        {{L"--seat=2", L"--profile", L"beta", L"--windowed"},
         L"C:\\FixtureSource\\게임2", L"C:\\FixtureSource\\데이터2"},
    };
    return value;
}

std::vector<PathBinding> remap() {
    return {
        {"WORKING_DIRECTORY_0", L"D:\\게임\\Portable1"},
        {"DATA_ROOT_0", L"D:\\HydraSeat\\데이터1"},
        {"WORKING_DIRECTORY_1", L"D:\\게임\\Portable2"},
        {"DATA_ROOT_1", L"D:\\HydraSeat\\데이터2"},
    };
}

std::vector<PortableInstanceMaterialization> materializations() {
    PortableInstanceMaterialization first;
    first.instanceIndex = 0u;
    first.steps = {
        {"copy-pre", setup::RecipeExecutionPhase::PreSpawn,
         setup::MutationScope::SeatWritableInstance,
         {{L"defaults/기본.ini", L"config/좌석1.ini", 4096u}}},
        {"copy-startup", setup::RecipeExecutionPhase::Startup,
         setup::MutationScope::SeatWritableInstance,
         {{L"defaults/startup.ini", L"state/startup.ini", 8192u}}},
    };

    PortableInstanceMaterialization second;
    second.instanceIndex = 1u;
    second.steps = {
        {"copy-window", setup::RecipeExecutionPhase::PostWindow,
         setup::MutationScope::SeatWritableInstance,
         {{L"defaults/window.ini", L"config/좌석2.ini", 4096u}}},
        {"copy-runtime", setup::RecipeExecutionPhase::Runtime,
         setup::MutationScope::SeatWritableInstance,
         {{L"defaults/runtime.ini", L"state/runtime.ini", 8192u}}},
    };
    return {first, second};
}

SetupPackage exported(bool withMaterializations = true) {
    SetupPackage package;
    const auto descriptors = materializations();
    const auto diagnostic = withMaterializations
        ? exportSetup(setup(), game(), {"community-fixture", 7u, "hydraseat-test"},
                      descriptors, package)
        : exportSetup(setup(), game(), {"community-fixture", 7u, "hydraseat-test"}, package);
    check(diagnostic.succeeded(), "setup package fixture exports successfully");
    return package;
}

void testCurrentRoundTripPreservesEverySupportedSemantic() {
    const auto sourceSetup = setup();
    const auto sourceMaterializations = materializations();
    SetupPackage package;
    check(exportSetup(sourceSetup, game(), {"community-fixture", 7u, "hydraseat-test"},
                      sourceMaterializations, package).succeeded(),
          "current setup package exports typed materialization semantics");
    check(package.version == kSetupPackageVersion && kSetupPackageVersion == 2u,
          "current package schema is version 2");
    check(package.pathVariables.size() == 4u,
          "all machine-specific working/data paths become typed variables");
    check(package.redactedSetup.instances[0].dataRoot == L"${DATA_ROOT_0}" &&
              package.redactedSetup.instances[1].workingDirectory == L"${WORKING_DIRECTORY_1}",
          "portable setup carries placeholders rather than source absolute paths");
    check(package.redactedSetup.instances[0].arguments == sourceSetup.instances[0].arguments &&
              package.redactedSetup.instances[1].arguments == sourceSetup.instances[1].arguments,
          "two distinct instance argument arrays are preserved exactly during export");
    check(package.redactedSetup.compatibility == sourceSetup.compatibility,
          "compatibility reference record/provenance/revision is preserved exactly");
    check(package.instanceMaterializations == sourceMaterializations,
          "phase/scope/relative-file materialization descriptors are preserved at export");

    std::string firstEncoding;
    std::string secondEncoding;
    check(encodePackage(package, firstEncoding).succeeded() &&
              encodePackage(package, secondEncoding).succeeded() &&
              firstEncoding == secondEncoding,
          "current package encoding is deterministic");
    check(firstEncoding.find("소스") == std::string::npos,
          "encoded package redacts source-machine Unicode absolute paths");

    SetupPackage decoded;
    check(decodePackage(firstEncoding, decoded).succeeded() && decoded == package,
          "current package decodes to the identical typed package");
    std::string reencoded;
    check(encodePackage(decoded, reencoded).succeeded() && reencoded == firstEncoding,
          "decode then encode is byte-stable");

    ImportedSetup imported;
    check(importSetup(decoded, game(), remap(), imported).succeeded(),
          "typed current package imports after explicit local path remapping");
    check(imported.setup.instances[0].arguments == sourceSetup.instances[0].arguments &&
              imported.setup.instances[1].arguments == sourceSetup.instances[1].arguments,
          "typed import preserves both distinct argument arrays exactly");
    check(imported.setup.instances[0].workingDirectory == L"D:\\게임\\Portable1" &&
              imported.setup.instances[1].workingDirectory == L"D:\\게임\\Portable2",
          "typed import preserves both explicit Unicode working-directory bindings");
    check(imported.setup.instances[0].dataRoot == L"D:\\HydraSeat\\데이터1" &&
              imported.setup.instances[1].dataRoot == L"D:\\HydraSeat\\데이터2",
          "typed import preserves both explicit Unicode data-root bindings");
    check(imported.setup.compatibility == sourceSetup.compatibility,
          "typed import preserves compatibility identity without changing its revision");
    check(imported.instanceMaterializations == sourceMaterializations,
          "typed import preserves all four declared lifecycle phases/materialization descriptors");
    check(setup::validateSetup(imported.setup, game()).succeeded(),
          "round-tripped local setup passes normal local setup validation");

    profile::TwoPlayerSetup legacyOutput;
    legacyOutput.setupId = "sentinel";
    const auto sentinel = legacyOutput;
    check(importSetup(decoded, game(), remap(), legacyOutput).result ==
              PackageResult::UnsupportedSemantic &&
              legacyOutput == sentinel,
          "legacy setup-only import rejects v2 materialization instead of silently dropping it");
}

void testBindingsAndTraversalFailTransactionally() {
    const auto package = exported(false);
    profile::TwoPlayerSetup output;
    output.setupId = "sentinel";
    const auto sentinel = output;

    auto bindings = remap();
    bindings.pop_back();
    check(importSetup(package, game(), bindings, output).result == PackageResult::MissingBinding &&
              output == sentinel,
          "missing local remap leaves previous output unchanged");

    bindings = remap();
    bindings.push_back({"UNDECLARED", L"D:\\Other"});
    check(importSetup(package, game(), bindings, output).result ==
              PackageResult::UnexpectedBinding &&
              output == sentinel,
          "undeclared local remap fails closed");

    bindings = remap();
    bindings[1].localPath = L"relative\\private";
    check(importSetup(package, game(), bindings, output).result ==
              PackageResult::InvalidLocalPath &&
              output == sentinel,
          "relative remap cannot become a local mutation path");

    bindings = remap();
    bindings[1].localPath = L"D:\\HydraSeat\\root\\..\\escape";
    check(importSetup(package, game(), bindings, output).result ==
              PackageResult::InvalidLocalPath &&
              output == sentinel,
          "absolute local remap containing traversal is rejected before import mutation");

    bindings = remap();
    bindings[1].localPath = L"D:\\HydraSeat\\NUL";
    check(importSetup(package, game(), bindings, output).result ==
              PackageResult::InvalidLocalPath &&
              output == sentinel,
          "Windows reserved-device local path is rejected before import mutation");
}

void testMalformedVersionAndSchemaBehavior() {
    const auto package = exported(true);
    std::string encoded;
    check(encodePackage(package, encoded).succeeded(), "version fixture encodes");

    SetupPackage output;
    output.provenance.sourceId = "sentinel";
    const auto sentinel = output;

    auto future = encoded;
    const auto currentVersion = future.find("VERSION 2\n");
    check(currentVersion != std::string::npos, "current v2 version marker exists");
    if (currentVersion != std::string::npos) future.replace(currentVersion, 10u, "VERSION 3\n");
    check(decodePackage(future, output).result == PackageResult::UnsupportedVersion &&
              output == sentinel,
          "unsupported future package version leaves previous output unchanged");

    auto malformedVersion = encoded;
    const auto malformedPosition = malformedVersion.find("VERSION 2\n");
    if (malformedPosition != std::string::npos) {
        malformedVersion.replace(malformedPosition, 10u, "VERSION x\n");
    }
    check(decodePackage(malformedVersion, output).result == PackageResult::InvalidPackage &&
              output == sentinel,
          "malformed package version is rejected transactionally");

    auto missingCurrentField = encoded;
    const auto materializationsLine = missingCurrentField.find("MATERIALIZATIONS 2\n");
    check(materializationsLine != std::string::npos,
          "current v2 materialization count marker exists");
    if (materializationsLine != std::string::npos) {
        missingCurrentField.erase(materializationsLine, std::string("MATERIALIZATIONS 2\n").size());
    }
    check(decodePackage(missingCurrentField, output).result == PackageResult::InvalidPackage &&
              output == sentinel,
          "v2 package missing its required materialization field is rejected");

    auto unknownField = encoded;
    const auto end = unknownField.rfind("END\n");
    check(end != std::string::npos, "package END marker exists");
    if (end != std::string::npos) unknownField.insert(end, "UNKNOWN 1\n");
    check(decodePackage(unknownField, output).result == PackageResult::InvalidPackage &&
              output == sentinel,
          "unknown/trailing envelope fields are rejected instead of best-effort parsed");

    auto invalidMaterializationUtf = encoded;
    const auto sourceRelativeHeader = invalidMaterializationUtf.find("SOURCE_RELATIVE ");
    check(sourceRelativeHeader != std::string::npos,
          "materialization UTF-8 blob header exists");
    if (sourceRelativeHeader != std::string::npos) {
        const auto data = invalidMaterializationUtf.find('\n', sourceRelativeHeader);
        if (data != std::string::npos && data + 1u < invalidMaterializationUtf.size()) {
            invalidMaterializationUtf[data + 1u] = static_cast<char>(0xffu);
        }
    }
    check(decodePackage(invalidMaterializationUtf, output).result ==
              PackageResult::InvalidPackage &&
              output == sentinel,
          "invalid UTF-8 in a materialization path is rejected transactionally");

    auto unsupportedPhase = encoded;
    const auto phase = unsupportedPhase.find("PHASE 0\n");
    check(phase != std::string::npos, "materialization phase field exists");
    if (phase != std::string::npos) unsupportedPhase.replace(phase, 8u, "PHASE 99\n");
    check(decodePackage(unsupportedPhase, output).result == PackageResult::UnsupportedSemantic &&
              output == sentinel,
          "unsupported decoded phase is rejected as an unsupported semantic");

    auto newerSemanticsDeclaredLegacy = encoded;
    const auto version = newerSemanticsDeclaredLegacy.find("VERSION 2\n");
    if (version != std::string::npos) {
        newerSemanticsDeclaredLegacy.replace(version, 10u, "VERSION 1\n");
    }
    check(decodePackage(newerSemanticsDeclaredLegacy, output).result ==
              PackageResult::InvalidPackage &&
              output == sentinel,
          "materialization semantics cannot be smuggled under legacy package version 1");
}

void testBoundsNulAndExactlyTwoInstances() {
    SetupPackage output;
    output.provenance.sourceId = "sentinel";
    const auto sentinel = output;

    auto tooManyArguments = setup();
    tooManyArguments.instances[0].arguments.assign(profile::kMaximumArgumentsPerInstance + 1u,
                                                    L"x");
    check(exportSetup(tooManyArguments, game(),
                      {"community-fixture", 7u, "hydraseat-test"}, output).result ==
              PackageResult::InvalidSetup && output == sentinel,
          "oversized argument list is rejected before package creation");

    auto oversizedArgument = setup();
    oversizedArgument.instances[0].arguments[0] =
        std::wstring(profile::kMaximumArgumentCodeUnits + 1u, L'x');
    check(exportSetup(oversizedArgument, game(),
                      {"community-fixture", 7u, "hydraseat-test"}, output).result ==
              PackageResult::InvalidSetup && output == sentinel,
          "oversized argument string is rejected before package creation");

    auto oversizedPath = setup();
    oversizedPath.instances[0].dataRoot =
        L"D:\\" + std::wstring(profile::kMaximumPathCodeUnits, L'x');
    check(exportSetup(oversizedPath, game(),
                      {"community-fixture", 7u, "hydraseat-test"}, output).result ==
              PackageResult::InvalidSetup && output == sentinel,
          "oversized path is rejected before package creation");

    auto nulArgument = setup();
    const wchar_t embeddedNul[] = {L'a', L'b', L'\0', L'c'};
    nulArgument.instances[0].arguments[0] = std::wstring(embeddedNul, 4u);
    check(exportSetup(nulArgument, game(),
                      {"community-fixture", 7u, "hydraseat-test"}, output).result ==
              PackageResult::InvalidSetup && output == sentinel,
          "embedded NUL argument is rejected at the package boundary");

    auto invalidUnicode = setup();
    invalidUnicode.instances[0].arguments[0] = std::wstring(1u, static_cast<wchar_t>(0xd800u));
    check(exportSetup(invalidUnicode, game(),
                      {"community-fixture", 7u, "hydraseat-test"}, output).result ==
              PackageResult::InvalidSetup && output == sentinel,
          "unpaired surrogate/invalid wide Unicode is rejected before package creation");

    auto thirdInstance = setup();
    thirdInstance.instances.push_back(thirdInstance.instances.front());
    check(exportSetup(thirdInstance, game(),
                      {"community-fixture", 7u, "hydraseat-test"}, output).result ==
              PackageResult::InvalidSetup && output == sentinel,
          "third instance cannot be smuggled into a v1 two-player setup package");
}

void testMaterializationSemanticValidation() {
    auto descriptors = materializations();
    SetupPackage package;

    descriptors[0].steps[0].phase = static_cast<setup::RecipeExecutionPhase>(99u);
    check(exportSetup(setup(), game(), {"community-fixture", 7u, "hydraseat-test"},
                      descriptors, package).result == PackageResult::UnsupportedSemantic,
          "unknown execution phase is explicitly rejected");

    descriptors = materializations();
    descriptors[0].steps[0].scope = setup::MutationScope::SharedInstallation;
    check(exportSetup(setup(), game(), {"community-fixture", 7u, "hydraseat-test"},
                      descriptors, package).result == PackageResult::UnsupportedSemantic,
          "community/package transport cannot authorize shared-installation mutation");

    descriptors = materializations();
    descriptors.push_back(descriptors.front());
    check(exportSetup(setup(), game(), {"community-fixture", 7u, "hydraseat-test"},
                      descriptors, package).result == PackageResult::InvalidPackage,
          "duplicate/third materialization instance definition is rejected");

    descriptors = materializations();
    descriptors[0].steps[0].files[0].sourceRelativePath = L"defaults/../secret.ini";
    check(exportSetup(setup(), game(), {"community-fixture", 7u, "hydraseat-test"},
                      descriptors, package).result == PackageResult::InvalidPackage,
          "relative materialization source traversal is rejected");

    descriptors = materializations();
    descriptors[0].steps.push_back(
        {"conflict", setup::RecipeExecutionPhase::Startup,
         setup::MutationScope::SeatWritableInstance,
         {{L"defaults/other.ini", L"config/좌석1.ini/child", 1024u}}});
    check(exportSetup(setup(), game(), {"community-fixture", 7u, "hydraseat-test"},
                      descriptors, package).result == PackageResult::InvalidPackage,
          "ancestor/child destination conflicts are rejected deterministically");

    descriptors = materializations();
    descriptors[0].steps[0].files.clear();
    for (std::size_t index = 0u; index <= kMaximumPortableMutableFilesPerInstance; ++index) {
        descriptors[0].steps[0].files.push_back(
            {L"defaults/file-" + std::to_wstring(index) + L".ini",
             L"many/file-" + std::to_wstring(index) + L".ini", 1u});
    }
    check(exportSetup(setup(), game(), {"community-fixture", 7u, "hydraseat-test"},
                      descriptors, package).result == PackageResult::InvalidPackage,
          "materialization file-count bound rejects 65 entries without size_t underflow");

    descriptors = materializations();
    descriptors[0].steps[0].files[0].maximumBytes = kMaximumPortableMutableFileBytes + 1u;
    check(exportSetup(setup(), game(), {"community-fixture", 7u, "hydraseat-test"},
                      descriptors, package).result == PackageResult::InvalidPackage,
          "oversized materialization file bound is rejected");

    auto legacyWithSemantics = exported(true);
    legacyWithSemantics.version = kLegacySetupPackageVersion;
    std::string encoded = "sentinel";
    check(encodePackage(legacyWithSemantics, encoded).result == PackageResult::UnsupportedSemantic &&
              encoded == "sentinel",
          "legacy package version cannot carry newer materialization semantics in memory");
}

void testLegacyVersionOneRemainsDeterministic() {
    auto legacy = exported(false);
    legacy.version = kLegacySetupPackageVersion;
    check(legacy.instanceMaterializations.empty(),
          "legacy fixture contains no v2-only semantics");

    std::string first;
    std::string second;
    check(encodePackage(legacy, first).succeeded() &&
              first.find("VERSION 1\n") != std::string::npos &&
              first.find("MATERIALIZATIONS") == std::string::npos,
          "legacy supported package uses the exact v1 envelope shape");
    SetupPackage decoded;
    check(decodePackage(first, decoded).succeeded() && decoded == legacy,
          "legacy v1 package still decodes deterministically");
    check(encodePackage(decoded, second).succeeded() && second == first,
          "legacy v1 decode then encode is byte-stable");

    profile::TwoPlayerSetup imported;
    check(importSetup(decoded, game(), remap(), imported).succeeded(),
          "legacy v1 package remains importable through setup-only API");
    check(imported.instances[0].arguments == setup().instances[0].arguments &&
              imported.compatibility == compatibility(),
          "legacy v1 preserves arguments and compatibility reference exactly");
}

void testPortableStructureRejectsDuplicateVariableIdentity() {
    auto package = exported(false);
    package.pathVariables[1].variableName = package.pathVariables[0].variableName;
    std::string output = "sentinel";
    check(encodePackage(package, output).result == PackageResult::InvalidPackage &&
              output == "sentinel",
          "duplicate path-variable identity is rejected transactionally");
}

} // namespace

int main() {
    testCurrentRoundTripPreservesEverySupportedSemantic();
    testBindingsAndTraversalFailTransactionally();
    testMalformedVersionAndSchemaBehavior();
    testBoundsNulAndExactlyTwoInstances();
    testMaterializationSemanticValidation();
    testLegacyVersionOneRemainsDeterministic();
    testPortableStructureRejectsDuplicateVariableIdentity();
    if (failures != 0) {
        std::cerr << failures << " setup package test(s) failed\n";
        return 1;
    }
    std::cout << "setup package tests passed\n";
    return 0;
}
