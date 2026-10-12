#pragma once

namespace hydra::ui {

struct UiSettingsSnapshot {
    bool startMinimized{false};
    bool confirmSeatStop{true};
    int refreshIntervalMs{2000};

    bool operator==(const UiSettingsSnapshot&) const = default;
};

class UiSettings final {
public:
    static constexpr int kMinimumRefreshIntervalMs = 500;
    static constexpr int kMaximumRefreshIntervalMs = 10000;
    static constexpr int kDefaultRefreshIntervalMs = 2000;

    static UiSettingsSnapshot load();
    static void save(const UiSettingsSnapshot& settings);
};

} // namespace hydra::ui
