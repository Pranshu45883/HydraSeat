#include "hydra/setup_package.hpp"

#include "hydra/instance_materialization.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hydra::portable {
namespace {

static_assert(kMaximumPortableMaterializationStepsPerInstance ==
              materialization::kMaximumCompatibilityRecipeSteps);
static_assert(kMaximumPortableMutableFilesPerInstance ==
              materialization::kMaximumMutableFilesPerRecipe);
static_assert(kMaximumPortableMutableFileBytes ==
              materialization::kMaximumSingleMutableFileBytes);
static_assert(kMaximumPortableMutableBytesPerInstance ==
              materialization::kMaximumMutableBytesPerInstance);

constexpr std::string_view kHeader = "HYDRASEAT_SETUP_PACKAGE\n";

PackageDiagnostic fail(PackageResult result, std::string message) {
    return {result, std::move(message)};
}

bool validOpaqueId(std::string_view value) noexcept {
    if (value.empty() || value.size() > profile::kMaximumIdentifierBytes) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (!(std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == ':')) {
            return false;
        }
    }
    return true;
}

bool validProvenance(const Provenance& provenance) noexcept {
    return validOpaqueId(provenance.sourceId) && provenance.sourceRevision != 0u &&
           validOpaqueId(provenance.exportedBy);
}

bool hasWideNul(std::wstring_view value) noexcept {
    return value.find(L'\0') != std::wstring_view::npos;
}

bool wideToUtf8(std::wstring_view input, std::string& output) noexcept {
    try {
        std::string encoded;
        encoded.reserve(input.size());
        const auto append = [&](std::uint32_t codePoint) {
            if (codePoint <= 0x7fu) {
                encoded.push_back(static_cast<char>(codePoint));
            } else if (codePoint <= 0x7ffu) {
                encoded.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
                encoded.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
            } else if (codePoint <= 0xffffu) {
                encoded.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
                encoded.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
                encoded.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
            } else {
                encoded.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
                encoded.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
                encoded.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
                encoded.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
            }
        };
        for (std::size_t index = 0u; index < input.size(); ++index) {
            std::uint32_t codePoint = static_cast<std::uint32_t>(input[index]);
            if constexpr (sizeof(wchar_t) == 2u) {
                if (codePoint >= 0xd800u && codePoint <= 0xdbffu) {
                    if (++index >= input.size()) return false;
                    const auto low = static_cast<std::uint32_t>(input[index]);
                    if (low < 0xdc00u || low > 0xdfffu) return false;
                    codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u) +
                                (low - 0xdc00u);
                } else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu) {
                    return false;
                }
            } else if (codePoint > 0x10ffffu ||
                       (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
                return false;
            }
            append(codePoint);
        }
        output = std::move(encoded);
        return true;
    } catch (...) {
        return false;
    }
}

bool utf8ToWide(std::string_view input, std::wstring& output) noexcept {
    try {
        std::wstring decoded;
        decoded.reserve(input.size());
        std::size_t position = 0u;
        while (position < input.size()) {
            const auto first = static_cast<unsigned char>(input[position++]);
            std::uint32_t codePoint = 0u;
            std::size_t continuation = 0u;
            if (first <= 0x7fu) {
                codePoint = first;
            } else if ((first & 0xe0u) == 0xc0u) {
                codePoint = first & 0x1fu;
                continuation = 1u;
                if (codePoint == 0u) return false;
            } else if ((first & 0xf0u) == 0xe0u) {
                codePoint = first & 0x0fu;
                continuation = 2u;
            } else if ((first & 0xf8u) == 0xf0u) {
                codePoint = first & 0x07u;
                continuation = 3u;
            } else {
                return false;
            }
            if (position + continuation > input.size()) return false;
            for (std::size_t index = 0u; index < continuation; ++index) {
                const auto next = static_cast<unsigned char>(input[position++]);
                if ((next & 0xc0u) != 0x80u) return false;
                codePoint = (codePoint << 6u) | (next & 0x3fu);
            }
            if ((continuation == 1u && codePoint < 0x80u) ||
                (continuation == 2u && codePoint < 0x800u) ||
                (continuation == 3u && codePoint < 0x10000u) ||
                codePoint > 0x10ffffu ||
                (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
                return false;
            }
            if constexpr (sizeof(wchar_t) == 2u) {
                if (codePoint <= 0xffffu) {
                    decoded.push_back(static_cast<wchar_t>(codePoint));
                } else {
                    codePoint -= 0x10000u;
                    decoded.push_back(static_cast<wchar_t>(0xd800u + (codePoint >> 10u)));
                    decoded.push_back(static_cast<wchar_t>(0xdc00u + (codePoint & 0x3ffu)));
                }
            } else {
                decoded.push_back(static_cast<wchar_t>(codePoint));
            }
        }
        output = std::move(decoded);
        return true;
    } catch (...) {
        return false;
    }
}

bool isAsciiAlpha(wchar_t value) noexcept {
    return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z');
}

bool isPathSeparator(wchar_t value) noexcept {
    return value == L'\\' || value == L'/';
}

bool windowsReservedPathComponent(std::wstring_view value) {
    const auto dot = value.find(L'.');
    auto base = std::wstring(value.substr(0u, dot));
    for (auto& ch : base) {
        if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
    }
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") {
        return true;
    }
    if (base.size() == 4u && base[3] >= L'1' && base[3] <= L'9') {
        return base.substr(0u, 3u) == L"COM" || base.substr(0u, 3u) == L"LPT";
    }
    return false;
}

bool validWindowsComponent(std::wstring_view component) {
    if (component.empty() || component == L"." || component == L".." ||
        component.back() == L'.' || component.back() == L' ' ||
        windowsReservedPathComponent(component)) {
        return false;
    }
    for (const wchar_t ch : component) {
        if (ch < 32 || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' ||
            ch == L'<' || ch == L'>' || ch == L'|') {
            return false;
        }
    }
    return true;
}

bool safeAbsoluteLocalPath(std::wstring_view path) {
    if (path.empty() || path.size() > profile::kMaximumPathCodeUnits || hasWideNul(path)) {
        return false;
    }
    std::string ignored;
    if (!wideToUtf8(path, ignored)) return false;

    const bool drivePath = path.size() >= 3u && isAsciiAlpha(path[0]) &&
                           path[1] == L':' && isPathSeparator(path[2]);
    const bool uncPath = path.size() >= 5u && isPathSeparator(path[0]) &&
                         isPathSeparator(path[1]) && path[0] == path[1];
    if (!drivePath && !uncPath) return false;

    std::size_t position = drivePath ? 3u : 2u;
    std::size_t componentCount = 0u;
    while (position < path.size()) {
        const auto next = path.find_first_of(L"\\/", position);
        const auto end = next == std::wstring_view::npos ? path.size() : next;
        const auto component = path.substr(position, end - position);
        if (!validWindowsComponent(component)) return false;
        ++componentCount;
        if (next == std::wstring_view::npos) break;
        position = next + 1u;
        if (position == path.size() || isPathSeparator(path[position])) return false;
    }
    return !uncPath || componentCount >= 2u;
}

bool validPortableRelativePath(std::wstring_view path, std::wstring& key) {
    key.clear();
    if (path.empty() || path.size() > profile::kMaximumPathCodeUnits || hasWideNul(path) ||
        path.front() == L'/' || path.find(L'\\') != std::wstring_view::npos) {
        return false;
    }
    std::string ignored;
    if (!wideToUtf8(path, ignored)) return false;

    std::size_t position = 0u;
    while (position < path.size()) {
        const auto next = path.find(L'/', position);
        const auto end = next == std::wstring_view::npos ? path.size() : next;
        const auto component = path.substr(position, end - position);
        if (!validWindowsComponent(component)) return false;
        if (!key.empty()) key.push_back(L'/');
        for (wchar_t ch : component) {
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
            key.push_back(ch);
        }
        if (next == std::wstring_view::npos) break;
        position = next + 1u;
        if (position == path.size()) return false;
    }
    return !key.empty();
}

bool pathPrefixConflict(std::wstring_view left, std::wstring_view right) noexcept {
    if (left == right) return true;
    const auto prefix = [](std::wstring_view shorter, std::wstring_view longer) {
        return longer.size() > shorter.size() &&
               longer.substr(0u, shorter.size()) == shorter &&
               longer[shorter.size()] == L'/';
    };
    return prefix(left, right) || prefix(right, left);
}

bool setupStringsContainNul(const profile::TwoPlayerSetup& setup) noexcept {
    if (hasWideNul(setup.displayName)) return true;
    for (const auto& instance : setup.instances) {
        for (const auto& argument : instance.arguments) {
            if (hasWideNul(argument)) return true;
        }
        if ((instance.workingDirectory && hasWideNul(*instance.workingDirectory)) ||
            (instance.dataRoot && hasWideNul(*instance.dataRoot))) {
            return true;
        }
    }
    return false;
}

bool setupStringsAreValidUnicode(const profile::TwoPlayerSetup& setup) noexcept {
    std::string ignored;
    if (!wideToUtf8(setup.displayName, ignored)) return false;
    for (const auto& instance : setup.instances) {
        for (const auto& argument : instance.arguments) {
            if (!wideToUtf8(argument, ignored)) return false;
        }
        if ((instance.workingDirectory && !wideToUtf8(*instance.workingDirectory, ignored)) ||
            (instance.dataRoot && !wideToUtf8(*instance.dataRoot, ignored))) {
            return false;
        }
    }
    return true;
}

PackageDiagnostic validateSourceSetupPaths(const profile::TwoPlayerSetup& setup) {
    if (setupStringsContainNul(setup)) {
        return fail(PackageResult::InvalidSetup,
                    "portable setup strings cannot contain embedded NUL code units");
    }
    if (!setupStringsAreValidUnicode(setup)) {
        return fail(PackageResult::InvalidSetup,
                    "portable setup contains invalid Unicode code points");
    }
    for (const auto& instance : setup.instances) {
        if (instance.workingDirectory && !safeAbsoluteLocalPath(*instance.workingDirectory)) {
            return fail(PackageResult::InvalidSetup,
                        "setup working directory contains traversal/device/invalid path semantics");
        }
        if (instance.dataRoot && !safeAbsoluteLocalPath(*instance.dataRoot)) {
            return fail(PackageResult::InvalidSetup,
                        "setup data root contains traversal/device/invalid path semantics");
        }
    }
    return {};
}

std::wstring placeholder(std::string_view name) {
    std::wstring value = L"${";
    value.reserve(name.size() + 3u);
    for (const char ch : name) {
        value.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    value.push_back(L'}');
    return value;
}

bool placeholderMatches(std::wstring_view value, std::string_view name) {
    return value == placeholder(name);
}

const std::optional<std::wstring>* pathField(
    const profile::TwoPlayerSetup& setup,
    const PathVariable& variable) noexcept {
    if (variable.instanceIndex >= setup.instances.size()) return nullptr;
    const auto& recipe = setup.instances[variable.instanceIndex];
    switch (variable.field) {
    case PathField::WorkingDirectory: return &recipe.workingDirectory;
    case PathField::DataRoot: return &recipe.dataRoot;
    }
    return nullptr;
}

std::optional<std::wstring>* pathField(profile::TwoPlayerSetup& setup,
                                       const PathVariable& variable) noexcept {
    if (variable.instanceIndex >= setup.instances.size()) return nullptr;
    auto& recipe = setup.instances[variable.instanceIndex];
    switch (variable.field) {
    case PathField::WorkingDirectory: return &recipe.workingDirectory;
    case PathField::DataRoot: return &recipe.dataRoot;
    }
    return nullptr;
}

PackageDiagnostic validateMaterializations(const SetupPackage& package) {
    if (package.version == kLegacySetupPackageVersion &&
        !package.instanceMaterializations.empty()) {
        return fail(PackageResult::UnsupportedSemantic,
                    "legacy setup package version cannot carry phase/materialization semantics");
    }
    if (package.instanceMaterializations.size() > kMaximumPortableInstanceMaterializations) {
        return fail(PackageResult::InvalidPackage,
                    "portable setup contains too many instance materialization descriptors");
    }

    std::set<std::uint32_t> instanceIndexes;
    for (const auto& materialization : package.instanceMaterializations) {
        if (materialization.instanceIndex >= 2u ||
            !instanceIndexes.insert(materialization.instanceIndex).second) {
            return fail(PackageResult::InvalidPackage,
                        "portable instance materialization index is invalid or duplicated");
        }
        if (materialization.steps.empty() ||
            materialization.steps.size() > kMaximumPortableMaterializationStepsPerInstance) {
            return fail(PackageResult::InvalidPackage,
                        "portable instance materialization must contain a bounded nonempty step list");
        }

        std::set<std::string> stepIds;
        std::vector<std::wstring> destinationKeys;
        std::size_t fileCount = 0u;
        std::uint64_t totalMaximum = 0u;
        for (const auto& step : materialization.steps) {
            if (!validOpaqueId(step.stepId) || !stepIds.insert(step.stepId).second) {
                return fail(PackageResult::InvalidPackage,
                            "portable materialization step identity is invalid or duplicated");
            }
            const auto phase = static_cast<std::uint8_t>(step.phase);
            if (phase > static_cast<std::uint8_t>(setup::RecipeExecutionPhase::Runtime)) {
                return fail(PackageResult::UnsupportedSemantic,
                            "portable materialization declares an unsupported execution phase");
            }
            if (step.scope != setup::MutationScope::SeatWritableInstance) {
                return fail(PackageResult::UnsupportedSemantic,
                            "portable package cannot authorize shared-installation mutation");
            }
            if (step.files.empty()) {
                return fail(PackageResult::InvalidPackage,
                            "portable materialization step must contain at least one file");
            }
            if (step.files.size() > kMaximumPortableMutableFilesPerInstance ||
                fileCount > kMaximumPortableMutableFilesPerInstance - step.files.size()) {
                return fail(PackageResult::InvalidPackage,
                            "portable materialization exceeds the bounded mutable-file count");
            }
            fileCount += step.files.size();

            for (const auto& file : step.files) {
                if (file.maximumBytes == 0u ||
                    file.maximumBytes > kMaximumPortableMutableFileBytes ||
                    totalMaximum > kMaximumPortableMutableBytesPerInstance - file.maximumBytes) {
                    return fail(PackageResult::InvalidPackage,
                                "portable mutable-file byte limits exceed the bounded maximum");
                }
                totalMaximum += file.maximumBytes;
                std::wstring sourceKey;
                std::wstring destinationKey;
                if (!validPortableRelativePath(file.sourceRelativePath, sourceKey) ||
                    !validPortableRelativePath(file.destinationRelativePath, destinationKey)) {
                    return fail(PackageResult::InvalidPackage,
                                "portable mutable-file path is absolute, traversing, malformed, or non-portable");
                }
                for (const auto& existing : destinationKeys) {
                    if (pathPrefixConflict(existing, destinationKey)) {
                        return fail(PackageResult::InvalidPackage,
                                    "portable materialization contains conflicting destination paths");
                    }
                }
                destinationKeys.push_back(std::move(destinationKey));
            }
        }
    }
    return {};
}

PackageDiagnostic validatePackageStructure(const SetupPackage& package) {
    if (!isSupportedSetupPackageVersion(package.version)) {
        return fail(PackageResult::UnsupportedVersion, "unsupported setup package version");
    }
    if (!validProvenance(package.provenance)) {
        return fail(PackageResult::InvalidProvenance,
                    "package provenance must use bounded opaque identifiers and nonzero revision");
    }
    if (package.pathVariables.size() > kMaximumPathVariables) {
        return fail(PackageResult::InvalidPackage, "too many portable path variables");
    }

    profile::TwoPlayerSetupDocument setupDocument;
    setupDocument.setups = {package.redactedSetup};
    const auto schema = profile::validateTwoPlayerSetupDocument(setupDocument);
    if (!schema.succeeded()) {
        return fail(PackageResult::InvalidPackage,
                    "portable setup schema is invalid: " + schema.message);
    }
    if (setupStringsContainNul(package.redactedSetup)) {
        return fail(PackageResult::InvalidPackage,
                    "portable setup strings cannot contain embedded NUL code units");
    }
    if (!setupStringsAreValidUnicode(package.redactedSetup)) {
        return fail(PackageResult::InvalidPackage,
                    "portable setup contains invalid Unicode code points");
    }

    std::set<std::string> names;
    std::set<std::pair<std::uint32_t, std::uint8_t>> fields;
    for (const auto& variable : package.pathVariables) {
        if (!validOpaqueId(variable.variableName) || variable.instanceIndex >= 2u) {
            return fail(PackageResult::InvalidPackage,
                        "path variable identity or instance index is invalid");
        }
        const auto fieldValue = static_cast<std::uint8_t>(variable.field);
        if (fieldValue > static_cast<std::uint8_t>(PathField::DataRoot)) {
            return fail(PackageResult::InvalidPackage, "unknown path variable field");
        }
        if (!names.insert(variable.variableName).second ||
            !fields.insert({variable.instanceIndex, fieldValue}).second) {
            return fail(PackageResult::InvalidPackage,
                        "portable path variables must be unique by identity and field");
        }
        const auto* field = pathField(package.redactedSetup, variable);
        if (field == nullptr || !field->has_value() ||
            !placeholderMatches(**field, variable.variableName)) {
            return fail(PackageResult::InvalidPackage,
                        "portable setup path does not match its declared typed variable");
        }
    }

    for (std::size_t index = 0; index < package.redactedSetup.instances.size(); ++index) {
        const auto& recipe = package.redactedSetup.instances[index];
        const std::array<std::pair<PathField, const std::optional<std::wstring>*>, 2> values{{
            {PathField::WorkingDirectory, &recipe.workingDirectory},
            {PathField::DataRoot, &recipe.dataRoot},
        }};
        for (const auto& [fieldKind, value] : values) {
            if (!value->has_value()) continue;
            const auto& path = **value;
            if (path.size() < 4u || path.rfind(L"${", 0u) != 0u || path.back() != L'}') {
                return fail(PackageResult::InvalidPackage,
                            "portable package contains an unredacted local path");
            }
            bool declared = false;
            for (const auto& variable : package.pathVariables) {
                if (variable.instanceIndex == index && variable.field == fieldKind &&
                    placeholderMatches(path, variable.variableName)) {
                    declared = true;
                    break;
                }
            }
            if (!declared) {
                return fail(PackageResult::InvalidPackage,
                            "portable path placeholder has no typed variable declaration");
            }
        }
    }

    return validateMaterializations(package);
}

void appendLine(std::string& output, std::string_view label, std::uint64_t value) {
    output.append(label);
    output.push_back(' ');
    output.append(std::to_string(value));
    output.push_back('\n');
}

void appendBlob(std::string& output, std::string_view label, std::string_view value) {
    appendLine(output, label, static_cast<std::uint64_t>(value.size()));
    output.append(value);
    output.push_back('\n');
}

class Reader final {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    bool line(std::string_view& output) noexcept {
        if (position_ > bytes_.size()) return false;
        const auto end = bytes_.find('\n', position_);
        if (end == std::string_view::npos) return false;
        output = bytes_.substr(position_, end - position_);
        position_ = end + 1u;
        return true;
    }

    bool blob(std::size_t size, std::string_view& output) noexcept {
        if (position_ > bytes_.size() || size > bytes_.size() - position_) return false;
        if (position_ + size >= bytes_.size()) return false;
        output = bytes_.substr(position_, size);
        position_ += size;
        if (bytes_[position_] != '\n') return false;
        ++position_;
        return true;
    }

    bool done() const noexcept { return position_ == bytes_.size(); }

private:
    std::string_view bytes_;
    std::size_t position_{0};
};

bool parseNumber(std::string_view text, std::uint64_t& output) noexcept {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    output = value;
    return true;
}

bool parseLabeledNumber(std::string_view line,
                        std::string_view label,
                        std::uint64_t& output) noexcept {
    if (line.size() <= label.size() + 1u || line.substr(0u, label.size()) != label ||
        line[label.size()] != ' ') {
        return false;
    }
    return parseNumber(line.substr(label.size() + 1u), output);
}

bool readBlob(Reader& reader,
              std::string_view expectedLabel,
              std::string_view& output) noexcept {
    std::string_view line;
    std::uint64_t length = 0;
    if (!reader.line(line) || !parseLabeledNumber(line, expectedLabel, length) ||
        length > static_cast<std::uint64_t>(kMaximumSetupPackageBytes) ||
        length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    return reader.blob(static_cast<std::size_t>(length), output);
}

PackageDiagnostic importSetupInternal(const SetupPackage& package,
                                      const profile::GameRecord& localGame,
                                      std::span<const PathBinding> bindings,
                                      ImportedSetup& output) {
    const auto validation = validatePackageStructure(package);
    if (!validation.succeeded()) return validation;
    if (package.redactedSetup.gameId != localGame.gameId) {
        return fail(PackageResult::InvalidSetup,
                    "portable setup does not match the selected local Game");
    }

    std::set<std::string> seenBindings;
    for (const auto& binding : bindings) {
        if (!validOpaqueId(binding.variableName) ||
            !seenBindings.insert(binding.variableName).second) {
            return fail(PackageResult::DuplicateBinding,
                        "local path bindings must use unique declared variables");
        }
        const auto declared = std::find_if(
            package.pathVariables.begin(), package.pathVariables.end(),
            [&binding](const auto& variable) {
                return variable.variableName == binding.variableName;
            });
        if (declared == package.pathVariables.end()) {
            return fail(PackageResult::UnexpectedBinding,
                        "local path binding was not declared by the portable package");
        }
        if (!safeAbsoluteLocalPath(binding.localPath)) {
            return fail(PackageResult::InvalidLocalPath,
                        "local path binding must be a bounded traversal-free absolute Windows path");
        }
    }
    if (bindings.size() != package.pathVariables.size()) {
        return fail(PackageResult::MissingBinding,
                    "every portable path variable requires explicit local remapping");
    }

    profile::TwoPlayerSetup imported = package.redactedSetup;
    for (const auto& variable : package.pathVariables) {
        const auto binding = std::find_if(
            bindings.begin(), bindings.end(),
            [&variable](const auto& candidate) {
                return candidate.variableName == variable.variableName;
            });
        if (binding == bindings.end()) {
            return fail(PackageResult::MissingBinding,
                        "portable path variable is missing a local binding");
        }
        auto* field = pathField(imported, variable);
        if (field == nullptr) {
            return fail(PackageResult::InvalidPackage,
                        "portable path variable points outside the two instances");
        }
        *field = binding->localPath;
    }

    const auto setupValidation = setup::validateSetup(imported, localGame);
    if (!setupValidation.succeeded()) {
        const auto result = setupValidation.code == setup::SetupIssueCode::InvalidPath
                                ? PackageResult::InvalidLocalPath
                                : PackageResult::InvalidSetup;
        return fail(result, setupValidation.message);
    }
    const auto localPathValidation = validateSourceSetupPaths(imported);
    if (!localPathValidation.succeeded()) {
        return fail(localPathValidation.result == PackageResult::InvalidSetup
                        ? PackageResult::InvalidLocalPath
                        : localPathValidation.result,
                    localPathValidation.message);
    }

    ImportedSetup importedBundle;
    importedBundle.setup = std::move(imported);
    importedBundle.instanceMaterializations = package.instanceMaterializations;
    output = std::move(importedBundle);
    return {};
}

} // namespace

PackageDiagnostic exportSetup(const profile::TwoPlayerSetup& setup,
                              const profile::GameRecord& game,
                              Provenance provenance,
                              SetupPackage& output) {
    return exportSetup(setup, game, std::move(provenance),
                       std::span<const PortableInstanceMaterialization>{}, output);
}

PackageDiagnostic exportSetup(
    const profile::TwoPlayerSetup& setup,
    const profile::GameRecord& game,
    Provenance provenance,
    std::span<const PortableInstanceMaterialization> instanceMaterializations,
    SetupPackage& output) {
    try {
        if (!validProvenance(provenance)) {
            return fail(PackageResult::InvalidProvenance,
                        "export provenance is missing or invalid");
        }
        const auto setupValidation = setup::validateSetup(setup, game);
        if (!setupValidation.succeeded()) {
            return fail(PackageResult::InvalidSetup, setupValidation.message);
        }
        const auto pathValidation = validateSourceSetupPaths(setup);
        if (!pathValidation.succeeded()) return pathValidation;

        SetupPackage package;
        package.version = kSetupPackageVersion;
        package.provenance = std::move(provenance);
        package.redactedSetup = setup;
        package.instanceMaterializations.assign(instanceMaterializations.begin(),
                                                instanceMaterializations.end());
        for (std::size_t index = 0; index < package.redactedSetup.instances.size(); ++index) {
            auto& recipe = package.redactedSetup.instances[index];
            if (recipe.workingDirectory) {
                PathVariable variable;
                variable.variableName = "WORKING_DIRECTORY_" + std::to_string(index);
                variable.instanceIndex = static_cast<std::uint32_t>(index);
                variable.field = PathField::WorkingDirectory;
                recipe.workingDirectory = placeholder(variable.variableName);
                package.pathVariables.push_back(std::move(variable));
            }
            if (recipe.dataRoot) {
                PathVariable variable;
                variable.variableName = "DATA_ROOT_" + std::to_string(index);
                variable.instanceIndex = static_cast<std::uint32_t>(index);
                variable.field = PathField::DataRoot;
                recipe.dataRoot = placeholder(variable.variableName);
                package.pathVariables.push_back(std::move(variable));
            }
        }

        const auto validation = validatePackageStructure(package);
        if (!validation.succeeded()) return validation;
        output = std::move(package);
        return {};
    } catch (...) {
        return fail(PackageResult::InvalidPackage, "setup export failed unexpectedly");
    }
}

PackageDiagnostic importSetup(const SetupPackage& package,
                              const profile::GameRecord& localGame,
                              std::span<const PathBinding> bindings,
                              ImportedSetup& output) {
    try {
        return importSetupInternal(package, localGame, bindings, output);
    } catch (...) {
        return fail(PackageResult::InvalidPackage, "setup import failed unexpectedly");
    }
}

PackageDiagnostic importSetup(const SetupPackage& package,
                              const profile::GameRecord& localGame,
                              std::span<const PathBinding> bindings,
                              profile::TwoPlayerSetup& output) {
    try {
        const auto validation = validatePackageStructure(package);
        if (!validation.succeeded()) return validation;
        if (!package.instanceMaterializations.empty()) {
            return fail(PackageResult::UnsupportedSemantic,
                        "typed import is required to preserve package materialization semantics");
        }
        ImportedSetup imported;
        const auto diagnostic = importSetupInternal(package, localGame, bindings, imported);
        if (!diagnostic.succeeded()) return diagnostic;
        output = std::move(imported.setup);
        return {};
    } catch (...) {
        return fail(PackageResult::InvalidPackage, "setup import failed unexpectedly");
    }
}

PackageDiagnostic encodePackage(const SetupPackage& package, std::string& output) {
    try {
        const auto validation = validatePackageStructure(package);
        if (!validation.succeeded()) return validation;

        profile::TwoPlayerSetupDocument document;
        document.setups = {package.redactedSetup};
        profile::SchemaDiagnostic schema;
        const auto setupJson = profile::encodeTwoPlayerSetupDocument(document, &schema);
        if (!schema.succeeded()) {
            return fail(PackageResult::InvalidPackage,
                        "portable setup JSON encoding failed: " + schema.message);
        }

        std::string encoded;
        encoded.reserve(setupJson.size() + 1024u);
        encoded.append(kHeader);
        appendLine(encoded, "VERSION", package.version);
        appendBlob(encoded, "SOURCE", package.provenance.sourceId);
        appendLine(encoded, "SOURCE_REVISION", package.provenance.sourceRevision);
        appendBlob(encoded, "EXPORTED_BY", package.provenance.exportedBy);
        appendBlob(encoded, "SETUP_JSON", setupJson);
        appendLine(encoded, "VARIABLES",
                   static_cast<std::uint64_t>(package.pathVariables.size()));
        for (const auto& variable : package.pathVariables) {
            appendLine(encoded, "INSTANCE", variable.instanceIndex);
            appendLine(encoded, "FIELD", static_cast<std::uint8_t>(variable.field));
            appendBlob(encoded, "VARIABLE_NAME", variable.variableName);
        }

        if (package.version == kSetupPackageVersion) {
            appendLine(encoded, "MATERIALIZATIONS",
                       static_cast<std::uint64_t>(package.instanceMaterializations.size()));
            for (const auto& materialization : package.instanceMaterializations) {
                appendLine(encoded, "MATERIALIZATION_INSTANCE", materialization.instanceIndex);
                appendLine(encoded, "STEPS",
                           static_cast<std::uint64_t>(materialization.steps.size()));
                for (const auto& step : materialization.steps) {
                    appendBlob(encoded, "STEP_ID", step.stepId);
                    appendLine(encoded, "PHASE", static_cast<std::uint8_t>(step.phase));
                    appendLine(encoded, "SCOPE", static_cast<std::uint8_t>(step.scope));
                    appendLine(encoded, "FILES", static_cast<std::uint64_t>(step.files.size()));
                    for (const auto& file : step.files) {
                        std::string sourceUtf8;
                        std::string destinationUtf8;
                        if (!wideToUtf8(file.sourceRelativePath, sourceUtf8) ||
                            !wideToUtf8(file.destinationRelativePath, destinationUtf8)) {
                            return fail(PackageResult::InvalidPackage,
                                        "portable materialization path is not valid Unicode");
                        }
                        appendBlob(encoded, "SOURCE_RELATIVE", sourceUtf8);
                        appendBlob(encoded, "DESTINATION_RELATIVE", destinationUtf8);
                        appendLine(encoded, "MAXIMUM_BYTES", file.maximumBytes);
                    }
                }
            }
        }

        encoded.append("END\n");
        if (encoded.size() > kMaximumSetupPackageBytes) {
            return fail(PackageResult::PackageTooLarge,
                        "encoded setup package exceeds the bounded maximum");
        }
        output = std::move(encoded);
        return {};
    } catch (...) {
        return fail(PackageResult::InvalidPackage, "setup package encoding failed unexpectedly");
    }
}

PackageDiagnostic decodePackage(std::string_view bytes, SetupPackage& output) {
    try {
        if (bytes.size() > kMaximumSetupPackageBytes) {
            return fail(PackageResult::PackageTooLarge,
                        "setup package exceeds the bounded maximum");
        }
        if (bytes.size() < kHeader.size() || bytes.substr(0u, kHeader.size()) != kHeader) {
            return fail(PackageResult::InvalidPackage, "setup package header is invalid");
        }

        Reader reader(bytes.substr(kHeader.size()));
        std::string_view line;
        std::uint64_t number = 0;
        if (!reader.line(line) || !parseLabeledNumber(line, "VERSION", number)) {
            return fail(PackageResult::InvalidPackage, "missing setup package version");
        }
        if (number > std::numeric_limits<std::uint32_t>::max() ||
            !isSupportedSetupPackageVersion(static_cast<std::uint32_t>(number))) {
            return fail(PackageResult::UnsupportedVersion, "unsupported setup package version");
        }

        SetupPackage decoded;
        decoded.version = static_cast<std::uint32_t>(number);
        std::string_view blob;
        if (!readBlob(reader, "SOURCE", blob)) {
            return fail(PackageResult::InvalidPackage, "invalid source provenance field");
        }
        decoded.provenance.sourceId.assign(blob);
        if (!reader.line(line) || !parseLabeledNumber(line, "SOURCE_REVISION", number)) {
            return fail(PackageResult::InvalidPackage, "invalid source revision field");
        }
        decoded.provenance.sourceRevision = number;
        if (!readBlob(reader, "EXPORTED_BY", blob)) {
            return fail(PackageResult::InvalidPackage, "invalid exporter provenance field");
        }
        decoded.provenance.exportedBy.assign(blob);
        if (!readBlob(reader, "SETUP_JSON", blob)) {
            return fail(PackageResult::InvalidPackage, "invalid portable setup JSON field");
        }

        profile::TwoPlayerSetupDocument setupDocument;
        const auto schema = profile::decodeTwoPlayerSetupDocument(blob, setupDocument);
        if (!schema.succeeded() || setupDocument.setups.size() != 1u) {
            return fail(PackageResult::InvalidPackage,
                        "portable package must contain exactly one valid setup");
        }
        decoded.redactedSetup = std::move(setupDocument.setups.front());

        if (!reader.line(line) || !parseLabeledNumber(line, "VARIABLES", number) ||
            number > kMaximumPathVariables) {
            return fail(PackageResult::InvalidPackage, "invalid path variable count");
        }
        decoded.pathVariables.reserve(static_cast<std::size_t>(number));
        for (std::uint64_t index = 0; index < number; ++index) {
            PathVariable variable;
            std::uint64_t instance = 0;
            std::uint64_t field = 0;
            if (!reader.line(line) || !parseLabeledNumber(line, "INSTANCE", instance) ||
                instance > std::numeric_limits<std::uint32_t>::max()) {
                return fail(PackageResult::InvalidPackage, "invalid path variable instance");
            }
            if (!reader.line(line) || !parseLabeledNumber(line, "FIELD", field) ||
                field > static_cast<std::uint64_t>(PathField::DataRoot)) {
                return fail(PackageResult::InvalidPackage, "invalid path variable field");
            }
            if (!readBlob(reader, "VARIABLE_NAME", blob)) {
                return fail(PackageResult::InvalidPackage, "invalid path variable identity");
            }
            variable.instanceIndex = static_cast<std::uint32_t>(instance);
            variable.field = static_cast<PathField>(field);
            variable.variableName.assign(blob);
            decoded.pathVariables.push_back(std::move(variable));
        }

        if (decoded.version == kSetupPackageVersion) {
            if (!reader.line(line) ||
                !parseLabeledNumber(line, "MATERIALIZATIONS", number) ||
                number > kMaximumPortableInstanceMaterializations) {
                return fail(PackageResult::InvalidPackage,
                            "invalid or missing materialization descriptor count");
            }
            decoded.instanceMaterializations.reserve(static_cast<std::size_t>(number));
            for (std::uint64_t materializationIndex = 0u;
                 materializationIndex < number; ++materializationIndex) {
                PortableInstanceMaterialization materialization;
                std::uint64_t instance = 0u;
                std::uint64_t stepCount = 0u;
                if (!reader.line(line) ||
                    !parseLabeledNumber(line, "MATERIALIZATION_INSTANCE", instance) ||
                    instance > std::numeric_limits<std::uint32_t>::max()) {
                    return fail(PackageResult::InvalidPackage,
                                "invalid materialization instance index");
                }
                if (!reader.line(line) || !parseLabeledNumber(line, "STEPS", stepCount) ||
                    stepCount > kMaximumPortableMaterializationStepsPerInstance) {
                    return fail(PackageResult::InvalidPackage,
                                "invalid materialization step count");
                }
                materialization.instanceIndex = static_cast<std::uint32_t>(instance);
                materialization.steps.reserve(static_cast<std::size_t>(stepCount));
                for (std::uint64_t stepIndex = 0u; stepIndex < stepCount; ++stepIndex) {
                    PortableCompatibilityStep step;
                    if (!readBlob(reader, "STEP_ID", blob)) {
                        return fail(PackageResult::InvalidPackage,
                                    "invalid materialization step identity");
                    }
                    step.stepId.assign(blob);
                    std::uint64_t phase = 0u;
                    std::uint64_t scope = 0u;
                    std::uint64_t fileCount = 0u;
                    if (!reader.line(line) || !parseLabeledNumber(line, "PHASE", phase) ||
                        phase > std::numeric_limits<std::uint8_t>::max()) {
                        return fail(PackageResult::InvalidPackage,
                                    "invalid materialization execution phase field");
                    }
                    if (!reader.line(line) || !parseLabeledNumber(line, "SCOPE", scope) ||
                        scope > std::numeric_limits<std::uint8_t>::max()) {
                        return fail(PackageResult::InvalidPackage,
                                    "invalid materialization scope field");
                    }
                    if (!reader.line(line) || !parseLabeledNumber(line, "FILES", fileCount) ||
                        fileCount > kMaximumPortableMutableFilesPerInstance) {
                        return fail(PackageResult::InvalidPackage,
                                    "invalid materialization file count");
                    }
                    step.phase = static_cast<setup::RecipeExecutionPhase>(phase);
                    step.scope = static_cast<setup::MutationScope>(scope);
                    step.files.reserve(static_cast<std::size_t>(fileCount));
                    for (std::uint64_t fileIndex = 0u; fileIndex < fileCount; ++fileIndex) {
                        PortableMutableFileSpec file;
                        if (!readBlob(reader, "SOURCE_RELATIVE", blob) ||
                            !utf8ToWide(blob, file.sourceRelativePath)) {
                            return fail(PackageResult::InvalidPackage,
                                        "invalid UTF-8 source-relative materialization path");
                        }
                        if (!readBlob(reader, "DESTINATION_RELATIVE", blob) ||
                            !utf8ToWide(blob, file.destinationRelativePath)) {
                            return fail(PackageResult::InvalidPackage,
                                        "invalid UTF-8 destination-relative materialization path");
                        }
                        if (!reader.line(line) ||
                            !parseLabeledNumber(line, "MAXIMUM_BYTES", file.maximumBytes)) {
                            return fail(PackageResult::InvalidPackage,
                                        "invalid materialization byte bound");
                        }
                        step.files.push_back(std::move(file));
                    }
                    materialization.steps.push_back(std::move(step));
                }
                decoded.instanceMaterializations.push_back(std::move(materialization));
            }
        }

        if (!reader.line(line) || line != "END" || !reader.done()) {
            return fail(PackageResult::InvalidPackage,
                        "setup package has trailing, unknown, or incomplete data");
        }

        const auto validation = validatePackageStructure(decoded);
        if (!validation.succeeded()) return validation;
        output = std::move(decoded);
        return {};
    } catch (...) {
        return fail(PackageResult::InvalidPackage, "setup package decoding failed unexpectedly");
    }
}

std::string_view packageResultName(PackageResult result) noexcept {
    switch (result) {
    case PackageResult::Success: return "Success";
    case PackageResult::InvalidSetup: return "InvalidSetup";
    case PackageResult::InvalidProvenance: return "InvalidProvenance";
    case PackageResult::InvalidPackage: return "InvalidPackage";
    case PackageResult::PackageTooLarge: return "PackageTooLarge";
    case PackageResult::UnsupportedVersion: return "UnsupportedVersion";
    case PackageResult::MissingBinding: return "MissingBinding";
    case PackageResult::DuplicateBinding: return "DuplicateBinding";
    case PackageResult::UnexpectedBinding: return "UnexpectedBinding";
    case PackageResult::InvalidLocalPath: return "InvalidLocalPath";
    case PackageResult::UnsupportedSemantic: return "UnsupportedSemantic";
    }
    return "Unknown";
}

std::string_view pathFieldName(PathField field) noexcept {
    switch (field) {
    case PathField::WorkingDirectory: return "WorkingDirectory";
    case PathField::DataRoot: return "DataRoot";
    }
    return "Unknown";
}

} // namespace hydra::portable
