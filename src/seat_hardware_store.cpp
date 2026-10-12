#include "hydra/seat_hardware_store.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace hydra::runtime {
namespace {

void setError(std::string* output, std::string message) {
    if (output) *output = std::move(message);
}

bool boundedSingle(
    const std::vector<std::wstring>& values,
    std::wstring& output,
    const char* label,
    std::string* error) {
    if (values.size() > 1u) {
        setError(
            error,
            std::string("HydraSeat v1 supports one ") + label +
                " assignment per Seat");
        return false;
    }
    output = values.empty() ? std::wstring{} : values.front();
    return true;
}

bool validateExclusive(
    const SeatHardwareConfigurations& configurations,
    std::string* error) {
    if (configurations[0].seatId != 1u ||
        configurations[1].seatId != 2u) {
        setError(error, "Seat hardware store requires exactly Seat 1 and Seat 2");
        return false;
    }

    const auto conflicts = [](const std::wstring& a, const std::wstring& b) {
        return !a.empty() && a == b;
    };
    if (conflicts(
            configurations[0].displayId,
            configurations[1].displayId) ||
        conflicts(
            configurations[0].keyboardId,
            configurations[1].keyboardId) ||
        conflicts(
            configurations[0].mouseId,
            configurations[1].mouseId) ||
        conflicts(
            configurations[0].controllerId,
            configurations[1].controllerId)) {
        setError(error, "one physical device cannot belong to both Seats");
        return false;
    }
    return true;
}

bool writeAtomically(
    const std::filesystem::path& path,
    std::string_view bytes,
    std::string* error) {
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            setError(error, "failed to create HydraSeat configuration directory");
            return false;
        }
    }

    auto temporary = path;
    temporary += L".tmp";

    {
        std::ofstream stream(
            temporary,
            std::ios::binary | std::ios::trunc);
        if (!stream) {
            setError(error, "failed to open temporary Seat configuration");
            return false;
        }
        stream.write(
            bytes.data(),
            static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream.good()) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            setError(error, "failed to write temporary Seat configuration");
            return false;
        }
    }

#if defined(_WIN32)
    if (!MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError();
        std::filesystem::remove(temporary, ec);
        setError(
            error,
            "failed to atomically replace Seat configuration (win32=" +
                std::to_string(code) + ")");
        return false;
    }
#else
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        setError(error, "failed to atomically replace Seat configuration");
        return false;
    }
#endif
    return true;
}

} // namespace

SeatHardwareStore::SeatHardwareStore(std::filesystem::path path)
    : path_(std::move(path)),
      document_(defaultDocument()) {}

profile::SeatConfigDocument SeatHardwareStore::defaultDocument() {
    profile::SeatConfigDocument document;
    document.managementSeatId = 1;
    document.seats = {
        profile::PersistedSeatConfig{
            1, L"Seat 1", {}, std::nullopt, {}, {}, {},
            std::nullopt, std::nullopt, true},
        profile::PersistedSeatConfig{
            2, L"Seat 2", {}, std::nullopt, {}, {}, {},
            std::nullopt, std::nullopt, true},
    };
    return document;
}

bool SeatHardwareStore::toConfigurations(
    const profile::SeatConfigDocument& document,
    SeatHardwareConfigurations& output,
    std::string* error) {
    SeatHardwareConfigurations candidate{{
        SeatHardwareConfiguration{1},
        SeatHardwareConfiguration{2},
    }};

    if (document.seats.size() != 2u) {
        setError(error, "Seat configuration must contain exactly two Seats");
        return false;
    }

    std::array<bool, 2> seen{};
    for (const auto& seat : document.seats) {
        if (seat.seatId == 0 || seat.seatId > 2u ||
            seen[seat.seatId - 1u]) {
            setError(error, "Seat configuration contains invalid or duplicate Seat ids");
            return false;
        }
        seen[seat.seatId - 1u] = true;

        auto& target = candidate[seat.seatId - 1u];
        if (!boundedSingle(
                seat.displayIds,
                target.displayId,
                "display",
                error) ||
            !boundedSingle(
                seat.keyboardIds,
                target.keyboardId,
                "keyboard",
                error) ||
            !boundedSingle(
                seat.mouseIds,
                target.mouseId,
                "mouse",
                error) ||
            !boundedSingle(
                seat.controllerIds,
                target.controllerId,
                "controller",
                error)) {
            return false;
        }

        if (seat.primaryDisplayId &&
            *seat.primaryDisplayId != target.displayId) {
            setError(
                error,
                "primary display must match the single v1 display assignment");
            return false;
        }
    }

    if (!seen[0] || !seen[1] ||
        !validateExclusive(candidate, error)) {
        return false;
    }

    output = std::move(candidate);
    return true;
}

bool SeatHardwareStore::applyConfigurations(
    const SeatHardwareConfigurations& configurations,
    profile::SeatConfigDocument& document,
    std::string* error) {
    if (!validateExclusive(configurations, error)) return false;

    if (document.seats.empty()) {
        document = defaultDocument();
    }

    for (const auto& configuration : configurations) {
        auto found = document.seats.end();
        for (auto it = document.seats.begin();
             it != document.seats.end();
             ++it) {
            if (it->seatId == configuration.seatId) {
                found = it;
                break;
            }
        }
        if (found == document.seats.end()) {
            setError(error, "Seat configuration document is missing a Seat");
            return false;
        }

        found->displayIds.clear();
        found->keyboardIds.clear();
        found->mouseIds.clear();
        found->controllerIds.clear();
        found->primaryDisplayId.reset();

        if (!configuration.displayId.empty()) {
            found->displayIds.push_back(configuration.displayId);
            found->primaryDisplayId = configuration.displayId;
        }
        if (!configuration.keyboardId.empty()) {
            found->keyboardIds.push_back(configuration.keyboardId);
        }
        if (!configuration.mouseId.empty()) {
            found->mouseIds.push_back(configuration.mouseId);
        }
        if (!configuration.controllerId.empty()) {
            found->controllerIds.push_back(configuration.controllerId);
        }
    }

    const auto diagnostic =
        profile::validateSeatConfigDocument(document);
    if (!diagnostic.succeeded()) {
        setError(
            error,
            "Seat configuration validation failed: " +
                diagnostic.message);
        return false;
    }
    return true;
}

bool SeatHardwareStore::load(
    SeatHardwareConfigurations& output,
    std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        if (ec) {
            setError(error, "failed to inspect Seat configuration path");
            return false;
        }
        auto empty = SeatHardwareConfigurations{{
            SeatHardwareConfiguration{1},
            SeatHardwareConfiguration{2},
        }};
        document_ = defaultDocument();
        output = std::move(empty);
        return true;
    }

    const auto size = std::filesystem::file_size(path_, ec);
    if (ec || size > profile::kMaximumSchemaDocumentBytes) {
        setError(error, "Seat configuration file is unreadable or too large");
        return false;
    }

    std::ifstream stream(path_, std::ios::binary);
    if (!stream) {
        setError(error, "failed to open Seat configuration file");
        return false;
    }
    std::string json{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    if (!stream.good() && !stream.eof()) {
        setError(error, "failed while reading Seat configuration file");
        return false;
    }

    profile::SeatConfigDocument candidateDocument;
    const auto diagnostic =
        profile::decodeSeatConfigDocument(json, candidateDocument);
    if (!diagnostic.succeeded()) {
        setError(
            error,
            "Seat configuration parse failed: " +
                diagnostic.message);
        return false;
    }

    SeatHardwareConfigurations candidate;
    if (!toConfigurations(candidateDocument, candidate, error)) {
        return false;
    }

    document_ = std::move(candidateDocument);
    output = std::move(candidate);
    return true;
}

bool SeatHardwareStore::save(
    const SeatHardwareConfigurations& configurations,
    std::string* error) {
    auto candidateDocument = document_;
    if (!applyConfigurations(
            configurations,
            candidateDocument,
            error)) {
        return false;
    }

    profile::SchemaDiagnostic diagnostic;
    const auto json =
        profile::encodeSeatConfigDocument(
            candidateDocument,
            &diagnostic);
    if (!diagnostic.succeeded() || json.empty()) {
        setError(
            error,
            "Seat configuration encoding failed: " +
                diagnostic.message);
        return false;
    }

    if (!writeAtomically(path_, json, error)) {
        return false;
    }

    document_ = std::move(candidateDocument);
    return true;
}

} // namespace hydra::runtime
