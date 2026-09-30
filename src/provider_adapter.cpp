#include "hydra/provider_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <utility>

namespace hydra::provider {
namespace {

ProviderDiagnostic diagnostic(ProviderResult result, std::string message) {
    if (message.size() > kMaximumProviderDiagnosticBytes) {
        message.resize(kMaximumProviderDiagnosticBytes);
    }
    return {result, std::move(message)};
}

bool validEnum(ProviderAvailability value) noexcept {
    switch (value) {
    case ProviderAvailability::Available:
    case ProviderAvailability::Offline:
    case ProviderAvailability::Absent:
        return true;
    }
    return false;
}

bool validEnum(ProviderResult value) noexcept {
    switch (value) {
    case ProviderResult::Success:
    case ProviderResult::ProviderAbsent:
    case ProviderResult::ProviderOffline:
    case ProviderResult::UnsupportedOperation:
    case ProviderResult::InvalidDescriptor:
    case ProviderResult::InvalidRequest:
    case ProviderResult::InvalidMetadata:
    case ProviderResult::StaleMetadata:
    case ProviderResult::ProviderFailure:
        return true;
    }
    return false;
}

bool validEnum(LaunchTargetKind value) noexcept {
    switch (value) {
    case LaunchTargetKind::Executable:
    case LaunchTargetKind::ProviderUri:
        return true;
    }
    return false;
}

bool validIdentifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > profile::kMaximumIdentifierBytes) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (!(std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == ':')) {
            return false;
        }
    }
    return true;
}

bool validWideText(std::wstring_view value, std::size_t maximum, bool allowEmpty = false) {
    if ((!allowEmpty && value.empty()) || value.size() > maximum) return false;
    for (std::size_t index = 0u; index < value.size(); ++index) {
        const auto codeUnit = static_cast<std::uint32_t>(value[index]);
        if (codeUnit == 0u || codeUnit > 0x10ffffu) return false;
        if (codeUnit >= 0xd800u && codeUnit <= 0xdbffu) {
            if constexpr (sizeof(wchar_t) == 2u) {
                if (++index >= value.size()) return false;
                const auto low = static_cast<std::uint32_t>(value[index]);
                if (low < 0xdc00u || low > 0xdfffu) return false;
            } else {
                return false;
            }
        } else if (codeUnit >= 0xdc00u && codeUnit <= 0xdfffu) {
            return false;
        }
    }
    return true;
}

bool absoluteWindowsPath(std::wstring_view value) {
    if (!validWideText(value, profile::kMaximumPathCodeUnits)) return false;
    const bool drive = value.size() >= 3u &&
                       ((value[0] >= L'A' && value[0] <= L'Z') ||
                        (value[0] >= L'a' && value[0] <= L'z')) &&
                       value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
    const bool unc = value.size() >= 3u &&
                     ((value[0] == L'\\' && value[1] == L'\\') ||
                      (value[0] == L'/' && value[1] == L'/'));
    return drive || unc;
}

bool validProviderUri(std::wstring_view value) {
    if (!validWideText(value, kMaximumLaunchUriCodeUnits)) return false;
    const auto delimiter = value.find(L"://");
    if (delimiter == std::wstring_view::npos || delimiter == 0u) return false;
    for (std::size_t index = 0u; index < delimiter; ++index) {
        const wchar_t ch = value[index];
        const bool valid = (ch >= L'a' && ch <= L'z') ||
                           (ch >= L'A' && ch <= L'Z') ||
                           (index > 0u && ((ch >= L'0' && ch <= L'9') || ch == L'+' ||
                                          ch == L'-' || ch == L'.'));
        if (!valid) return false;
    }
    for (const wchar_t ch : value) {
        if (ch <= L' ' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'`') {
            return false;
        }
    }
    return true;
}

ProviderDiagnostic validateDescriptor(const ProviderDescriptor& descriptor) {
    if (!validIdentifier(descriptor.providerId) || !validEnum(descriptor.availability)) {
        return diagnostic(ProviderResult::InvalidDescriptor,
                          "provider descriptor has invalid identity or availability");
    }
    if (descriptor.availability != ProviderAvailability::Absent &&
        descriptor.metadataRevision == 0u) {
        return diagnostic(ProviderResult::InvalidDescriptor,
                          "installed provider descriptor requires a nonzero metadata revision");
    }
    if (descriptor.capabilities.offlineLaunch &&
        !descriptor.capabilities.launchRequests) {
        return diagnostic(ProviderResult::InvalidDescriptor,
                          "offline launch cannot be declared without launch-request capability");
    }
    return {};
}

ProviderDiagnostic operationPreflight(const ProviderDescriptor& descriptor,
                                      bool supported,
                                      bool requiresOnline) {
    auto result = validateDescriptor(descriptor);
    if (!result.succeeded()) return result;
    if (descriptor.availability == ProviderAvailability::Absent) {
        return diagnostic(ProviderResult::ProviderAbsent, "provider is not installed");
    }
    if (!supported) {
        return diagnostic(ProviderResult::UnsupportedOperation,
                          "provider does not support this operation");
    }
    if (requiresOnline && descriptor.availability == ProviderAvailability::Offline) {
        return diagnostic(ProviderResult::ProviderOffline,
                          "provider is offline and does not support offline launch");
    }
    return {};
}

ProviderDiagnostic validateAdapterResponse(ProviderResult result,
                                           std::string_view message) {
    if (!validEnum(result)) {
        return diagnostic(ProviderResult::InvalidMetadata,
                          "provider returned an unknown result value");
    }
    if (result == ProviderResult::Success) return {};
    return diagnostic(result, message.empty() ? "provider operation failed" : std::string(message));
}

ProviderDiagnostic validateRevision(std::uint64_t expected, std::uint64_t actual) {
    if (actual == 0u || actual != expected) {
        return diagnostic(ProviderResult::StaleMetadata,
                          "provider metadata revision changed during the operation");
    }
    return {};
}

ProviderDiagnostic validateLaunchSelection(const ProviderDescriptor& descriptor,
                                           const LaunchSelection& selection) {
    if (selection.providerId != descriptor.providerId ||
        !validIdentifier(selection.gameId) ||
        selection.expectedMetadataRevision == 0u) {
        return diagnostic(ProviderResult::InvalidRequest,
                          "launch selection identity or revision is invalid");
    }
    if (selection.providerAppId && !validIdentifier(*selection.providerAppId)) {
        return diagnostic(ProviderResult::InvalidRequest,
                          "launch selection provider app identity is invalid");
    }
    if (selection.accountRef && !validIdentifier(*selection.accountRef)) {
        return diagnostic(ProviderResult::InvalidRequest,
                          "launch selection account reference is invalid");
    }
    if (selection.instanceArguments.size() > kMaximumLaunchArguments) {
        return diagnostic(ProviderResult::InvalidRequest,
                          "launch selection contains too many instance arguments");
    }
    for (const auto& argument : selection.instanceArguments) {
        if (!validWideText(argument, kMaximumLaunchArgumentCodeUnits, true)) {
            return diagnostic(ProviderResult::InvalidRequest,
                              "launch selection contains an invalid instance argument");
        }
    }
    return validateRevision(descriptor.metadataRevision,
                            selection.expectedMetadataRevision);
}

ProviderDiagnostic validateLaunch(const ProviderDescriptor& descriptor,
                                  const LaunchSelection& selection,
                                  const ProviderLaunchRequest& request) {
    if (!validEnum(request.targetKind) || request.providerId != descriptor.providerId ||
        request.providerId != selection.providerId || request.gameId != selection.gameId ||
        request.providerAppId != selection.providerAppId ||
        request.accountRef != selection.accountRef ||
        !validIdentifier(request.launchCorrelationId)) {
        return diagnostic(ProviderResult::InvalidMetadata,
                          "provider launch response does not match the requested identity");
    }
    auto revision = validateRevision(descriptor.metadataRevision, request.metadataRevision);
    if (!revision.succeeded()) return revision;
    if (request.arguments.size() > kMaximumLaunchArguments) {
        return diagnostic(ProviderResult::InvalidMetadata,
                          "provider launch response contains too many arguments");
    }
    for (const auto& argument : request.arguments) {
        if (!validWideText(argument, kMaximumLaunchArgumentCodeUnits, true)) {
            return diagnostic(ProviderResult::InvalidMetadata,
                              "provider launch response contains an invalid argument");
        }
    }
    if (request.workingDirectory && !absoluteWindowsPath(*request.workingDirectory)) {
        return diagnostic(ProviderResult::InvalidMetadata,
                          "provider launch response has an invalid working directory");
    }
    const bool targetValid = request.targetKind == LaunchTargetKind::Executable
                                 ? absoluteWindowsPath(request.target)
                                 : validProviderUri(request.target);
    if (!targetValid) {
        return diagnostic(ProviderResult::InvalidMetadata,
                          "provider launch response has an invalid typed target");
    }
    return {};
}

} // namespace

ProviderDiagnostic discoverInstalledGames(
    LauncherProviderAdapter& adapter,
    std::vector<catalog::GameCatalogCandidate>& output) {
    const auto descriptor = adapter.descriptor();
    auto result = operationPreflight(descriptor,
                                     descriptor.capabilities.installedGameDiscovery,
                                     false);
    if (!result.succeeded()) return result;

    auto response = adapter.discoverInstalledGames();
    result = validateAdapterResponse(response.result, response.diagnostic);
    if (!result.succeeded()) return result;
    result = validateRevision(descriptor.metadataRevision, response.metadataRevision);
    if (!result.succeeded()) return result;
    if (response.candidates.size() > catalog::kMaximumCatalogCandidates) {
        return diagnostic(ProviderResult::InvalidMetadata,
                          "provider returned too many discovery candidates");
    }
    for (const auto& candidate : response.candidates) {
        if (candidate.providerId != descriptor.providerId || !candidate.providerAppId) {
            return diagnostic(ProviderResult::InvalidMetadata,
                              "provider discovery candidate has mismatched or missing app identity");
        }
    }
    catalog::LocalGameCatalog validated;
    const auto catalogResult = catalog::buildLocalGameCatalog(response.candidates, validated);
    if (!catalogResult.succeeded()) {
        return diagnostic(ProviderResult::InvalidMetadata,
                          "provider discovery candidate failed catalog validation: " +
                              catalogResult.message);
    }
    output = std::move(response.candidates);
    return {};
}

ProviderDiagnostic listAccountReferences(
    LauncherProviderAdapter& adapter,
    std::vector<profile::ProviderAccountReference>& output) {
    const auto descriptor = adapter.descriptor();
    auto result = operationPreflight(descriptor,
                                     descriptor.capabilities.accountReferences,
                                     false);
    if (!result.succeeded()) return result;

    auto response = adapter.listAccountReferences();
    result = validateAdapterResponse(response.result, response.diagnostic);
    if (!result.succeeded()) return result;
    result = validateRevision(descriptor.metadataRevision, response.metadataRevision);
    if (!result.succeeded()) return result;
    if (response.accounts.size() > kMaximumProviderAccounts) {
        return diagnostic(ProviderResult::InvalidMetadata,
                          "provider returned too many account references");
    }
    std::set<std::string> unique;
    for (const auto& account : response.accounts) {
        if (account.providerId != descriptor.providerId ||
            !validIdentifier(account.accountRef) || !unique.insert(account.accountRef).second) {
            return diagnostic(ProviderResult::InvalidMetadata,
                              "provider returned an invalid or duplicate account reference");
        }
    }
    std::sort(response.accounts.begin(), response.accounts.end(),
              [](const auto& left, const auto& right) {
                  return left.accountRef < right.accountRef;
              });
    output = std::move(response.accounts);
    return {};
}

ProviderDiagnostic buildLaunchRequest(
    LauncherProviderAdapter& adapter,
    const LaunchSelection& selection,
    ProviderLaunchRequest& output) {
    const auto descriptor = adapter.descriptor();
    auto result = operationPreflight(
        descriptor, descriptor.capabilities.launchRequests,
        descriptor.availability == ProviderAvailability::Offline &&
            !descriptor.capabilities.offlineLaunch);
    if (!result.succeeded()) return result;
    result = validateLaunchSelection(descriptor, selection);
    if (!result.succeeded()) return result;

    auto response = adapter.buildLaunchRequest(selection);
    result = validateAdapterResponse(response.result, response.diagnostic);
    if (!result.succeeded()) return result;
    result = validateLaunch(descriptor, selection, response.request);
    if (!result.succeeded()) return result;
    output = std::move(response.request);
    return {};
}

ProviderDiagnostic identifyProcesses(
    LauncherProviderAdapter& adapter,
    const ProcessIdentificationQuery& query,
    std::vector<ProviderProcessEvidence>& output) {
    const auto descriptor = adapter.descriptor();
    auto result = operationPreflight(descriptor,
                                     descriptor.capabilities.processIdentification,
                                     false);
    if (!result.succeeded()) return result;
    if (query.providerId != descriptor.providerId || !validIdentifier(query.gameId) ||
        (query.providerAppId && !validIdentifier(*query.providerAppId)) ||
        !validIdentifier(query.launchCorrelationId) ||
        query.expectedMetadataRevision == 0u) {
        return diagnostic(ProviderResult::InvalidRequest,
                          "process-identification query is invalid");
    }
    result = validateRevision(descriptor.metadataRevision,
                              query.expectedMetadataRevision);
    if (!result.succeeded()) return result;

    auto response = adapter.identifyProcesses(query);
    result = validateAdapterResponse(response.result, response.diagnostic);
    if (!result.succeeded()) return result;
    result = validateRevision(descriptor.metadataRevision, response.metadataRevision);
    if (!result.succeeded()) return result;
    if (response.processes.size() > kMaximumProcessEvidence) {
        return diagnostic(ProviderResult::InvalidMetadata,
                          "provider returned too many process evidence records");
    }
    std::set<std::pair<std::uint32_t, std::uint64_t>> unique;
    for (const auto& process : response.processes) {
        if (process.processId == 0u || process.creationTime100ns == 0u ||
            !absoluteWindowsPath(process.executablePath) ||
            !unique.emplace(process.processId, process.creationTime100ns).second) {
            return diagnostic(ProviderResult::InvalidMetadata,
                              "provider returned malformed or duplicate process evidence");
        }
    }
    output = std::move(response.processes);
    return {};
}

std::string_view providerResultName(ProviderResult result) noexcept {
    switch (result) {
    case ProviderResult::Success: return "success";
    case ProviderResult::ProviderAbsent: return "provider-absent";
    case ProviderResult::ProviderOffline: return "provider-offline";
    case ProviderResult::UnsupportedOperation: return "unsupported-operation";
    case ProviderResult::InvalidDescriptor: return "invalid-descriptor";
    case ProviderResult::InvalidRequest: return "invalid-request";
    case ProviderResult::InvalidMetadata: return "invalid-metadata";
    case ProviderResult::StaleMetadata: return "stale-metadata";
    case ProviderResult::ProviderFailure: return "provider-failure";
    }
    return "unknown";
}

std::string_view providerAvailabilityName(ProviderAvailability availability) noexcept {
    switch (availability) {
    case ProviderAvailability::Available: return "available";
    case ProviderAvailability::Offline: return "offline";
    case ProviderAvailability::Absent: return "absent";
    }
    return "unknown";
}

std::string_view launchTargetKindName(LaunchTargetKind kind) noexcept {
    switch (kind) {
    case LaunchTargetKind::Executable: return "executable";
    case LaunchTargetKind::ProviderUri: return "provider-uri";
    }
    return "unknown";
}

} // namespace hydra::provider
