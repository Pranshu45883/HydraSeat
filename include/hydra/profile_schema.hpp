#pragma once

#include "hydra/workspace_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hydra::profile {

inline constexpr std::uint32_t kProfileSchemaVersion = 1u;
inline constexpr std::size_t kMaximumSchemaDocumentBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaximumIdentifierBytes = 128u;
inline constexpr std::size_t kMaximumLocaleBytes = 64u;
inline constexpr std::size_t kMaximumDisplayNameCodeUnits = 256u;
inline constexpr std::size_t kMaximumTitleCodeUnits = 512u;
inline constexpr std::size_t kMaximumPathCodeUnits = 32767u;
inline constexpr std::size_t kMaximumDeviceIdCodeUnits = 4096u;
inline constexpr std::size_t kMaximumDeviceIdsPerSeat = 32u;
inline constexpr std::size_t kMaximumProviderAccountsPerPlayer = 16u;
inline constexpr std::size_t kMaximumPlayers = 64u;
inline constexpr std::size_t kMaximumGames = 4096u;
inline constexpr std::size_t kMaximumExecutableCandidates = 32u;
inline constexpr std::size_t kMaximumSetups = 512u;
inline constexpr std::size_t kMaximumArgumentsPerInstance = 128u;
inline constexpr std::size_t kMaximumArgumentCodeUnits = 4096u;
inline constexpr std::size_t kMaximumRuntimeBindings = 2u;
inline constexpr std::size_t kMaximumPersistedSeats = 2u;

// Persisted Seat data is deliberately a hardware-only subset of the legacy
// runtime SeatConfig. HWNDs, PIDs, handles, and other process-lifetime state
// have no representation in this schema.
struct PersistedSeatConfig {
    SeatId seatId{0};
    std::wstring name;
    std::vector<std::wstring> displayIds;
    std::optional<std::wstring> primaryDisplayId;
    std::vector<std::wstring> keyboardIds;
    std::vector<std::wstring> mouseIds;
    std::vector<std::wstring> controllerIds;
    std::optional<std::wstring> audioOutputEndpointId;
    std::optional<std::wstring> audioInputEndpointId;
    bool active{true};

    bool operator==(const PersistedSeatConfig&) const = default;
};

struct SeatConfigDocument {
    std::uint32_t schemaVersion{kProfileSchemaVersion};
    SeatId managementSeatId{1};
    std::vector<PersistedSeatConfig> seats;

    bool operator==(const SeatConfigDocument&) const = default;
};

struct ProviderAccountReference {
    std::string providerId;
    // Opaque local account reference only. Passwords, OAuth tokens, cookies,
    // refresh tokens, and credentials are intentionally not representable.
    std::string accountRef;

    bool operator==(const ProviderAccountReference&) const = default;
};

struct PlayerProfile {
    std::string playerId;
    std::wstring displayName;
    std::string preferredLocale;
    std::vector<ProviderAccountReference> providerAccounts;

    bool operator==(const PlayerProfile&) const = default;
};

struct PlayerProfileDocument {
    std::uint32_t schemaVersion{kProfileSchemaVersion};
    std::vector<PlayerProfile> players;

    bool operator==(const PlayerProfileDocument&) const = default;
};

enum class GameOrigin : std::uint8_t {
    Discovered = 0,
    Manual = 1,
};

struct CompatibilityReference {
    // Stable logical record reference only. Compatibility evidence/results live
    // in their own store and are not duplicated into Game/setup persistence.
    std::string recordId;
    std::string provenance;
    std::uint32_t evidenceRevision{0};

    bool operator==(const CompatibilityReference&) const = default;
};

struct GameRecord {
    std::string gameId;
    std::string providerId;
    std::optional<std::string> providerAppId;
    std::wstring title;
    std::wstring installRoot;
    std::vector<std::wstring> executableCandidates;
    std::optional<std::wstring> localVersion;
    std::optional<std::string> executableSha256;
    std::optional<CompatibilityReference> compatibility;
    GameOrigin origin{GameOrigin::Discovered};

    bool operator==(const GameRecord&) const = default;
};

struct GameRecordDocument {
    std::uint32_t schemaVersion{kProfileSchemaVersion};
    std::vector<GameRecord> games;

    bool operator==(const GameRecordDocument&) const = default;
};

struct InstanceRecipe {
    std::vector<std::wstring> arguments;
    std::optional<std::wstring> workingDirectory;
    std::optional<std::wstring> dataRoot;

    bool operator==(const InstanceRecipe&) const = default;
};

struct TwoPlayerSetup {
    std::string setupId;
    std::string gameId;
    std::wstring displayName;
    std::optional<CompatibilityReference> compatibility;
    // Exactly two typed instance recipes. There is no script/shell field.
    std::vector<InstanceRecipe> instances;

    bool operator==(const TwoPlayerSetup&) const = default;
};

struct TwoPlayerSetupDocument {
    std::uint32_t schemaVersion{kProfileSchemaVersion};
    std::vector<TwoPlayerSetup> setups;

    bool operator==(const TwoPlayerSetupDocument&) const = default;
};

struct RuntimeBinding {
    SeatId seatId{0};
    std::string playerId;
    std::string gameId;
    std::optional<std::string> setupId;
    std::uint32_t instanceIndex{0};

    bool operator==(const RuntimeBinding&) const = default;
};

// This schema may be serialized for IPC/test/export, but is temporary runtime
// selection state and is not a stable process identity. It contains no PID,
// HWND, process handle, window handle, or raw OS object reference.
struct RuntimeSessionSelection {
    std::uint32_t schemaVersion{kProfileSchemaVersion};
    std::vector<RuntimeBinding> bindings;

    bool operator==(const RuntimeSessionSelection&) const = default;
};

enum class SchemaResult : std::uint8_t {
    Success = 0,
    ParseError = 1,
    DocumentTooLarge = 2,
    UnsupportedVersion = 3,
    UnknownField = 4,
    MissingField = 5,
    WrongType = 6,
    InvalidValue = 7,
    BoundsExceeded = 8,
    DuplicateId = 9,
    RuntimeOnlyStatePresent = 10,
    CrossReferenceError = 11,
};

struct SchemaDiagnostic {
    SchemaResult result{SchemaResult::Success};
    std::string message;

    bool succeeded() const noexcept { return result == SchemaResult::Success; }
};

SchemaDiagnostic validateSeatConfigDocument(const SeatConfigDocument& document);
SchemaDiagnostic validatePlayerProfileDocument(const PlayerProfileDocument& document);
SchemaDiagnostic validateGameRecordDocument(const GameRecordDocument& document);
SchemaDiagnostic validateTwoPlayerSetupDocument(const TwoPlayerSetupDocument& document);
SchemaDiagnostic validateRuntimeSessionSelection(
    const RuntimeSessionSelection& selection,
    const SeatConfigDocument& seats,
    const PlayerProfileDocument& players,
    const GameRecordDocument& games,
    const TwoPlayerSetupDocument& setups);

// Conversion is intentionally fail-closed if the legacy runtime Seat carries a
// transient HWND. P6-MIG-01 owns migration/cleanup of legacy persisted state.
SchemaDiagnostic makePersistedSeatConfig(const SeatConfig& runtime,
                                         PersistedSeatConfig& persisted);
SeatConfig makeRuntimeSeatConfig(const PersistedSeatConfig& persisted);

std::string encodeSeatConfigDocument(const SeatConfigDocument& document,
                                     SchemaDiagnostic* diagnostic = nullptr);
std::string encodePlayerProfileDocument(const PlayerProfileDocument& document,
                                        SchemaDiagnostic* diagnostic = nullptr);
std::string encodeGameRecordDocument(const GameRecordDocument& document,
                                     SchemaDiagnostic* diagnostic = nullptr);
std::string encodeTwoPlayerSetupDocument(const TwoPlayerSetupDocument& document,
                                         SchemaDiagnostic* diagnostic = nullptr);
std::string encodeRuntimeSessionSelection(const RuntimeSessionSelection& selection,
                                          SchemaDiagnostic* diagnostic = nullptr);

SchemaDiagnostic decodeSeatConfigDocument(std::string_view json,
                                          SeatConfigDocument& document);
SchemaDiagnostic decodePlayerProfileDocument(std::string_view json,
                                             PlayerProfileDocument& document);
SchemaDiagnostic decodeGameRecordDocument(std::string_view json,
                                          GameRecordDocument& document);
SchemaDiagnostic decodeTwoPlayerSetupDocument(std::string_view json,
                                              TwoPlayerSetupDocument& document);
SchemaDiagnostic decodeRuntimeSessionSelection(std::string_view json,
                                               RuntimeSessionSelection& selection);

std::string_view schemaResultName(SchemaResult result) noexcept;
std::string_view gameOriginName(GameOrigin origin) noexcept;

} // namespace hydra::profile
