#include "ui/application_library.hpp"

#include <QDir>
#include <QFileInfo>
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

QString canonicalKey(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

ApplicationLaunchEntry normalized(ApplicationLaunchEntry value) {
    const QFileInfo executable(value.executablePath);
    value.executablePath = QDir::cleanPath(executable.absoluteFilePath());
    if (value.workingDirectory.trimmed().isEmpty()) {
        value.workingDirectory = executable.absolutePath();
    } else {
        value.workingDirectory =
            QDir::cleanPath(QFileInfo(value.workingDirectory).absoluteFilePath());
    }
    if (value.title.trimmed().isEmpty()) {
        value.title = executable.completeBaseName();
        if (value.title.isEmpty()) value.title = executable.fileName();
    }
    value.title = value.title.trimmed();
    value.arguments = value.arguments.trimmed();
    return value;
}

bool validEntry(const ApplicationLaunchEntry& entry) {
    const QFileInfo executable(entry.executablePath);
    return executable.isAbsolute() &&
           executable.exists() &&
           executable.isFile() &&
           executable.suffix().compare(
               QStringLiteral("exe"), Qt::CaseInsensitive) == 0;
}

void saveEntries(const QVector<ApplicationLaunchEntry>& entries) {
    auto settings = settingsStore();
    settings.beginWriteArray(QStringLiteral("applications/library"));
    for (int index = 0; index < entries.size(); ++index) {
        settings.setArrayIndex(index);
        settings.setValue(QStringLiteral("title"), entries[index].title);
        settings.setValue(
            QStringLiteral("executablePath"), entries[index].executablePath);
        settings.setValue(QStringLiteral("arguments"), entries[index].arguments);
        settings.setValue(
            QStringLiteral("workingDirectory"),
            entries[index].workingDirectory);
    }
    settings.endArray();
    settings.sync();
}

} // namespace

QVector<ApplicationLaunchEntry> ApplicationLibrary::load() {
    auto settings = settingsStore();
    const int count = settings.beginReadArray(
        QStringLiteral("applications/library"));

    QVector<ApplicationLaunchEntry> entries;
    entries.reserve(std::min(count, kMaximumEntries));
    for (int index = 0;
         index < count && entries.size() < kMaximumEntries;
         ++index) {
        settings.setArrayIndex(index);
        ApplicationLaunchEntry entry;
        entry.title = settings.value(QStringLiteral("title")).toString();
        entry.executablePath =
            settings.value(QStringLiteral("executablePath")).toString();
        entry.arguments =
            settings.value(QStringLiteral("arguments")).toString();
        entry.workingDirectory =
            settings.value(QStringLiteral("workingDirectory")).toString();
        entry = normalized(std::move(entry));
        if (!validEntry(entry)) continue;

        const auto key = canonicalKey(entry.executablePath);
        const bool duplicate = std::any_of(
            entries.cbegin(), entries.cend(),
            [&](const ApplicationLaunchEntry& existing) {
                return canonicalKey(existing.executablePath) == key;
            });
        if (!duplicate) entries.push_back(std::move(entry));
    }
    settings.endArray();
    return entries;
}

void ApplicationLibrary::remember(const ApplicationLaunchEntry& rawEntry) {
    auto entry = normalized(rawEntry);
    if (!validEntry(entry)) return;

    auto entries = load();
    const auto key = canonicalKey(entry.executablePath);
    for (qsizetype index = entries.size(); index-- > 0;) {
        if (canonicalKey(entries[index].executablePath) == key) {
            entries.removeAt(index);
        }
    }
    entries.prepend(std::move(entry));
    while (entries.size() > kMaximumEntries) entries.removeLast();
    saveEntries(entries);
}

} // namespace hydra::ui
