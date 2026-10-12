#include "ui/ui_settings.hpp"

#include <QSettings>

#include <algorithm>

namespace hydra::ui {
namespace {

QSettings settingsStore() {
    return QSettings(
        QSettings::NativeFormat,
        QSettings::UserScope,
        QStringLiteral("HydraSeat"),
        QStringLiteral("HydraSeat"));
}

int boundedRefreshInterval(int value) {
    return std::clamp(
        value,
        UiSettings::kMinimumRefreshIntervalMs,
        UiSettings::kMaximumRefreshIntervalMs);
}

} // namespace

UiSettingsSnapshot UiSettings::load() {
    auto settings = settingsStore();

    UiSettingsSnapshot snapshot;
    snapshot.startMinimized =
        settings.value(QStringLiteral("ui/startMinimized"), false).toBool();
    snapshot.confirmSeatStop =
        settings.value(QStringLiteral("ui/confirmSeatStop"), true).toBool();
    snapshot.refreshIntervalMs = boundedRefreshInterval(
        settings.value(
            QStringLiteral("ui/refreshIntervalMs"),
            kDefaultRefreshIntervalMs).toInt());
    return snapshot;
}

void UiSettings::save(const UiSettingsSnapshot& settingsSnapshot) {
    auto settings = settingsStore();
    settings.setValue(
        QStringLiteral("ui/startMinimized"),
        settingsSnapshot.startMinimized);
    settings.setValue(
        QStringLiteral("ui/confirmSeatStop"),
        settingsSnapshot.confirmSeatStop);
    settings.setValue(
        QStringLiteral("ui/refreshIntervalMs"),
        boundedRefreshInterval(settingsSnapshot.refreshIntervalMs));
    settings.sync();
}

} // namespace hydra::ui
