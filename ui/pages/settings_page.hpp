#pragma once

#include <QWidget>

class QPushButton;

namespace hydra::ui {

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(QWidget* parent = nullptr);

signals:
    void preferencesChanged();

private:
    void savePreferences();
    void refreshButtonLabels();

    QPushButton* m_startWithWindowsButton{nullptr};
    QPushButton* m_startMinimizedButton{nullptr};
    QPushButton* m_confirmSeatStopButton{nullptr};
    QPushButton* m_refreshIntervalButton{nullptr};

    bool m_startWithWindows{false};
    bool m_startMinimized{false};
    bool m_confirmSeatStop{true};
    int m_refreshIntervalMs{2000};
};

} // namespace hydra::ui
