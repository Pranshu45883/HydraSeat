#include "hydra/profile_cli.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hydra::cli {
namespace {

constexpr std::string_view kSnapshotHeader = "HYDRASEAT_PLAN_SNAPSHOT\n";

CliDiagnostic fail(CliResult result, std::string message) {
    return {result, std::move(message)};
}

bool validFormat(OutputFormat format) noexcept {
    return format == OutputFormat::Human || format == OutputFormat::Json;
}

bool validTargetKind(provider::LaunchTargetKind kind) noexcept {
    return kind == provider::LaunchTargetKind::Executable ||
           kind == provider::LaunchTargetKind::ProviderUri;
}

bool validIdentifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > profile::kMaximumIdentifierBytes) return false;
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
              ch == ':')) {
            return false;
        }
    }
    return true;
}

void appendUtf8CodePoint(std::uint32_t codePoint, std::string& output) {
    if (codePoint <= 0x7fu) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else if (codePoint <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
}

bool wideToUtf8(std::wstring_view input, std::string& output) {
    output.clear();
    try {
        output.reserve(input.size());
        for (std::size_t index = 0; index < input.size(); ++index) {
            std::uint32_t codePoint = static_cast<std::uint32_t>(input[index]);
            if constexpr (sizeof(wchar_t) == 2u) {
                if (codePoint >= 0xd800u && codePoint <= 0xdbffu) {
                    if (index + 1u >= input.size()) return false;
                    const auto low = static_cast<std::uint32_t>(input[index + 1u]);
                    if (low < 0xdc00u || low > 0xdfffu) return false;
                    codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u) + (low - 0xdc00u);
                    ++index;
                } else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu) {
                    return false;
                }
            }
            if (codePoint > 0x10ffffu || (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
                return false;
            }
            appendUtf8CodePoint(codePoint, output);
        }
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

bool readUtf8CodePoint(std::string_view input, std::size_t& index, std::uint32_t& codePoint) {
    if (index >= input.size()) return false;
    const auto first = static_cast<unsigned char>(input[index++]);
    if (first <= 0x7fu) {
        codePoint = first;
        return true;
    }
    std::uint32_t value = 0;
    std::size_t continuationCount = 0;
    if ((first & 0xe0u) == 0xc0u) {
        value = first & 0x1fu;
        continuationCount = 1u;
        if (value == 0u) return false;
    } else if ((first & 0xf0u) == 0xe0u) {
        value = first & 0x0fu;
        continuationCount = 2u;
    } else if ((first & 0xf8u) == 0xf0u) {
        value = first & 0x07u;
        continuationCount = 3u;
    } else {
        return false;
    }
    if (index + continuationCount > input.size()) return false;
    for (std::size_t count = 0; count < continuationCount; ++count) {
        const auto next = static_cast<unsigned char>(input[index++]);
        if ((next & 0xc0u) != 0x80u) return false;
        value = (value << 6u) | (next & 0x3fu);
    }
    if ((continuationCount == 1u && value < 0x80u) ||
        (continuationCount == 2u && value < 0x800u) ||
        (continuationCount == 3u && value < 0x10000u) || value > 0x10ffffu ||
        (value >= 0xd800u && value <= 0xdfffu)) {
        return false;
    }
    codePoint = value;
    return true;
}

bool utf8ToWide(std::string_view input, std::wstring& output) {
    output.clear();
    try {
        output.reserve(input.size());
        std::size_t index = 0;
        while (index < input.size()) {
            std::uint32_t codePoint = 0;
            if (!readUtf8CodePoint(input, index, codePoint)) return false;
            if constexpr (sizeof(wchar_t) == 2u) {
                if (codePoint <= 0xffffu) {
                    output.push_back(static_cast<wchar_t>(codePoint));
                } else {
                    const auto adjusted = codePoint - 0x10000u;
                    output.push_back(static_cast<wchar_t>(0xd800u + (adjusted >> 10u)));
                    output.push_back(static_cast<wchar_t>(0xdc00u + (adjusted & 0x3ffu)));
                }
            } else {
                output.push_back(static_cast<wchar_t>(codePoint));
            }
        }
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

void appendJsonString(std::string_view value, std::string& output) {
    output.push_back('"');
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
        case '"': output.append("\\\""); break;
        case '\\': output.append("\\\\"); break;
        case '\b': output.append("\\b"); break;
        case '\f': output.append("\\f"); break;
        case '\n': output.append("\\n"); break;
        case '\r': output.append("\\r"); break;
        case '\t': output.append("\\t"); break;
        default:
            if (ch < 0x20u) {
                constexpr char hex[] = "0123456789abcdef";
                output.append("\\u00");
                output.push_back(hex[(ch >> 4u) & 0x0fu]);
                output.push_back(hex[ch & 0x0fu]);
            } else {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    output.push_back('"');
}

void appendJsonWide(std::wstring_view value, std::string& output, bool& valid) {
    std::string utf8;
    if (!wideToUtf8(value, utf8)) {
        valid = false;
        return;
    }
    appendJsonString(utf8, output);
}

CliDiagnostic validateSnapshot(const PlanSnapshot& snapshot) {
    if (snapshot.version != kPlanSnapshotVersion) {
        return fail(CliResult::UnsupportedVersion, "unsupported plan snapshot version");
    }
    if (snapshot.fingerprint == 0u || snapshot.seats.empty() || snapshot.seats.size() > 2u) {
        return fail(CliResult::InvalidInput,
                    "plan snapshot requires a nonzero fingerprint and one or two Seats");
    }
    std::set<SeatId> seatIds;
    for (const auto& seat : snapshot.seats) {
        if ((seat.seatId != 1u && seat.seatId != 2u) || !seatIds.insert(seat.seatId).second ||
            !validIdentifier(seat.playerId) || !validIdentifier(seat.gameId) ||
            !validIdentifier(seat.providerId) ||
            (seat.setupId && !validIdentifier(*seat.setupId)) ||
            (seat.providerAppId && !validIdentifier(*seat.providerAppId)) ||
            seat.requirementRevision == 0u || seat.hardwareFingerprint == 0u ||
            seat.providerMetadataRevision == 0u || !validTargetKind(seat.targetKind) ||
            seat.target.empty() || seat.target.size() > provider::kMaximumLaunchUriCodeUnits ||
            seat.arguments.size() > provider::kMaximumLaunchArguments) {
            return fail(CliResult::InvalidInput, "plan snapshot contains invalid bounded fields");
        }
        for (const auto& argument : seat.arguments) {
            if (argument.size() > provider::kMaximumLaunchArgumentCodeUnits) {
                return fail(CliResult::BoundsExceeded,
                            "plan snapshot argument exceeds the bounded maximum");
            }
        }
        if (seat.workingDirectory &&
            seat.workingDirectory->size() > profile::kMaximumPathCodeUnits) {
            return fail(CliResult::BoundsExceeded,
                        "plan snapshot working directory exceeds the bounded maximum");
        }
    }
    return {};
}

void appendNumberLine(std::string& output, std::string_view label, std::uint64_t value) {
    output.append(label);
    output.push_back(' ');
    output.append(std::to_string(value));
    output.push_back('\n');
}

void appendBlob(std::string& output, std::string_view label, std::string_view value) {
    appendNumberLine(output, label, static_cast<std::uint64_t>(value.size()));
    output.append(value);
    output.push_back('\n');
}

bool parseNumber(std::string_view value, std::uint64_t& output) noexcept {
    if (value.empty()) return false;
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
    output = parsed;
    return true;
}

bool parseNumberLine(std::string_view line,
                     std::string_view label,
                     std::uint64_t& output) noexcept {
    if (line.size() <= label.size() || line.substr(0u, label.size()) != label ||
        line[label.size()] != ' ') {
        return false;
    }
    return parseNumber(line.substr(label.size() + 1u), output);
}

class SnapshotReader final {
public:
    explicit SnapshotReader(std::string_view bytes) : bytes_(bytes) {}

    bool line(std::string_view& output) noexcept {
        const auto end = bytes_.find('\n', position_);
        if (end == std::string_view::npos) return false;
        output = bytes_.substr(position_, end - position_);
        position_ = end + 1u;
        return true;
    }

    bool blob(std::size_t size, std::string_view& output) noexcept {
        if (position_ > bytes_.size() || size > bytes_.size() - position_ ||
            position_ + size >= bytes_.size()) {
            return false;
        }
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

bool readBlob(SnapshotReader& reader,
              std::string_view label,
              std::string_view& output) noexcept {
    std::string_view line;
    std::uint64_t size = 0;
    if (!reader.line(line) || !parseNumberLine(line, label, size) ||
        size > static_cast<std::uint64_t>(kMaximumPlanSnapshotBytes) ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    return reader.blob(static_cast<std::size_t>(size), output);
}

CliDiagnostic renderRedactedPlayerJson(const profile::PlayerProfileDocument& document,
                                       std::string& output) {
    auto redacted = document;
    for (auto& player : redacted.players) {
        for (auto& account : player.providerAccounts) account.accountRef = "redacted";
    }
    profile::SchemaDiagnostic diagnostic;
    const auto json = profile::encodePlayerProfileDocument(redacted, &diagnostic);
    if (!diagnostic.succeeded()) return fail(CliResult::InvalidInput, diagnostic.message);
    output = json;
    return {};
}

} // namespace

CliDiagnostic makePlanSnapshot(const plan::ProviderAwareLaunchPlan& plan,
                               PlanSnapshot& output) {
    try {
        if (plan.schemaVersion != plan::kProviderLaunchPlanSchemaVersion ||
            plan.fingerprint == 0u || plan.seats.empty() || plan.seats.size() > 2u) {
            return fail(CliResult::InvalidInput, "compiled plan is invalid for CLI snapshot export");
        }
        PlanSnapshot snapshot;
        snapshot.fingerprint = plan.fingerprint;
        snapshot.seats.reserve(plan.seats.size());
        for (const auto& seat : plan.seats) {
            PlanSeatSnapshot item;
            item.seatId = seat.seatId;
            item.playerId = seat.playerId;
            item.gameId = seat.gameId;
            item.setupId = seat.setupId;
            item.instanceIndex = seat.instanceIndex;
            item.requirementRevision = seat.requirementRevision;
            item.hardwareFingerprint = seat.hardwareFingerprint;
            item.providerId = seat.launchRequest.providerId;
            item.providerAppId = seat.launchRequest.providerAppId;
            item.providerMetadataRevision = seat.launchRequest.metadataRevision;
            item.targetKind = seat.launchRequest.targetKind;
            item.target = seat.launchRequest.target;
            item.arguments = seat.launchRequest.arguments;
            item.workingDirectory = seat.launchRequest.workingDirectory;
            item.accountReferenceSelected = seat.launchRequest.accountRef.has_value();
            item.protectedOrExperimental = seat.requirements.highRisk;
            snapshot.seats.push_back(std::move(item));
        }
        std::sort(snapshot.seats.begin(), snapshot.seats.end(), [](const auto& left,
                                                                  const auto& right) {
            return left.seatId < right.seatId;
        });
        const auto validation = validateSnapshot(snapshot);
        if (!validation.succeeded()) return validation;
        output = std::move(snapshot);
        return {};
    } catch (...) {
        return fail(CliResult::InvalidInput, "plan snapshot allocation failed");
    }
}

CliDiagnostic encodePlanSnapshot(const PlanSnapshot& snapshot, std::string& output) {
    try {
        const auto validation = validateSnapshot(snapshot);
        if (!validation.succeeded()) return validation;
        std::string encoded;
        encoded.append(kSnapshotHeader);
        appendNumberLine(encoded, "VERSION", snapshot.version);
        appendNumberLine(encoded, "FINGERPRINT", snapshot.fingerprint);
        appendNumberLine(encoded, "SEATS", static_cast<std::uint64_t>(snapshot.seats.size()));
        for (const auto& seat : snapshot.seats) {
            appendNumberLine(encoded, "SEAT", seat.seatId);
            appendBlob(encoded, "PLAYER", seat.playerId);
            appendBlob(encoded, "GAME", seat.gameId);
            appendNumberLine(encoded, "HAS_SETUP", seat.setupId ? 1u : 0u);
            if (seat.setupId) appendBlob(encoded, "SETUP", *seat.setupId);
            appendNumberLine(encoded, "INSTANCE", seat.instanceIndex);
            appendNumberLine(encoded, "REQUIREMENT_REVISION", seat.requirementRevision);
            appendNumberLine(encoded, "HARDWARE_FINGERPRINT", seat.hardwareFingerprint);
            appendBlob(encoded, "PROVIDER", seat.providerId);
            appendNumberLine(encoded, "HAS_APP", seat.providerAppId ? 1u : 0u);
            if (seat.providerAppId) appendBlob(encoded, "APP", *seat.providerAppId);
            appendNumberLine(encoded, "PROVIDER_REVISION", seat.providerMetadataRevision);
            appendNumberLine(encoded, "TARGET_KIND",
                             static_cast<std::uint8_t>(seat.targetKind));
            std::string target;
            if (!wideToUtf8(seat.target, target)) {
                return fail(CliResult::InvalidInput, "plan target contains invalid Unicode");
            }
            appendBlob(encoded, "TARGET", target);
            appendNumberLine(encoded, "ARGUMENTS",
                             static_cast<std::uint64_t>(seat.arguments.size()));
            for (const auto& argument : seat.arguments) {
                std::string utf8;
                if (!wideToUtf8(argument, utf8)) {
                    return fail(CliResult::InvalidInput,
                                "plan argument contains invalid Unicode");
                }
                appendBlob(encoded, "ARGUMENT", utf8);
            }
            appendNumberLine(encoded, "HAS_WORKDIR", seat.workingDirectory ? 1u : 0u);
            if (seat.workingDirectory) {
                std::string utf8;
                if (!wideToUtf8(*seat.workingDirectory, utf8)) {
                    return fail(CliResult::InvalidInput,
                                "plan working directory contains invalid Unicode");
                }
                appendBlob(encoded, "WORKDIR", utf8);
            }
            appendNumberLine(encoded, "ACCOUNT_SELECTED",
                             seat.accountReferenceSelected ? 1u : 0u);
            appendNumberLine(encoded, "PROTECTED",
                             seat.protectedOrExperimental ? 1u : 0u);
        }
        encoded.append("END\n");
        if (encoded.size() > kMaximumPlanSnapshotBytes) {
            return fail(CliResult::BoundsExceeded,
                        "encoded plan snapshot exceeds the bounded maximum");
        }
        output = std::move(encoded);
        return {};
    } catch (...) {
        return fail(CliResult::InvalidInput, "plan snapshot encoding failed");
    }
}

CliDiagnostic decodePlanSnapshot(std::string_view bytes, PlanSnapshot& output) {
    try {
        if (bytes.size() > kMaximumPlanSnapshotBytes) {
            return fail(CliResult::BoundsExceeded, "plan snapshot exceeds the bounded maximum");
        }
        if (bytes.size() < kSnapshotHeader.size() ||
            bytes.substr(0u, kSnapshotHeader.size()) != kSnapshotHeader) {
            return fail(CliResult::ParseError, "plan snapshot header is invalid");
        }
        SnapshotReader reader(bytes.substr(kSnapshotHeader.size()));
        std::string_view line;
        std::string_view blob;
        std::uint64_t number = 0;
        if (!reader.line(line) || !parseNumberLine(line, "VERSION", number)) {
            return fail(CliResult::ParseError, "plan snapshot version is missing");
        }
        if (number != kPlanSnapshotVersion) {
            return fail(CliResult::UnsupportedVersion, "unsupported plan snapshot version");
        }
        PlanSnapshot decoded;
        decoded.version = static_cast<std::uint32_t>(number);
        if (!reader.line(line) || !parseNumberLine(line, "FINGERPRINT", decoded.fingerprint) ||
            !reader.line(line) || !parseNumberLine(line, "SEATS", number) ||
            number == 0u || number > 2u) {
            return fail(CliResult::ParseError, "plan snapshot header fields are invalid");
        }
        decoded.seats.reserve(static_cast<std::size_t>(number));
        for (std::uint64_t seatIndex = 0; seatIndex < number; ++seatIndex) {
            PlanSeatSnapshot seat;
            std::uint64_t value = 0;
            if (!reader.line(line) || !parseNumberLine(line, "SEAT", value) ||
                value > std::numeric_limits<SeatId>::max()) {
                return fail(CliResult::ParseError, "invalid Seat in plan snapshot");
            }
            seat.seatId = static_cast<SeatId>(value);
            if (!readBlob(reader, "PLAYER", blob)) return fail(CliResult::ParseError, "invalid Player field");
            seat.playerId.assign(blob);
            if (!readBlob(reader, "GAME", blob)) return fail(CliResult::ParseError, "invalid Game field");
            seat.gameId.assign(blob);
            if (!reader.line(line) || !parseNumberLine(line, "HAS_SETUP", value) || value > 1u) {
                return fail(CliResult::ParseError, "invalid setup presence field");
            }
            if (value == 1u) {
                if (!readBlob(reader, "SETUP", blob)) return fail(CliResult::ParseError, "invalid setup field");
                seat.setupId = std::string(blob);
            }
            if (!reader.line(line) || !parseNumberLine(line, "INSTANCE", value) ||
                value > std::numeric_limits<std::uint32_t>::max()) {
                return fail(CliResult::ParseError, "invalid instance index");
            }
            seat.instanceIndex = static_cast<std::uint32_t>(value);
            if (!reader.line(line) ||
                !parseNumberLine(line, "REQUIREMENT_REVISION", seat.requirementRevision) ||
                !reader.line(line) ||
                !parseNumberLine(line, "HARDWARE_FINGERPRINT", seat.hardwareFingerprint)) {
                return fail(CliResult::ParseError, "invalid requirement/hardware revision fields");
            }
            if (!readBlob(reader, "PROVIDER", blob)) return fail(CliResult::ParseError, "invalid provider field");
            seat.providerId.assign(blob);
            if (!reader.line(line) || !parseNumberLine(line, "HAS_APP", value) || value > 1u) {
                return fail(CliResult::ParseError, "invalid provider app presence field");
            }
            if (value == 1u) {
                if (!readBlob(reader, "APP", blob)) return fail(CliResult::ParseError, "invalid provider app field");
                seat.providerAppId = std::string(blob);
            }
            if (!reader.line(line) ||
                !parseNumberLine(line, "PROVIDER_REVISION", seat.providerMetadataRevision) ||
                !reader.line(line) || !parseNumberLine(line, "TARGET_KIND", value) ||
                value > static_cast<std::uint64_t>(provider::LaunchTargetKind::ProviderUri)) {
                return fail(CliResult::ParseError, "invalid provider revision/target kind");
            }
            seat.targetKind = static_cast<provider::LaunchTargetKind>(value);
            if (!readBlob(reader, "TARGET", blob) || !utf8ToWide(blob, seat.target)) {
                return fail(CliResult::ParseError, "invalid target Unicode");
            }
            if (!reader.line(line) || !parseNumberLine(line, "ARGUMENTS", value) ||
                value > provider::kMaximumLaunchArguments) {
                return fail(CliResult::ParseError, "invalid argument count");
            }
            seat.arguments.reserve(static_cast<std::size_t>(value));
            for (std::uint64_t argumentIndex = 0; argumentIndex < value; ++argumentIndex) {
                std::wstring argument;
                if (!readBlob(reader, "ARGUMENT", blob) || !utf8ToWide(blob, argument)) {
                    return fail(CliResult::ParseError, "invalid argument Unicode");
                }
                seat.arguments.push_back(std::move(argument));
            }
            if (!reader.line(line) || !parseNumberLine(line, "HAS_WORKDIR", value) || value > 1u) {
                return fail(CliResult::ParseError, "invalid working-directory presence field");
            }
            if (value == 1u) {
                std::wstring working;
                if (!readBlob(reader, "WORKDIR", blob) || !utf8ToWide(blob, working)) {
                    return fail(CliResult::ParseError, "invalid working-directory Unicode");
                }
                seat.workingDirectory = std::move(working);
            }
            if (!reader.line(line) || !parseNumberLine(line, "ACCOUNT_SELECTED", value) ||
                value > 1u) {
                return fail(CliResult::ParseError, "invalid account-selection field");
            }
            seat.accountReferenceSelected = value == 1u;
            if (!reader.line(line) || !parseNumberLine(line, "PROTECTED", value) || value > 1u) {
                return fail(CliResult::ParseError, "invalid protection field");
            }
            seat.protectedOrExperimental = value == 1u;
            decoded.seats.push_back(std::move(seat));
        }
        if (!reader.line(line) || line != "END" || !reader.done()) {
            return fail(CliResult::ParseError, "plan snapshot has trailing/incomplete data");
        }
        std::sort(decoded.seats.begin(), decoded.seats.end(), [](const auto& left,
                                                                const auto& right) {
            return left.seatId < right.seatId;
        });
        const auto validation = validateSnapshot(decoded);
        if (!validation.succeeded()) return validation;
        output = std::move(decoded);
        return {};
    } catch (...) {
        return fail(CliResult::ParseError, "plan snapshot decoding failed");
    }
}

CliDiagnostic renderGames(const profile::GameRecordDocument& document,
                          OutputFormat format,
                          std::string& output) {
    if (!validFormat(format)) return fail(CliResult::InvalidInput, "unknown output format");
    const auto validation = profile::validateGameRecordDocument(document);
    if (!validation.succeeded()) return fail(CliResult::InvalidInput, validation.message);
    if (format == OutputFormat::Json) {
        profile::SchemaDiagnostic diagnostic;
        const auto json = profile::encodeGameRecordDocument(document, &diagnostic);
        if (!diagnostic.succeeded()) return fail(CliResult::InvalidInput, diagnostic.message);
        output = json;
        return {};
    }
    std::ostringstream human;
    human << "Games: " << document.games.size() << '\n';
    for (const auto& game : document.games) {
        human << "- " << game.gameId << " provider=" << game.providerId;
        if (game.providerAppId) human << " app=" << *game.providerAppId;
        human << " executables=" << game.executableCandidates.size() << '\n';
    }
    output = human.str();
    return {};
}

CliDiagnostic renderPlayers(const profile::PlayerProfileDocument& document,
                            OutputFormat format,
                            std::string& output) {
    if (!validFormat(format)) return fail(CliResult::InvalidInput, "unknown output format");
    const auto validation = profile::validatePlayerProfileDocument(document);
    if (!validation.succeeded()) return fail(CliResult::InvalidInput, validation.message);
    if (format == OutputFormat::Json) return renderRedactedPlayerJson(document, output);

    std::ostringstream human;
    human << "Players: " << document.players.size() << '\n';
    for (const auto& player : document.players) {
        human << "- " << player.playerId << " locale=" << player.preferredLocale
              << " provider_accounts=" << player.providerAccounts.size() << '\n';
        for (const auto& account : player.providerAccounts) {
            human << "  provider=" << account.providerId << " account=<redacted>\n";
        }
    }
    output = human.str();
    return {};
}

CliDiagnostic renderSetups(const profile::TwoPlayerSetupDocument& document,
                           OutputFormat format,
                           std::string& output) {
    if (!validFormat(format)) return fail(CliResult::InvalidInput, "unknown output format");
    const auto validation = profile::validateTwoPlayerSetupDocument(document);
    if (!validation.succeeded()) return fail(CliResult::InvalidInput, validation.message);
    if (format == OutputFormat::Json) {
        profile::SchemaDiagnostic diagnostic;
        const auto json = profile::encodeTwoPlayerSetupDocument(document, &diagnostic);
        if (!diagnostic.succeeded()) return fail(CliResult::InvalidInput, diagnostic.message);
        output = json;
        return {};
    }
    std::ostringstream human;
    human << "TwoPlayerSetups: " << document.setups.size() << '\n';
    for (const auto& setup : document.setups) {
        human << "- " << setup.setupId << " game=" << setup.gameId
              << " instances=" << setup.instances.size() << '\n';
    }
    output = human.str();
    return {};
}

CliDiagnostic renderPlan(const PlanSnapshot& snapshot,
                         OutputFormat format,
                         std::string& output) {
    if (!validFormat(format)) return fail(CliResult::InvalidInput, "unknown output format");
    const auto validation = validateSnapshot(snapshot);
    if (!validation.succeeded()) return validation;
    if (format == OutputFormat::Human) {
        std::ostringstream human;
        human << "Plan fingerprint=" << snapshot.fingerprint
              << " seats=" << snapshot.seats.size() << '\n';
        for (const auto& seat : snapshot.seats) {
            human << "- Seat " << seat.seatId << " player=" << seat.playerId
                  << " game=" << seat.gameId << " provider=" << seat.providerId
                  << " provider_revision=" << seat.providerMetadataRevision
                  << " account=" << (seat.accountReferenceSelected ? "<redacted-selected>" : "-")
                  << " protected=" << (seat.protectedOrExperimental ? "yes" : "no") << '\n';
        }
        output = human.str();
        return {};
    }

    try {
        std::string json;
        json.append("{\"schema_version\":1,\"fingerprint\":");
        json.append(std::to_string(snapshot.fingerprint));
        json.append(",\"seats\":[");
        bool firstSeat = true;
        bool validUnicode = true;
        for (const auto& seat : snapshot.seats) {
            if (!firstSeat) json.push_back(',');
            firstSeat = false;
            json.append("{\"seat_id\":");
            json.append(std::to_string(seat.seatId));
            json.append(",\"player_id\":");
            appendJsonString(seat.playerId, json);
            json.append(",\"game_id\":");
            appendJsonString(seat.gameId, json);
            json.append(",\"setup_id\":");
            if (seat.setupId) appendJsonString(*seat.setupId, json); else json.append("null");
            json.append(",\"instance_index\":");
            json.append(std::to_string(seat.instanceIndex));
            json.append(",\"requirement_revision\":");
            json.append(std::to_string(seat.requirementRevision));
            json.append(",\"hardware_fingerprint\":");
            json.append(std::to_string(seat.hardwareFingerprint));
            json.append(",\"provider_id\":");
            appendJsonString(seat.providerId, json);
            json.append(",\"provider_app_id\":");
            if (seat.providerAppId) appendJsonString(*seat.providerAppId, json); else json.append("null");
            json.append(",\"provider_revision\":");
            json.append(std::to_string(seat.providerMetadataRevision));
            json.append(",\"target_kind\":");
            appendJsonString(provider::launchTargetKindName(seat.targetKind), json);
            json.append(",\"target\":");
            appendJsonWide(seat.target, json, validUnicode);
            json.append(",\"arguments\":[");
            bool firstArgument = true;
            for (const auto& argument : seat.arguments) {
                if (!firstArgument) json.push_back(',');
                firstArgument = false;
                appendJsonWide(argument, json, validUnicode);
            }
            json.append("],\"working_directory\":");
            if (seat.workingDirectory) {
                appendJsonWide(*seat.workingDirectory, json, validUnicode);
            } else {
                json.append("null");
            }
            json.append(",\"account_reference_selected\":");
            json.append(seat.accountReferenceSelected ? "true" : "false");
            json.append(",\"protected_or_experimental\":");
            json.append(seat.protectedOrExperimental ? "true" : "false");
            json.push_back('}');
        }
        json.append("]}");
        if (!validUnicode) return fail(CliResult::InvalidInput, "plan contains invalid Unicode");
        output = std::move(json);
        return {};
    } catch (...) {
        return fail(CliResult::InvalidInput, "plan JSON rendering failed");
    }
}

std::string_view cliResultName(CliResult result) noexcept {
    switch (result) {
    case CliResult::Success: return "Success";
    case CliResult::InvalidInput: return "InvalidInput";
    case CliResult::UnsupportedVersion: return "UnsupportedVersion";
    case CliResult::BoundsExceeded: return "BoundsExceeded";
    case CliResult::ParseError: return "ParseError";
    }
    return "Unknown";
}

std::string_view outputFormatName(OutputFormat format) noexcept {
    switch (format) {
    case OutputFormat::Human: return "Human";
    case OutputFormat::Json: return "Json";
    }
    return "Unknown";
}

} // namespace hydra::cli
