#include "hydra/acceptance_probe.hpp"

#include "hydra/audio_endpoint_inventory.hpp"
#include "hydra/controller_runtime.hpp"
#include "hydra/display_topology.hpp"
#include "hydra/internal/strict_json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <softpub.h>
#include <tlhelp32.h>
#include <wintrust.h>
#endif

namespace hydra::acceptance {
namespace {

using JsonValue = internal::json::Value;
using JsonObject = internal::json::Value::Object;
using JsonArray = internal::json::Value::Array;

ProbeDiagnostic fail(ProbeCode code, std::string message) {
    return {code, std::move(message)};
}

bool token(std::string_view value, std::size_t maximum = 128u) {
    if (value.empty() || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](char raw) {
        const auto ch = static_cast<unsigned char>(raw);
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' ||
               ch == '+';
    });
}

bool hex(std::string_view value, std::size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

const JsonObject* object(const JsonValue& value) {
    return std::get_if<JsonObject>(&value.value);
}

const JsonArray* array(const JsonValue& value) {
    return std::get_if<JsonArray>(&value.value);
}

bool exactKeys(const JsonObject& value, std::initializer_list<std::string_view> keys) {
    if (value.size() != keys.size()) return false;
    return std::all_of(keys.begin(), keys.end(), [&](std::string_view key) {
        return value.contains(std::string(key));
    });
}

const std::string* stringValue(const JsonObject& value, std::string_view key) {
    const auto found = value.find(std::string(key));
    return found == value.end() ? nullptr : std::get_if<std::string>(&found->second.value);
}

std::optional<std::uint64_t> uintValue(const JsonObject& value, std::string_view key) {
    const auto found = value.find(std::string(key));
    if (found == value.end()) return std::nullopt;
    const auto* number = std::get_if<internal::json::Number>(&found->second.value);
    if (!number || number->text.empty() || number->text.front() == '-') return std::nullopt;
    std::uint64_t result = 0u;
    const auto parsed = std::from_chars(number->text.data(),
                                        number->text.data() + number->text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == number->text.data() + number->text.size()
               ? std::optional(result) : std::nullopt;
}

void appendEscaped(std::string& output, std::string_view value) {
    output.push_back('"');
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '"') output += "\\\"";
        else if (ch == '\\') output += "\\\\";
        else if (ch == '\n') output += "\\n";
        else if (ch == '\r') output += "\\r";
        else if (ch == '\t') output += "\\t";
        else if (ch >= 0x20u) output.push_back(static_cast<char>(ch));
    }
    output.push_back('"');
}

#if defined(_WIN32)

struct Handle {
    HANDLE value{nullptr};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE input) : value(input) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::uint64_t fileTime(const FILETIME& value) {
    ULARGE_INTEGER integer{};
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (required <= 0) return {};
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), output.data(), required,
                            nullptr, nullptr) != required) return {};
    return output;
}

std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), output.data(), required) != required) {
        return {};
    }
    return output;
}

std::wstring lowerPath(const std::filesystem::path& value) {
    std::wstring output = value.lexically_normal().wstring();
    std::transform(output.begin(), output.end(), output.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(ch)));
    });
    return output;
}

std::string sha256Bytes(const void* bytes, std::size_t size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0u;
    DWORD returned = 0u;
    DWORD digestLength = 0u;
    std::vector<UCHAR> objectBytes;
    std::vector<UCHAR> digest;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0u) < 0) return {};
    const auto closeAlgorithm = [&] { BCryptCloseAlgorithmProvider(algorithm, 0u); };
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                          &returned, 0u) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength),
                          &returned, 0u) < 0) {
        closeAlgorithm();
        return {};
    }
    objectBytes.resize(objectLength);
    digest.resize(digestLength);
    if (BCryptCreateHash(algorithm, &hash, objectBytes.data(), objectLength,
                         nullptr, 0u, 0u) < 0) {
        closeAlgorithm();
        return {};
    }
    const auto close = [&] { BCryptDestroyHash(hash); closeAlgorithm(); };
    const auto* cursor = static_cast<const UCHAR*>(bytes);
    std::size_t remaining = size;
    while (remaining != 0u) {
        const auto chunk = static_cast<ULONG>(
            std::min<std::size_t>(remaining, std::numeric_limits<ULONG>::max()));
        if (BCryptHashData(hash, const_cast<PUCHAR>(cursor), chunk, 0u) < 0) {
            close();
            return {};
        }
        cursor += chunk;
        remaining -= chunk;
    }
    if (BCryptFinishHash(hash, digest.data(), digestLength, 0u) < 0) {
        close();
        return {};
    }
    close();
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : digest) stream << std::setw(2) << static_cast<unsigned>(byte);
    return stream.str();
}

bool authenticodeValid(const std::filesystem::path& path) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    const LONG status = WinVerifyTrust(nullptr, &policy, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust(nullptr, &policy, &data);
    return status == ERROR_SUCCESS;
}

bool safeRoot(const std::filesystem::path& root) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(root, error);
    if (error || canonical.empty() || canonical == canonical.root_path()) return false;
    const DWORD attributes = GetFileAttributesW(canonical.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u;
}

struct LiveSample {
    ProbeProcessRecord record;
    std::uint64_t firstCpu{0u};
    std::uint64_t lastCpu{0u};
    std::vector<std::uint64_t> workingSets;
};

std::uint64_t percentile(std::vector<std::uint64_t> values, std::size_t numerator) {
    if (values.empty()) return 0u;
    std::sort(values.begin(), values.end());
    const auto rank = ((values.size() * numerator) + 99u) / 100u;
    return values[std::min(values.size() - 1u, rank - 1u)];
}

void sampleProcesses(const std::map<std::wstring, InstalledFileClaim>& allowed,
                     std::map<std::pair<std::uint32_t, std::uint64_t>, LiveSample>& samples) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0u));
    if (snapshot.value == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) return;
    do {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                   FALSE, entry.th32ProcessID));
        if (!process.value) continue;
        std::wstring path(32768u, L'\0');
        DWORD pathSize = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.value, 0u, path.data(), &pathSize)) continue;
        path.resize(pathSize);
        const auto allowedIt = allowed.find(lowerPath(path));
        if (allowedIt == allowed.end()) continue;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process.value, &created, &exited, &kernel, &user)) continue;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        if (!GetProcessMemoryInfo(process.value,
                                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                  sizeof(memory))) continue;
        DWORD handles = 0u;
        if (!GetProcessHandleCount(process.value, &handles)) continue;
        const auto identity = std::make_pair(entry.th32ProcessID, fileTime(created));
        auto [it, inserted] = samples.try_emplace(identity);
        auto& sample = it->second;
        const auto cpu = fileTime(kernel) + fileTime(user);
        if (inserted) {
            sample.record.processId = entry.th32ProcessID;
            sample.record.creationTime100ns = identity.second;
            sample.record.fileName = allowedIt->second.fileName;
            sample.record.executableSha256 = allowedIt->second.sha256;
            sample.record.firstWorkingSetBytes = memory.WorkingSetSize;
            sample.firstCpu = cpu;
        }
        sample.record.lastWorkingSetBytes = memory.WorkingSetSize;
        sample.record.maximumWorkingSetBytes = std::max<std::uint64_t>(
            sample.record.maximumWorkingSetBytes, memory.WorkingSetSize);
        sample.record.maximumHandleCount = std::max(sample.record.maximumHandleCount,
                                                     static_cast<std::uint32_t>(handles));
        sample.record.maximumThreadCount = std::max(
            sample.record.maximumThreadCount,
            static_cast<std::uint32_t>(entry.cntThreads));
        sample.lastCpu = cpu;
        sample.record.cpuTimeDelta100ns = sample.lastCpu - sample.firstCpu;
        sample.workingSets.push_back(memory.WorkingSetSize);
        ++sample.record.samples;
    } while (Process32NextW(snapshot.value, &entry));
}

ProbeInventory observeInventory() {
    ProbeInventory result;
    std::string displayCanonical;
    hydra::display::DisplayTopologyInventory displayInventory;
    const auto displays = displayInventory.refresh();
    result.displayQuerySucceeded = displays.querySucceeded;
    if (displays.querySucceeded) {
        std::vector<std::string> rows;
        for (const auto& output : displays.outputs) {
            if (output.active && output.attached) ++result.activeDisplayCount;
            std::ostringstream row;
            row << output.identity.adapterLuid.highPart << ':'
                << output.identity.adapterLuid.lowPart << ':'
                << output.identity.targetId << ':' << output.active << ':' << output.attached
                << ':' << output.desktopBounds.left << ':' << output.desktopBounds.top << ':'
                << output.desktopBounds.right << ':' << output.desktopBounds.bottom << ':'
                << output.mode.width << ':' << output.mode.height << ':' << output.dpiX << ':'
                << output.dpiY;
            rows.push_back(row.str());
        }
        std::sort(rows.begin(), rows.end());
        for (const auto& row : rows) displayCanonical += row + '\n';
        result.displayFingerprintSha256 = sha256Bytes(displayCanonical.data(), displayCanonical.size());
    }

    hydra::audio::EndpointInventory audioInventory(hydra::audio::makeNativeEndpointSource());
    std::string audioError;
    result.audioQuerySucceeded = audioInventory.refresh(&audioError) && audioInventory.current().has_value();
    if (result.audioQuerySucceeded) {
        std::vector<std::string> rows;
        for (const auto& endpoint : audioInventory.current()->endpoints) {
            if (endpoint.flow == hydra::audio::DataFlow::Render &&
                hydra::audio::isEndpointCurrentlyAvailable(endpoint)) {
                ++result.activeRenderEndpointCount;
            }
            rows.push_back(utf8(endpoint.endpointId) + ':' +
                           std::to_string(static_cast<unsigned>(endpoint.flow)) + ':' +
                           std::to_string(endpoint.stateMask) + ':' +
                           std::to_string(endpoint.defaultRoleMask));
        }
        std::sort(rows.begin(), rows.end());
        std::string canonical;
        for (const auto& row : rows) canonical += row + '\n';
        result.audioFingerprintSha256 = sha256Bytes(canonical.data(), canonical.size());
    }

    auto controllerBackend = hydra::controller::makeNativeControllerSourceBackend();
    std::vector<hydra::controller::SourceDescriptor> controllers;
    std::string controllerError;
    result.controllerQuerySucceeded = controllerBackend &&
        controllerBackend->scan(controllers, controllerError);
    if (result.controllerQuerySucceeded) {
        std::vector<std::string> rows;
        for (const auto& controller : controllers) {
            if (controller.connected) ++result.connectedControllerCount;
            rows.push_back(controller.runtimeKey + ':' +
                           std::to_string(static_cast<unsigned>(controller.api)) + ':' +
                           std::to_string(controller.connected) + ':' +
                           std::to_string(controller.sourceGeneration));
        }
        std::sort(rows.begin(), rows.end());
        std::string canonical;
        for (const auto& row : rows) canonical += row + '\n';
        result.controllerFingerprintSha256 = sha256Bytes(canonical.data(), canonical.size());
    }
    return result;
}

#endif

} // namespace

std::string sha256FileHex(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0u;
    DWORD digestLength = 0u;
    DWORD returned = 0u;
    std::vector<UCHAR> objectBytes;
    std::vector<UCHAR> digest;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0u) < 0) return {};
    const auto closeAlgorithm = [&] { BCryptCloseAlgorithmProvider(algorithm, 0u); };
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                          &returned, 0u) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&digestLength), sizeof(digestLength),
                          &returned, 0u) < 0) {
        closeAlgorithm();
        return {};
    }
    objectBytes.resize(objectLength);
    digest.resize(digestLength);
    if (BCryptCreateHash(algorithm, &hash, objectBytes.data(), objectLength,
                         nullptr, 0u, 0u) < 0) {
        closeAlgorithm();
        return {};
    }
    const auto close = [&] { BCryptDestroyHash(hash); closeAlgorithm(); };
    std::array<char, 65536u> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                        static_cast<ULONG>(count), 0u) < 0) {
            close();
            return {};
        }
    }
    if (!input.eof() || BCryptFinishHash(hash, digest.data(), digestLength, 0u) < 0) {
        close();
        return {};
    }
    close();
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : digest) stream << std::setw(2) << static_cast<unsigned>(byte);
    return stream.str();
#else
    (void)path;
    return {};
#endif
}

ProbeDiagnostic loadInstalledReleaseClaim(const std::filesystem::path& path,
                                          InstalledReleaseClaim& output,
                                          std::string* exactStateSha256) {
#if defined(_WIN32)
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0u || size > kMaximumInstalledStateBytes) {
        return fail(ProbeCode::StateReadFailed, "install state is missing, empty, or oversized");
    }
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input) return fail(ProbeCode::StateReadFailed, "install state read failed");
    try {
        const auto parsed = internal::json::parse(bytes, {8u, 512u});
        const auto* root = object(parsed);
        if (!root || !exactKeys(*root, {"schemaVersion", "releaseVersion", "releaseRevision",
            "commitSha", "architecture", "installRoot", "startupMode", "ownedFiles"})) {
            throw std::runtime_error("unknown or missing install state fields");
        }
        const auto schema = uintValue(*root, "schemaVersion");
        const auto revision = uintValue(*root, "releaseRevision");
        const auto* version = stringValue(*root, "releaseVersion");
        const auto* commit = stringValue(*root, "commitSha");
        const auto* architecture = stringValue(*root, "architecture");
        const auto* installRoot = stringValue(*root, "installRoot");
        const auto* startup = stringValue(*root, "startupMode");
        const auto filesIt = root->find("ownedFiles");
        const auto* files = filesIt == root->end() ? nullptr : array(filesIt->second);
        if (!schema || *schema != 1u || !revision || *revision == 0u || !version ||
            !commit || !architecture || !installRoot || !startup || !files || files->empty() ||
            files->size() > kMaximumProbeOwnedFiles) throw std::runtime_error("invalid install state values");
        InstalledReleaseClaim candidate;
        candidate.releaseVersion = *version;
        candidate.releaseRevision = *revision;
        candidate.commitSha = *commit;
        candidate.architecture = *architecture;
        candidate.installRoot = std::filesystem::path(wide(*installRoot));
        candidate.startupMode = *startup;
        std::set<std::string> names;
        for (const auto& fileValue : *files) {
            const auto* file = object(fileValue);
            if (!file || !exactKeys(*file, {"fileName", "sha256"})) throw std::runtime_error("invalid owned file fields");
            const auto* name = stringValue(*file, "fileName");
            const auto* sha = stringValue(*file, "sha256");
            if (!name || !sha || !token(*name, 128u) || !hex(*sha, 64u) ||
                !names.insert(*name).second) throw std::runtime_error("invalid duplicate owned file");
            candidate.ownedFiles.push_back({*name, *sha});
        }
        if (!token(candidate.releaseVersion, 64u) || !hex(candidate.commitSha, 40u) ||
            candidate.architecture != "x64" || !token(candidate.startupMode, 64u) ||
            !safeRoot(candidate.installRoot)) throw std::runtime_error("unsafe install identity/root");
        const auto stateSha = sha256Bytes(bytes.data(), bytes.size());
        if (stateSha.empty()) throw std::runtime_error("install state hash failed");
        output = std::move(candidate);
        if (exactStateSha256) *exactStateSha256 = stateSha;
        return {};
    } catch (const std::exception& exception) {
        return fail(ProbeCode::StateDecodeFailed,
                    std::string("strict install state decode failed: ") + exception.what());
    }
#else
    (void)path; (void)output; (void)exactStateSha256;
    return fail(ProbeCode::UnsupportedPlatform, "acceptance probe is Windows-only");
#endif
}

ProbeDiagnostic runAcceptanceProbe(const InstalledReleaseClaim& claim,
                                   std::string_view installStateSha256,
                                   bool allowUnsignedDevelopment,
                                   std::size_t sampleCount,
                                   std::uint32_t sampleIntervalMilliseconds,
                                   AcceptanceProbeReport& output) {
#if defined(_WIN32)
    if (!hex(claim.commitSha, 40u) || !hex(installStateSha256, 64u) ||
        claim.ownedFiles.empty() || claim.ownedFiles.size() > kMaximumProbeOwnedFiles ||
        sampleCount == 0u || sampleCount > kMaximumProbeSamples ||
        sampleIntervalMilliseconds < 10u || sampleIntervalMilliseconds > 60000u ||
        !safeRoot(claim.installRoot)) {
        return fail(ProbeCode::InvalidArgument, "probe arguments or installed claim are invalid");
    }
    std::map<std::wstring, InstalledFileClaim> allowed;
    for (const auto& file : claim.ownedFiles) {
        const auto path = claim.installRoot / std::filesystem::path(file.fileName);
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            return fail(ProbeCode::OwnedFileMissing, "an exact installed owned file is missing");
        }
        const auto observedHash = sha256FileHex(path);
        if (observedHash != file.sha256) {
            return fail(ProbeCode::OwnedFileHashMismatch, "installed owned file hash mismatch");
        }
        if (!allowUnsignedDevelopment && !authenticodeValid(path)) {
            return fail(ProbeCode::OwnedFileSignatureInvalid,
                        "installed owned file Authenticode verification failed");
        }
        if (path.extension() == L".exe") allowed.emplace(lowerPath(path), file);
    }

    std::map<std::pair<std::uint32_t, std::uint64_t>, LiveSample> samples;
    for (std::size_t index = 0u; index < sampleCount; ++index) {
        sampleProcesses(allowed, samples);
        if (index + 1u != sampleCount) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sampleIntervalMilliseconds));
        }
    }
    AcceptanceProbeReport candidate;
    candidate.releaseVersion = claim.releaseVersion;
    candidate.releaseRevision = claim.releaseRevision;
    candidate.commitSha = claim.commitSha;
    candidate.architecture = claim.architecture;
    candidate.installStateSha256 = std::string(installStateSha256);
    candidate.developmentUnsignedAllowed = allowUnsignedDevelopment;
    candidate.allOwnedFilesVerified = true;
    for (auto& [identity, sample] : samples) {
        (void)identity;
        sample.record.workingSetP50Bytes = percentile(sample.workingSets, 50u);
        sample.record.workingSetP95Bytes = percentile(sample.workingSets, 95u);
        sample.record.workingSetP99Bytes = percentile(sample.workingSets, 99u);
        candidate.runningOwnedProcesses.push_back(sample.record);
    }
    candidate.inventory = observeInventory();
    if (!candidate.inventory.displayQuerySucceeded ||
        !candidate.inventory.audioQuerySucceeded ||
        !candidate.inventory.controllerQuerySucceeded) {
        return fail(ProbeCode::InventoryObservationFailed,
                    "one or more read-only hardware inventories failed");
    }
    output = std::move(candidate);
    return {};
#else
    (void)claim; (void)installStateSha256; (void)allowUnsignedDevelopment;
    (void)sampleCount; (void)sampleIntervalMilliseconds; (void)output;
    return fail(ProbeCode::UnsupportedPlatform, "acceptance probe is Windows-only");
#endif
}

std::string encodeAcceptanceProbeJson(const AcceptanceProbeReport& report) {
    std::string output = "{\"schema_version\":" + std::to_string(report.schemaVersion) +
        ",\"release_version\":";
    appendEscaped(output, report.releaseVersion);
    output += ",\"release_revision\":" + std::to_string(report.releaseRevision) +
        ",\"commit_sha\":";
    appendEscaped(output, report.commitSha);
    output += ",\"architecture\":";
    appendEscaped(output, report.architecture);
    output += ",\"install_state_sha256\":";
    appendEscaped(output, report.installStateSha256);
    output += ",\"development_unsigned_allowed\":";
    output += report.developmentUnsignedAllowed ? "true" : "false";
    output += ",\"all_owned_files_verified\":";
    output += report.allOwnedFilesVerified ? "true" : "false";
    output += ",\"running_owned_processes\":[";
    for (std::size_t index = 0u; index < report.runningOwnedProcesses.size(); ++index) {
        const auto& process = report.runningOwnedProcesses[index];
        if (index != 0u) output.push_back(',');
        output += "{\"process_id\":" + std::to_string(process.processId) +
            ",\"creation_time_100ns\":" + std::to_string(process.creationTime100ns) +
            ",\"file_name\":";
        appendEscaped(output, process.fileName);
        output += ",\"executable_sha256\":"; appendEscaped(output, process.executableSha256);
        output += ",\"first_working_set_bytes\":" + std::to_string(process.firstWorkingSetBytes) +
            ",\"working_set_p50_bytes\":" + std::to_string(process.workingSetP50Bytes) +
            ",\"working_set_p95_bytes\":" + std::to_string(process.workingSetP95Bytes) +
            ",\"working_set_p99_bytes\":" + std::to_string(process.workingSetP99Bytes) +
            ",\"maximum_working_set_bytes\":" + std::to_string(process.maximumWorkingSetBytes) +
            ",\"last_working_set_bytes\":" + std::to_string(process.lastWorkingSetBytes) +
            ",\"maximum_handle_count\":" + std::to_string(process.maximumHandleCount) +
            ",\"maximum_thread_count\":" + std::to_string(process.maximumThreadCount) +
            ",\"cpu_time_delta_100ns\":" + std::to_string(process.cpuTimeDelta100ns) +
            ",\"samples\":" + std::to_string(process.samples) + '}';
    }
    output += "],\"inventory\":{\"display_query_succeeded\":";
    output += report.inventory.displayQuerySucceeded ? "true" : "false";
    output += ",\"audio_query_succeeded\":";
    output += report.inventory.audioQuerySucceeded ? "true" : "false";
    output += ",\"controller_query_succeeded\":";
    output += report.inventory.controllerQuerySucceeded ? "true" : "false";
    output += ",\"display_fingerprint_sha256\":"; appendEscaped(output, report.inventory.displayFingerprintSha256);
    output += ",\"audio_fingerprint_sha256\":"; appendEscaped(output, report.inventory.audioFingerprintSha256);
    output += ",\"controller_fingerprint_sha256\":"; appendEscaped(output, report.inventory.controllerFingerprintSha256);
    output += ",\"active_display_count\":" + std::to_string(report.inventory.activeDisplayCount) +
        ",\"active_render_endpoint_count\":" + std::to_string(report.inventory.activeRenderEndpointCount) +
        ",\"connected_controller_count\":" + std::to_string(report.inventory.connectedControllerCount) + "}}";
    return output;
}

std::string_view probeCodeName(ProbeCode value) noexcept {
    switch (value) {
    case ProbeCode::Success: return "Success";
    case ProbeCode::UnsupportedPlatform: return "UnsupportedPlatform";
    case ProbeCode::InvalidArgument: return "InvalidArgument";
    case ProbeCode::StateReadFailed: return "StateReadFailed";
    case ProbeCode::StateDecodeFailed: return "StateDecodeFailed";
    case ProbeCode::UnsafeInstallRoot: return "UnsafeInstallRoot";
    case ProbeCode::OwnedFileMissing: return "OwnedFileMissing";
    case ProbeCode::OwnedFileHashMismatch: return "OwnedFileHashMismatch";
    case ProbeCode::OwnedFileSignatureInvalid: return "OwnedFileSignatureInvalid";
    case ProbeCode::ProcessObservationFailed: return "ProcessObservationFailed";
    case ProbeCode::InventoryObservationFailed: return "InventoryObservationFailed";
    }
    return "Unknown";
}

} // namespace hydra::acceptance
