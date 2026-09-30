#include "hydra/compatibility_share_model.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace hydra::community {
namespace {

ShareModelDiagnostic fail(ShareModelCode code, std::string message,
                          std::optional<SubmissionCode> submission = std::nullopt) {
    return {code, submission, std::move(message)};
}

static_assert(kCompatibilityLocalHistorySchemaVersion == 1u);
constexpr std::string_view kCompatibilityLocalHistoryHeader =
    "{\"schemaVersion\":1,\"kind\":\"compatibility-local-history\"}";

ShareModelDiagnostic validatePrivacySettingsValue(const CompatibilityPrivacySettings& settings) {
    if (settings.retainedLocalResults == 0u ||
        settings.retainedLocalResults > kMaximumRetainedCompatibilityResults) {
        return fail(ShareModelCode::InvalidPrivacySettings,
                    "retained local result count must be between 1 and 64");
    }
    return {};
}

bool publicCompatibilityEnvironment(const compat::CompatibilityResult& result) noexcept {
    const bool publicWindowsClass = result.windowsBuildClass.starts_with("win10-") ||
                                    result.windowsBuildClass.starts_with("win11-");
    const bool publicArchitecture = result.architecture == "x64" ||
                                    result.architecture == "x86" ||
                                    result.architecture == "arm64";
    return publicWindowsClass && publicArchitecture;
}

class PrivacySettingsParser final {
public:
    explicit PrivacySettingsParser(std::string_view text) : text_(text) {}

    bool parse(CompatibilityPrivacySettings& output, std::string& error) {
        skipWhitespace();
        if (!consume('{')) return setError(error, "privacy settings must be a JSON object");

        bool seenSchema = false;
        bool seenSharing = false;
        bool seenRetention = false;
        std::uint64_t schemaVersion = 0u;
        CompatibilityPrivacySettings candidate;

        skipWhitespace();
        if (consume('}')) return setError(error, "privacy settings object is missing required fields");

        for (;;) {
            std::string_view key;
            if (!parseKey(key, error)) return false;
            skipWhitespace();
            if (!consume(':')) return setError(error, "privacy settings field is missing ':'");
            skipWhitespace();

            if (key == "schemaVersion") {
                if (seenSchema) return setError(error, "duplicate schemaVersion field");
                seenSchema = true;
                if (!parseUnsigned(schemaVersion, error)) return false;
            } else if (key == "communitySharingEnabled") {
                if (seenSharing) return setError(error, "duplicate communitySharingEnabled field");
                seenSharing = true;
                if (!parseBool(candidate.communitySharingEnabled, error)) return false;
            } else if (key == "retainedLocalResults") {
                if (seenRetention) return setError(error, "duplicate retainedLocalResults field");
                seenRetention = true;
                std::uint64_t retained = 0u;
                if (!parseUnsigned(retained, error)) return false;
                if (retained > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                    return setError(error, "retainedLocalResults exceeds the platform size limit");
                }
                candidate.retainedLocalResults = static_cast<std::size_t>(retained);
            } else {
                return setError(error, "unknown privacy settings field");
            }

            skipWhitespace();
            if (consume('}')) break;
            if (!consume(',')) return setError(error, "privacy settings fields must be comma separated");
            skipWhitespace();
        }

        skipWhitespace();
        if (position_ != text_.size()) return setError(error, "trailing content after privacy settings object");
        if (!seenSchema || !seenSharing || !seenRetention) {
            return setError(error, "privacy settings object is missing required fields");
        }
        if (schemaVersion != kCompatibilityPrivacySettingsSchemaVersion) {
            return setError(error, "unsupported privacy settings schema version");
        }
        const auto validation = validatePrivacySettingsValue(candidate);
        if (!validation.succeeded()) return setError(error, validation.message);
        output = candidate;
        return true;
    }

private:
    void skipWhitespace() noexcept {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }
    }

    bool consume(char expected) noexcept {
        if (position_ >= text_.size() || text_[position_] != expected) return false;
        ++position_;
        return true;
    }

    static bool setError(std::string& error, std::string message) {
        error = std::move(message);
        return false;
    }

    bool parseKey(std::string_view& key, std::string& error) {
        skipWhitespace();
        if (!consume('"')) return setError(error, "privacy settings field name must be a JSON string");
        const auto start = position_;
        while (position_ < text_.size()) {
            const auto value = static_cast<unsigned char>(text_[position_]);
            if (value == static_cast<unsigned char>('"')) {
                key = text_.substr(start, position_ - start);
                ++position_;
                if (key.empty()) return setError(error, "privacy settings field name cannot be empty");
                return true;
            }
            if (value < 0x20u || value == static_cast<unsigned char>('\\')) {
                return setError(error, "privacy settings field name must use plain ASCII key text");
            }
            ++position_;
        }
        return setError(error, "unterminated privacy settings field name");
    }

    bool parseBool(bool& output, std::string& error) {
        if (text_.substr(position_, 4u) == "true") {
            position_ += 4u;
            output = true;
            return true;
        }
        if (text_.substr(position_, 5u) == "false") {
            position_ += 5u;
            output = false;
            return true;
        }
        return setError(error, "communitySharingEnabled must be a JSON boolean");
    }

    bool parseUnsigned(std::uint64_t& output, std::string& error) {
        if (position_ >= text_.size() ||
            std::isdigit(static_cast<unsigned char>(text_[position_])) == 0) {
            return setError(error, "privacy settings numeric field must be an unsigned integer");
        }
        const auto start = position_;
        if (text_[position_] == '0') {
            ++position_;
            if (position_ < text_.size() &&
                std::isdigit(static_cast<unsigned char>(text_[position_])) != 0) {
                return setError(error, "privacy settings integer cannot contain a leading zero");
            }
        } else {
            while (position_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[position_])) != 0) {
                ++position_;
            }
        }
        const auto token = text_.substr(start, position_ - start);
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), output);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
            return setError(error, "privacy settings integer is out of range");
        }
        return true;
    }

    std::string_view text_;
    std::size_t position_{0u};
};

} // namespace

ShareModelDiagnostic CompatibilityShareModel::setPrivacySettings(
    const CompatibilityPrivacySettings& settings) {
    const auto validation = validatePrivacySettingsValue(settings);
    if (!validation.succeeded()) return validation;

    const bool settingsChanged = settings != privacySettings_;
    const bool retentionChanged = settings.retainedLocalResults != privacySettings_.retainedLocalResults;
    if (settingsChanged) resetLocalExportApproval();
    if (retentionChanged) {
        if (auto* entry = activeMutable(); entry != nullptr && entry->preview) {
            entry->preview.reset();
            if (entry->state == CompatibilityShareState::PreviewReady ||
                entry->state == CompatibilityShareState::SubmitPending ||
                entry->state == CompatibilityShareState::SubmitFailed ||
                entry->state == CompatibilityShareState::UserDeclined) {
                entry->state = CompatibilityShareState::LocalResultAvailable;
            }
        }
        submission_ = SubmissionSession{};
    }

    privacySettings_ = settings;
    if (!privacySettings_.communitySharingEnabled) {
        if (auto* entry = activeMutable();
            entry != nullptr && entry->state == CompatibilityShareState::SubmitPending) {
            entry->state = CompatibilityShareState::PreviewReady;
        }
    }
    enforceRetentionLimit();
    return {};
}

ShareModelDiagnostic CompatibilityShareModel::loadPrivacySettingsJson(std::string_view json) {
    if (json.empty() || json.size() > kMaximumCompatibilityPrivacySettingsBytes) {
        return fail(ShareModelCode::InvalidPrivacySettings,
                    "privacy settings JSON must be non-empty and at most 1024 bytes");
    }

    CompatibilityPrivacySettings candidate;
    std::string error;
    PrivacySettingsParser parser(json);
    if (!parser.parse(candidate, error)) {
        return fail(ShareModelCode::InvalidPrivacySettings,
                    "privacy settings JSON is invalid: " + error);
    }
    return setPrivacySettings(candidate);
}

ShareModelDiagnostic CompatibilityShareModel::exportPrivacySettingsJson(std::string& json) const {
    const auto validation = validatePrivacySettingsValue(privacySettings_);
    if (!validation.succeeded()) return validation;

    std::string candidate =
        "{\"schemaVersion\":" + std::to_string(kCompatibilityPrivacySettingsSchemaVersion) +
        ",\"communitySharingEnabled\":" +
        std::string(privacySettings_.communitySharingEnabled ? "true" : "false") +
        ",\"retainedLocalResults\":" +
        std::to_string(privacySettings_.retainedLocalResults) + "}";
    if (candidate.size() > kMaximumCompatibilityPrivacySettingsBytes) {
        return fail(ShareModelCode::InvalidPrivacySettings,
                    "encoded privacy settings exceed the bounded persistence contract");
    }
    json = std::move(candidate);
    return {};
}

void CompatibilityShareModel::resetLocalExportApproval() noexcept {
    localExport_.resultId.reset();
    localExport_.exactJson.clear();
    localExport_.payloadIdentity.clear();
    localExport_.generation = 0u;
    localExport_.approved = false;
}

void CompatibilityShareModel::enforceRetentionLimit() noexcept {
    if (history_.size() <= privacySettings_.retainedLocalResults) return;

    resetLocalExportApproval();
    const auto removeCount = history_.size() - privacySettings_.retainedLocalResults;
    history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(removeCount));
    if (!activeIndex_) return;
    if (*activeIndex_ < removeCount) {
        activeIndex_.reset();
        submission_ = SubmissionSession{};
        return;
    }
    *activeIndex_ -= removeCount;
}

CompatibilityShareEntry* CompatibilityShareModel::activeMutable() noexcept {
    if (!activeIndex_ || *activeIndex_ >= history_.size()) return nullptr;
    return &history_[*activeIndex_];
}

const CompatibilityShareEntry* CompatibilityShareModel::active() const noexcept {
    if (!activeIndex_ || *activeIndex_ >= history_.size()) return nullptr;
    return &history_[*activeIndex_];
}

CompatibilityShareState CompatibilityShareModel::state() const noexcept {
    const auto* entry = active();
    return entry == nullptr ? CompatibilityShareState::TestNotRun : entry->state;
}

ShareModelDiagnostic CompatibilityShareModel::recordLocalResult(
    const compat::CompatibilityResult& result) {
    auto canonical = result;
    const auto validation = compat::canonicalizeCompatibilityResult(canonical);
    if (!validation.succeeded()) {
        return fail(ShareModelCode::InvalidResult,
                    "local compatibility result is invalid: " + validation.message);
    }
    if (!publicCompatibilityEnvironment(canonical)) {
        return fail(ShareModelCode::InvalidResult,
                    "local compatibility result environment is not a bounded public build class");
    }

    const auto duplicate = std::find_if(history_.begin(), history_.end(),
                                        [&](const auto& entry) {
                                            return entry.result.resultId == canonical.resultId;
                                        });
    if (duplicate != history_.end()) {
        return fail(ShareModelCode::InvalidResult,
                    "local compatibility result id already exists");
    }

    const auto previousIndex = activeIndex_;
    CompatibilityShareEntry entry;
    entry.result = std::move(canonical);
    entry.state = CompatibilityShareState::LocalResultAvailable;
    try {
        history_.push_back(std::move(entry));
    } catch (...) {
        return fail(ShareModelCode::InvalidResult,
                    "local compatibility result history allocation failed");
    }
    if (previousIndex && *previousIndex < history_.size() - 1u) {
        auto& previous = history_[*previousIndex];
        previous.state = CompatibilityShareState::Superseded;
        previous.supersededByResultId = history_.back().result.resultId;
    }
    activeIndex_ = history_.size() - 1u;
    submission_ = SubmissionSession{};
    resetLocalExportApproval();
    enforceRetentionLimit();
    return {};
}

ShareModelDiagnostic CompatibilityShareModel::preparePreview(CompatibilitySharePreview& preview) {
    auto* entry = activeMutable();
    if (entry == nullptr) {
        return fail(ShareModelCode::NoActiveResult,
                    "run a local compatibility test before preparing a sharing preview");
    }
    if (entry->state != CompatibilityShareState::LocalResultAvailable &&
        entry->state != CompatibilityShareState::UserDeclined &&
        entry->state != CompatibilityShareState::SubmitFailed) {
        return fail(ShareModelCode::InvalidState,
                    "sharing preview cannot be regenerated from the current state");
    }
    if (nextPreviewGeneration_ == 0u) {
        return fail(ShareModelCode::InvalidState,
                    "sharing preview generation is exhausted; restart the sharing flow");
    }

    SubmissionPreview submissionPreview;
    const auto prepared = submission_.prepare(entry->result, submissionPreview);
    if (!prepared.succeeded()) {
        return fail(ShareModelCode::InvalidResult, prepared.message, prepared.code);
    }
    CompatibilitySharePreview candidate;
    candidate.exactRedactedJson = submissionPreview.exactRedactedJson;
    candidate.payloadIdentity = submissionPreview.idempotencyKey;
    candidate.submissionId = submissionPreview.submissionId;
    candidate.protectedExperimental = submissionPreview.protectedExperimental;
    candidate.generation = nextPreviewGeneration_++;
    entry->preview = candidate;
    entry->receipt.reset();
    entry->lastSubmissionCode.reset();
    entry->state = CompatibilityShareState::PreviewReady;
    preview = std::move(candidate);
    return {};
}

ShareModelDiagnostic CompatibilityShareModel::declineSharing() {
    auto* entry = activeMutable();
    if (entry == nullptr) {
        return fail(ShareModelCode::NoActiveResult,
                    "there is no local result whose sharing can be declined");
    }
    if (entry->state != CompatibilityShareState::PreviewReady) {
        return fail(ShareModelCode::PreviewRequired,
                    "review the exact redacted preview before declining sharing");
    }
    entry->state = CompatibilityShareState::UserDeclined;
    return {};
}

ShareModelDiagnostic CompatibilityShareModel::beginSubmission(
    const CompatibilitySharePreview& approvedPreview) {
    if (!privacySettings_.communitySharingEnabled) {
        return fail(ShareModelCode::SharingDisabled,
                    "community sharing is disabled; enable it explicitly before submission");
    }
    auto* entry = activeMutable();
    if (entry == nullptr) {
        return fail(ShareModelCode::NoActiveResult,
                    "there is no local result available for submission");
    }
    if (!entry->preview || approvedPreview.generation == 0u ||
        approvedPreview.generation != entry->preview->generation) {
        return fail(ShareModelCode::StaleApproval,
                    "sharing approval refers to a stale or superseded preview generation");
    }
    if (entry->state != CompatibilityShareState::PreviewReady &&
        entry->state != CompatibilityShareState::SubmitFailed) {
        return fail(ShareModelCode::InvalidState,
                    "submission can begin only from PreviewReady or SubmitFailed");
    }
    if (approvedPreview.canonicalizationVersion != kCompatibilityCanonicalizationVersion ||
        approvedPreview.canonicalizationVersion != entry->preview->canonicalizationVersion ||
        approvedPreview.payloadIdentity != entry->preview->payloadIdentity ||
        approvedPreview.submissionId != entry->preview->submissionId ||
        approvedPreview.protectedExperimental != entry->preview->protectedExperimental ||
        approvedPreview.exactRedactedJson != entry->preview->exactRedactedJson) {
        return fail(ShareModelCode::PayloadIdentityMismatch,
                    "sharing approval identity or bytes do not match the prepared payload");
    }
    const auto approved = submission_.approve(approvedPreview.exactRedactedJson);
    if (!approved.succeeded()) {
        return fail(ShareModelCode::PreviewRequired, approved.message, approved.code);
    }
    entry->state = CompatibilityShareState::SubmitPending;
    return {};
}

ShareModelDiagnostic CompatibilityShareModel::completeSubmission(
    CommunitySubmissionTransport& transport, SubmissionReceipt& receipt) {
    auto* entry = activeMutable();
    if (entry == nullptr) {
        return fail(ShareModelCode::NoActiveResult,
                    "there is no local result available for submission");
    }
    if (!privacySettings_.communitySharingEnabled) {
        if (entry->state == CompatibilityShareState::SubmitPending) {
            entry->state = CompatibilityShareState::PreviewReady;
        }
        return fail(ShareModelCode::SharingDisabled,
                    "community sharing was disabled before transport; no network call was made");
    }
    if (entry->state != CompatibilityShareState::SubmitPending) {
        return fail(ShareModelCode::InvalidState,
                    "submission completion requires the explicit SubmitPending state");
    }

    SubmissionReceipt candidate;
    const auto submitted = submission_.submit(transport, candidate);
    entry->lastSubmissionCode = submitted.code;
    if (submitted.succeeded()) {
        entry->receipt = candidate;
        entry->state = CompatibilityShareState::SubmitSucceeded;
        receipt = std::move(candidate);
        return {};
    }

    entry->state = CompatibilityShareState::SubmitFailed;
    if (candidate.receiptId || candidate.remoteAccepted ||
        candidate.result != SubmissionTransportResult::RetryableFailure) {
        entry->receipt = candidate;
        receipt = std::move(candidate);
    }
    return fail(ShareModelCode::SubmissionFailed, submitted.message, submitted.code);
}

ShareModelDiagnostic CompatibilityShareModel::prepareActiveLocalExport(
    CompatibilityLocalExportPreview& preview) {
    const auto* entry = active();
    if (entry == nullptr) {
        resetLocalExportApproval();
        return fail(ShareModelCode::NoActiveResult,
                    "there is no active local compatibility result to preview for export");
    }
    return prepareLocalExport(entry->result.resultId, preview);
}

ShareModelDiagnostic CompatibilityShareModel::prepareLocalExport(
    std::string_view resultId, CompatibilityLocalExportPreview& preview) {
    resetLocalExportApproval();
    const auto found = std::find_if(history_.begin(), history_.end(),
                                    [&](const auto& entry) {
                                        return entry.result.resultId == resultId;
                                    });
    if (found == history_.end()) {
        return fail(ShareModelCode::ResultNotFound,
                    "the requested local compatibility result does not exist");
    }
    if (nextLocalExportGeneration_ == 0u) {
        return fail(ShareModelCode::InvalidState,
                    "local export preview generation is exhausted; restart the export flow");
    }

    SubmissionSession canonicalizer;
    SubmissionPreview canonical;
    const auto prepared = canonicalizer.prepare(found->result, canonical);
    if (!prepared.succeeded()) {
        return fail(ShareModelCode::ExportFailed,
                    "local compatibility result could not be frozen for export: " + prepared.message,
                    prepared.code);
    }

    CompatibilityLocalExportPreview candidate;
    candidate.resultId = found->result.resultId;
    candidate.exactRedactedJson = canonical.exactRedactedJson;
    candidate.payloadIdentity = canonical.idempotencyKey;
    candidate.generation = nextLocalExportGeneration_++;
    localExport_.resultId = candidate.resultId;
    localExport_.exactJson = candidate.exactRedactedJson;
    localExport_.payloadIdentity = candidate.payloadIdentity;
    localExport_.generation = candidate.generation;
    localExport_.approved = false;
    preview = std::move(candidate);
    return {};
}

ShareModelDiagnostic CompatibilityShareModel::approveLocalExport(
    const CompatibilityLocalExportPreview& approvedPreview) {
    if (!localExport_.resultId || localExport_.generation == 0u) {
        return fail(ShareModelCode::PreviewRequired,
                    "local compatibility export must be previewed before approval");
    }
    if (approvedPreview.generation == 0u || approvedPreview.generation != localExport_.generation) {
        localExport_.approved = false;
        return fail(ShareModelCode::StaleApproval,
                    "local export approval refers to a stale or superseded preview generation");
    }
    if (approvedPreview.canonicalizationVersion != kCompatibilityCanonicalizationVersion ||
        approvedPreview.resultId != *localExport_.resultId ||
        approvedPreview.exactRedactedJson != localExport_.exactJson ||
        approvedPreview.payloadIdentity != localExport_.payloadIdentity) {
        localExport_.approved = false;
        return fail(ShareModelCode::PayloadIdentityMismatch,
                    "local export approval identity or bytes do not match the frozen payload");
    }
    localExport_.approved = true;
    return {};
}

ShareModelDiagnostic CompatibilityShareModel::declineLocalExport() {
    if (!localExport_.resultId || localExport_.generation == 0u) {
        return fail(ShareModelCode::PreviewRequired,
                    "local compatibility export must be previewed before it can be declined");
    }
    resetLocalExportApproval();
    return {};
}

ShareModelDiagnostic CompatibilityShareModel::exportActiveLocalResult(
    std::string& exactJson) const {
    const auto* entry = active();
    if (entry == nullptr) {
        return fail(ShareModelCode::NoActiveResult,
                    "there is no active local compatibility result to export");
    }
    return exportLocalResult(entry->result.resultId, exactJson);
}

ShareModelDiagnostic CompatibilityShareModel::exportLocalResult(
    std::string_view resultId, std::string& exactJson) const {
    const auto found = std::find_if(history_.begin(), history_.end(),
                                    [&](const auto& entry) {
                                        return entry.result.resultId == resultId;
                                    });
    if (found == history_.end()) {
        return fail(ShareModelCode::ResultNotFound,
                    "the requested local compatibility result does not exist");
    }
    if (!localExport_.resultId || !localExport_.approved || localExport_.generation == 0u) {
        return fail(ShareModelCode::PreviewRequired,
                    "local compatibility export requires approval of the exact frozen preview");
    }
    if (resultId != *localExport_.resultId) {
        return fail(ShareModelCode::StaleApproval,
                    "local export approval belongs to another result or superseded preview");
    }
    exactJson = localExport_.exactJson;
    return {};
}

ShareModelDiagnostic CompatibilityShareModel::loadLocalHistoryJsonl(std::string_view jsonl) {
    if (jsonl.empty()) {
        return fail(ShareModelCode::InvalidLocalHistory,
                    "local compatibility history must contain a versioned header");
    }
    if (jsonl.size() > kMaximumCompatibilityLocalHistoryBytes) {
        return fail(ShareModelCode::LocalHistoryTooLarge,
                    "local compatibility history exceeds the bounded store size");
    }

    const auto firstNewline = jsonl.find('\n');
    auto header = jsonl.substr(0u, firstNewline);
    if (!header.empty() && header.back() == '\r') header.remove_suffix(1u);
    if (header != kCompatibilityLocalHistoryHeader) {
        return fail(ShareModelCode::InvalidLocalHistory,
                    "unsupported or malformed local compatibility history header");
    }
    if (firstNewline != std::string_view::npos && firstNewline + 1u == jsonl.size()) {
        return fail(ShareModelCode::InvalidLocalHistory,
                    "local compatibility history cannot end with an empty result line");
    }

    CompatibilityShareModel candidate;
    const auto settingsApplied = candidate.setPrivacySettings(privacySettings_);
    if (!settingsApplied.succeeded()) return settingsApplied;

    std::set<std::string> seenResultIds;
    std::size_t position = firstNewline == std::string_view::npos
                               ? jsonl.size()
                               : firstNewline + 1u;
    std::size_t resultCount = 0u;
    while (position < jsonl.size()) {
        const auto nextNewline = jsonl.find('\n', position);
        const auto end = nextNewline == std::string_view::npos ? jsonl.size() : nextNewline;
        auto line = jsonl.substr(position, end - position);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1u);
        if (line.empty()) {
            return fail(ShareModelCode::InvalidLocalHistory,
                        "local compatibility history cannot contain empty result lines");
        }
        if (line.size() > compat::kMaximumCompatibilityResultBytes) {
            return fail(ShareModelCode::LocalHistoryTooLarge,
                        "one local compatibility result exceeds the public result bound");
        }
        if (++resultCount > kMaximumRetainedCompatibilityResults) {
            return fail(ShareModelCode::LocalHistoryTooLarge,
                        "local compatibility history contains too many results");
        }

        compat::CompatibilityResult result;
        const auto decoded = compat::decodeCompatibilityResultJson(line, result);
        if (!decoded.succeeded()) {
            return fail(ShareModelCode::InvalidLocalHistory,
                        "stored compatibility result is invalid: " + decoded.message);
        }
        if (!seenResultIds.insert(result.resultId).second) {
            return fail(ShareModelCode::InvalidLocalHistory,
                        "local compatibility history contains a duplicate result id");
        }
        const auto recorded = candidate.recordLocalResult(result);
        if (!recorded.succeeded()) {
            return fail(ShareModelCode::InvalidLocalHistory,
                        "stored compatibility result could not be restored: " + recorded.message);
        }

        if (nextNewline == std::string_view::npos) break;
        position = nextNewline + 1u;
        if (position == jsonl.size()) {
            return fail(ShareModelCode::InvalidLocalHistory,
                        "local compatibility history cannot end with an empty result line");
        }
    }

    history_ = std::move(candidate.history_);
    activeIndex_ = candidate.activeIndex_;
    submission_ = SubmissionSession{};
    resetLocalExportApproval();
    return {};
}

ShareModelDiagnostic CompatibilityShareModel::exportLocalHistoryJsonl(std::string& jsonl) const {
    if (history_.size() > kMaximumRetainedCompatibilityResults) {
        return fail(ShareModelCode::LocalHistoryTooLarge,
                    "local compatibility history contains too many results");
    }

    std::string candidate(kCompatibilityLocalHistoryHeader);
    for (const auto& entry : history_) {
        std::string encoded;
        const auto diagnostic = compat::encodeCompatibilityResultJson(entry.result, encoded);
        if (!diagnostic.succeeded()) {
            return fail(ShareModelCode::ExportFailed,
                        "local compatibility history contains an invalid result: " +
                            diagnostic.message);
        }
        if (candidate.size() + 1u + encoded.size() > kMaximumCompatibilityLocalHistoryBytes) {
            return fail(ShareModelCode::LocalHistoryTooLarge,
                        "encoded local compatibility history exceeds the bounded store size");
        }
        candidate.push_back('\n');
        candidate += encoded;
    }
    jsonl = std::move(candidate);
    return {};
}

ShareModelDiagnostic CompatibilityShareModel::deleteLocalResult(std::string_view resultId) {
    for (std::size_t index = 0; index < history_.size(); ++index) {
        if (history_[index].result.resultId != resultId) continue;

        const bool deletingActive = activeIndex_ && *activeIndex_ == index;
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(index));
        resetLocalExportApproval();
        if (deletingActive) {
            activeIndex_.reset();
            submission_ = SubmissionSession{};
        } else if (activeIndex_ && *activeIndex_ > index) {
            --(*activeIndex_);
        }
        return {};
    }
    return fail(ShareModelCode::ResultNotFound,
                "the requested local compatibility result does not exist");
}

ShareModelDiagnostic CompatibilityShareModel::clearLocalResults() {
    history_.clear();
    activeIndex_.reset();
    submission_ = SubmissionSession{};
    resetLocalExportApproval();
    return {};
}

std::string_view compatibilityShareStateName(CompatibilityShareState value) noexcept {
    switch (value) {
        case CompatibilityShareState::TestNotRun: return "TestNotRun";
        case CompatibilityShareState::LocalResultAvailable: return "LocalResultAvailable";
        case CompatibilityShareState::PreviewReady: return "PreviewReady";
        case CompatibilityShareState::UserDeclined: return "UserDeclined";
        case CompatibilityShareState::SubmitPending: return "SubmitPending";
        case CompatibilityShareState::SubmitSucceeded: return "SubmitSucceeded";
        case CompatibilityShareState::SubmitFailed: return "SubmitFailed";
        case CompatibilityShareState::Superseded: return "Superseded";
    }
    return "Unknown";
}

std::string_view shareModelCodeName(ShareModelCode value) noexcept {
    switch (value) {
        case ShareModelCode::Success: return "Success";
        case ShareModelCode::InvalidResult: return "InvalidResult";
        case ShareModelCode::NoActiveResult: return "NoActiveResult";
        case ShareModelCode::InvalidState: return "InvalidState";
        case ShareModelCode::PreviewRequired: return "PreviewRequired";
        case ShareModelCode::SubmissionFailed: return "SubmissionFailed";
        case ShareModelCode::InvalidPrivacySettings: return "InvalidPrivacySettings";
        case ShareModelCode::SharingDisabled: return "SharingDisabled";
        case ShareModelCode::ResultNotFound: return "ResultNotFound";
        case ShareModelCode::ExportFailed: return "ExportFailed";
        case ShareModelCode::InvalidLocalHistory: return "InvalidLocalHistory";
        case ShareModelCode::LocalHistoryTooLarge: return "LocalHistoryTooLarge";
        case ShareModelCode::StaleApproval: return "StaleApproval";
        case ShareModelCode::PayloadIdentityMismatch: return "PayloadIdentityMismatch";
    }
    return "Unknown";
}

} // namespace hydra::community
