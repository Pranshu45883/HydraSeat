#pragma once

#include "hydra/profile_schema.hpp"
#include "hydra/seat_hardware_configuration.hpp"

#include <filesystem>
#include <string>

namespace hydra::runtime {

class SeatHardwareStore final {
public:
    explicit SeatHardwareStore(std::filesystem::path path);

    const std::filesystem::path& path() const noexcept { return path_; }

    // Missing files are a valid first-run state and produce two empty Seats.
    // Malformed or incompatible files fail closed and leave output unchanged.
    bool load(SeatHardwareConfigurations& output, std::string* error = nullptr);

    // Writes the canonical SeatConfigDocument transactionally. The in-memory
    // document is advanced only after the replacement succeeds.
    bool save(
        const SeatHardwareConfigurations& configurations,
        std::string* error = nullptr);

private:
    std::filesystem::path path_;
    profile::SeatConfigDocument document_;

    static profile::SeatConfigDocument defaultDocument();
    static bool toConfigurations(
        const profile::SeatConfigDocument& document,
        SeatHardwareConfigurations& output,
        std::string* error);
    static bool applyConfigurations(
        const SeatHardwareConfigurations& configurations,
        profile::SeatConfigDocument& document,
        std::string* error);
};

} // namespace hydra::runtime
