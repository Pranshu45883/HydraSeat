#include "ui/pages/settings_page.hpp"

#include "ui/ui_settings.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

#include <functional>
#include <vector>

namespace hydra::ui {
namespace {

const char* toggleButtonStyle() {
    return
        "QPushButton { background-color: #292929; color: #F5F5F5; "
        "border: 1px solid #333333; border-radius: 4px; } "
        "QPushButton:hover { background-color: #333333; } "
        "QPushButton:disabled { background-color: #202020; color: #777777; }";
}

QPushButton* makeSettingButton(
    const QString& text,
    QFrame* frame,
    bool enabled = true) {
    auto* button = new QPushButton(text, frame);
    button->setFixedSize(70, 24);
    button->setStyleSheet(toggleButtonStyle());
    button->setEnabled(enabled);
    return button;
}

QString startupCommand() {
    return QStringLiteral("\"%1\"").arg(
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
}

QSettings startupRegistry() {
    return QSettings(
        QStringLiteral(
            "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QSettings::NativeFormat);
}

bool startupEnabled() {
    auto settings = startupRegistry();
    return settings.value(QStringLiteral("HydraSeat")).toString() ==
           startupCommand();
}

bool setStartupEnabled(bool enabled) {
    auto settings = startupRegistry();
    if (enabled) {
        settings.setValue(QStringLiteral("HydraSeat"), startupCommand());
    } else {
        settings.remove(QStringLiteral("HydraSeat"));
    }
    settings.sync();
    return settings.status() == QSettings::NoError;
}

int nextRefreshInterval(int current) {
    if (current < 1500) return 2000;
    if (current < 3500) return 5000;
    return 1000;
}

QString refreshIntervalLabel(int milliseconds) {
    if (milliseconds % 1000 == 0) {
        return QStringLiteral("%1 s").arg(milliseconds / 1000);
    }
    return QStringLiteral("%1 ms").arg(milliseconds);
}

} // namespace

SettingsPage::SettingsPage(QWidget* parent)
    : QWidget(parent) {
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet(
        "QScrollArea { border: none; background-color: transparent; }");

    auto* container = new QWidget();
    container->setStyleSheet("background-color: transparent;");
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(24);

    auto* title = new QLabel("Settings", container);
    title->setStyleSheet(
        "font-size: 28px; font-weight: bold; color: #F5F5F5; "
        "font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);

    const auto saved = UiSettings::load();
    m_startWithWindows = startupEnabled();
    m_startMinimized = saved.startMinimized;
    m_confirmSeatStop = saved.confirmSeatStop;
    m_refreshIntervalMs = saved.refreshIntervalMs;

    const auto addSectionTitle = [&](const QString& name) {
        auto* label = new QLabel(name, container);
        label->setStyleSheet(
            "font-size: 14px; font-weight: bold; color: #B5B5B5; "
            "letter-spacing: 1px; margin-top: 16px;");
        layout->addWidget(label);
    };

    const auto addSetting = [&](const QString& labelText,
                                const QString& buttonText,
                                bool enabled,
                                const std::function<void(QPushButton*)>& bind)
        -> QPushButton* {
        auto* frame = new QFrame(container);
        frame->setStyleSheet(
            "background-color: #151515; border-radius: 6px; padding: 16px; "
            "border: 1px solid #292929;");
        frame->setMinimumWidth(400);
        frame->setMaximumWidth(600);

        auto* row = new QHBoxLayout(frame);
        row->setContentsMargins(0, 0, 0, 0);

        auto* label = new QLabel(labelText, frame);
        label->setStyleSheet(
            "font-size: 14px; color: #F5F5F5; border: none;");
        row->addWidget(label);

        auto* button = makeSettingButton(buttonText, frame, enabled);
        row->addWidget(button);
        if (bind) bind(button);

        layout->addWidget(frame);
        return button;
    };

    addSectionTitle("GENERAL");
    m_startWithWindowsButton = addSetting(
        "Start HydraSeat with Windows",
        "Off",
        true,
        [this](QPushButton* button) {
            connect(button, &QPushButton::clicked, this, [this]() {
                const bool wanted = !m_startWithWindows;
                if (setStartupEnabled(wanted)) {
                    m_startWithWindows = wanted;
                }
                refreshButtonLabels();
            });
        });
    m_startMinimizedButton = addSetting(
        "Start minimized",
        "Off",
        true,
        [this](QPushButton* button) {
            connect(button, &QPushButton::clicked, this, [this]() {
                m_startMinimized = !m_startMinimized;
                savePreferences();
            });
        });
    m_confirmSeatStopButton = addSetting(
        "Confirm destructive actions",
        "On",
        true,
        [this](QPushButton* button) {
            connect(button, &QPushButton::clicked, this, [this]() {
                m_confirmSeatStop = !m_confirmSeatStop;
                savePreferences();
            });
        });

    addSectionTitle("APPEARANCE");
    addSetting("Theme (Dark)", "Dark", false, {});
    addSetting("Interface scaling (100%)", "100%", false, {});

    addSectionTitle("BEHAVIOR");
    m_refreshIntervalButton = addSetting(
        "Refresh interval",
        refreshIntervalLabel(m_refreshIntervalMs),
        true,
        [this](QPushButton* button) {
            connect(button, &QPushButton::clicked, this, [this]() {
                m_refreshIntervalMs =
                    nextRefreshInterval(m_refreshIntervalMs);
                savePreferences();
            });
        });
    addSetting("Notifications", "Off", false, {});

    addSectionTitle("AUDIO");
    addSetting("Default audio behavior", "System", false, {});

    layout->addStretch();
    scrollArea->setWidget(container);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(scrollArea);

    refreshButtonLabels();
}

void SettingsPage::refreshButtonLabels() {
    if (m_startWithWindowsButton) {
        m_startWithWindowsButton->setText(
            m_startWithWindows ? "On" : "Off");
    }
    if (m_startMinimizedButton) {
        m_startMinimizedButton->setText(
            m_startMinimized ? "On" : "Off");
    }
    if (m_confirmSeatStopButton) {
        m_confirmSeatStopButton->setText(
            m_confirmSeatStop ? "On" : "Off");
    }
    if (m_refreshIntervalButton) {
        m_refreshIntervalButton->setText(
            refreshIntervalLabel(m_refreshIntervalMs));
    }
}

void SettingsPage::savePreferences() {
    UiSettingsSnapshot settings;
    settings.startMinimized = m_startMinimized;
    settings.confirmSeatStop = m_confirmSeatStop;
    settings.refreshIntervalMs = m_refreshIntervalMs;
    UiSettings::save(settings);

    refreshButtonLabels();
    emit preferencesChanged();
}

} // namespace hydra::ui
