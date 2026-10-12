#pragma once

#include <QString>
#include <QVector>

namespace hydra::ui {

struct ApplicationLaunchEntry {
    QString title;
    QString executablePath;
    QString arguments;
    QString workingDirectory;

    bool operator==(const ApplicationLaunchEntry&) const = default;
};

class ApplicationLibrary final {
public:
    static constexpr int kMaximumEntries = 64;

    static QVector<ApplicationLaunchEntry> load();
    static void remember(const ApplicationLaunchEntry& entry);
};

} // namespace hydra::ui
