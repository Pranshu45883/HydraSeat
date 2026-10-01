#pragma once

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QScrollArea>

namespace hydra::ui {

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(QWidget* parent = nullptr);
};

} // namespace hydra::ui
