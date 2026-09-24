#include "ui/pages/hardware_page.hpp"
#include <QLabel>
#include <QFrame>

namespace hydra::ui {

HardwarePage::HardwarePage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent)
    : QWidget(parent), m_sessionController(std::move(sessionController)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(24);

    auto* title = new QLabel("Hardware Inventory", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: white;");
    layout->addWidget(title);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");

    auto* listContainer = new QWidget(scrollArea);
    listContainer->setStyleSheet("background-color: transparent;");
    m_listLayout = new QVBoxLayout(listContainer);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(12);
    m_listLayout->addStretch();

    scrollArea->setWidget(listContainer);
    layout->addWidget(scrollArea);
}

void HardwarePage::addSection(const QString& title, const std::vector<hydra::DeviceInfo>& devices) {
    if (devices.empty()) return;

    auto* sectionTitle = new QLabel(title);
    sectionTitle->setStyleSheet("font-size: 20px; font-weight: bold; color: white; margin-top: 16px; margin-bottom: 8px;");
    m_listLayout->addWidget(sectionTitle);

    for (const auto& dev : devices) {
        auto* frame = new QFrame();
        frame->setStyleSheet("background-color: #1A1A1A; border-radius: 6px; padding: 12px;");
        auto* fl = new QVBoxLayout(frame);
        
        auto* nameLabel = new QLabel(QString::fromStdWString(dev.name), frame);
        nameLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: white;");
        fl->addWidget(nameLabel);

        auto* detailsLabel = new QLabel("Status: Connected", frame);
        detailsLabel->setStyleSheet("font-size: 13px; color: #00FF00;");
        fl->addWidget(detailsLabel);

        m_listLayout->addWidget(frame);
    }
}

void HardwarePage::updateState(const EngineStatePayload& payload) {
    while (QLayoutItem* item = m_listLayout->takeAt(0)) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    if (payload.hardwareError) {
        auto* err = new QLabel("Failed to enumerate hardware.");
        err->setStyleSheet("color: #FF5555;");
        m_listLayout->addWidget(err);
        m_listLayout->addStretch();
        return;
    }

    addSection("DISPLAYS", payload.displays);
    addSection("KEYBOARDS", payload.keyboards);
    addSection("MICE", payload.mice);
    addSection("CONTROLLERS", payload.controllers);
    
    // Audio endpoints are handled on Audio page primarily, but we can list them here too
    if (!payload.audioEndpoints.empty()) {
        auto* sectionTitle = new QLabel("AUDIO OUTPUTS");
        sectionTitle->setStyleSheet("font-size: 20px; font-weight: bold; color: white; margin-top: 16px; margin-bottom: 8px;");
        m_listLayout->addWidget(sectionTitle);

        for (const auto& ep : payload.audioEndpoints) {
            auto* frame = new QFrame();
            frame->setStyleSheet("background-color: #1A1A1A; border-radius: 6px; padding: 12px;");
            auto* fl = new QVBoxLayout(frame);
            
            auto* nameLabel = new QLabel(QString::fromStdWString(ep.friendlyName), frame);
            nameLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: white;");
            fl->addWidget(nameLabel);

            QString statusStr = ep.isAvailable() ? "Status: Connected" : "Status: Unavailable";
            QString colorStr = ep.isAvailable() ? "#00FF00" : "#FF5555";
            auto* detailsLabel = new QLabel(statusStr, frame);
            detailsLabel->setStyleSheet(QString("font-size: 13px; color: %1;").arg(colorStr));
            fl->addWidget(detailsLabel);

            m_listLayout->addWidget(frame);
        }
    }

    m_listLayout->addStretch();
}

} // namespace hydra::ui
