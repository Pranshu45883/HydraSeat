#include "ui/pages/settings_page.hpp"
#include <QFrame>
#include <QPushButton>

namespace hydra::ui {

SettingsPage::SettingsPage(QWidget* parent) : QWidget(parent) {
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");

    auto* container = new QWidget();
    container->setStyleSheet("background-color: transparent;");
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(24);

    auto* title = new QLabel("Settings", container);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5; font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);

    auto createSection = [&](const QString& name, const std::vector<QString>& toggles) {
        auto* l = new QLabel(name, container);
        l->setStyleSheet("font-size: 14px; font-weight: bold; color: #B5B5B5; letter-spacing: 1px; margin-top: 16px;");
        layout->addWidget(l);

        for (const auto& t : toggles) {
            auto* frame = new QFrame(container);
            frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 16px; border: 1px solid #292929;");
            frame->setMinimumWidth(400);
            frame->setMaximumWidth(600);

            auto* fl = new QHBoxLayout(frame);
            fl->setContentsMargins(0,0,0,0);
            
            auto* label = new QLabel(t, frame);
            label->setStyleSheet("font-size: 14px; color: #F5F5F5; border: none;");
            fl->addWidget(label);
            
            auto* btn = new QPushButton("Toggle", frame);
            btn->setFixedSize(70, 24);
            btn->setStyleSheet("QPushButton { background-color: #292929; color: #F5F5F5; border: 1px solid #333333; border-radius: 4px; } QPushButton:disabled { color: #777777; }");
            fl->addWidget(btn);

            auto* msg = new QLabel("Missing host capability: Settings state storage.", frame);
            msg->setStyleSheet("font-size: 12px; color: #E10600; border: none; margin-left: 8px;");
            msg->setVisible(false);
            fl->addWidget(msg);
            
            connect(btn, &QPushButton::clicked, [msg]() { msg->setVisible(true); });
            
            layout->addWidget(frame);
        }
    };

    createSection("GENERAL", {"Start HydraSeat with Windows", "Start minimized", "Confirm destructive actions"});
    createSection("APPEARANCE", {"Theme (Dark)", "Interface scaling (100%)"});
    createSection("BEHAVIOR", {"Refresh interval", "Notifications"});
    createSection("AUDIO", {"Default audio behavior"});

    layout->addStretch();
    scrollArea->setWidget(container);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(scrollArea);
}

} // namespace hydra::ui
