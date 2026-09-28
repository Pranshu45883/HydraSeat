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
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5;");
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
    sectionTitle->setStyleSheet("font-size: 20px; font-weight: bold; color: #F5F5F5; margin-top: 16px; margin-bottom: 8px;");
    m_listLayout->addWidget(sectionTitle);

    for (const auto& dev : devices) {
        auto* frame = new QFrame();
        frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 12px; border: 1px solid #2A2A2A;");
        auto* fl = new QVBoxLayout(frame);
        
        auto* nameLabel = new QLabel(QString::fromStdWString(dev.name), frame);
        nameLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: #F5F5F5; border: none;");
        fl->addWidget(nameLabel);

        auto* detailsLabel = new QLabel("Status: Connected", frame);
        detailsLabel->setStyleSheet("font-size: 13px; color: #32D74B; border: none;");
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
        err->setStyleSheet("color: #E10600;");
        m_listLayout->addWidget(err);
        m_listLayout->addStretch();
        return;
    }

    addSection("DISPLAYS", payload.displays);
    addSection("KEYBOARDS", payload.keyboards);
    addSection("MICE", payload.mice);
    // New Controllers Rendering
    if (!payload.controllerInventory.physicalControllers.empty() || !payload.controllerInventory.sources.empty()) {
        auto* sectionTitle = new QLabel("CONTROLLERS");
        sectionTitle->setStyleSheet("font-size: 20px; font-weight: bold; color: #F5F5F5; margin-top: 16px; margin-bottom: 8px;");
        m_listLayout->addWidget(sectionTitle);

        // Physical Controllers
        for (const auto& phys : payload.controllerInventory.physicalControllers) {
            auto* frame = new QFrame();
            frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 12px; border: 1px solid #2A2A2A;");
            auto* fl = new QVBoxLayout(frame);
            
            auto* nameLabel = new QLabel(QString::fromStdWString(phys.displayName), frame);
            nameLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: #F5F5F5; border: none;");
            fl->addWidget(nameLabel);

            auto* identityLabel = new QLabel("Stable physical identity", frame);
            identityLabel->setStyleSheet("font-size: 12px; color: #777777; border: none;");
            fl->addWidget(identityLabel);

            auto* detailsLabel = new QLabel("● Connected", frame);
            detailsLabel->setStyleSheet("font-size: 13px; color: #32D74B; font-weight: bold; border: none;");
            fl->addWidget(detailsLabel);

            m_listLayout->addWidget(frame);
        }

        // Runtime Sources
        if (!payload.controllerInventory.sources.empty()) {
            auto* runtimeTitle = new QLabel("Runtime Sources");
            runtimeTitle->setStyleSheet("font-size: 16px; font-weight: bold; color: #B5B5B5; margin-top: 12px; margin-bottom: 4px;");
            m_listLayout->addWidget(runtimeTitle);

            for (const auto& src : payload.controllerInventory.sources) {
                auto* frame = new QFrame();
                frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 12px; border: 1px solid #2A2A2A; border-left: 4px solid #E10600;");
                auto* fl = new QVBoxLayout(frame);
                
                QString apiStr = src.api == hydra::controller::ApiSurface::XInput ? "XInput" : "DirectInput";
                auto* apiLabel = new QLabel(apiStr, frame);
                apiLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: #F5F5F5; border: none;");
                fl->addWidget(apiLabel);

                QString slotStr = src.runtimeXInputSlot ? QString::number(*src.runtimeXInputSlot) : "N/A";
                auto* metaLabel = new QLabel(QString("Slot %1\nGeneration %2").arg(slotStr).arg(src.sourceGeneration), frame);
                metaLabel->setStyleSheet("font-size: 13px; color: #777777; border: none;");
                fl->addWidget(metaLabel);

                QString statusStr = src.connected ? "● Connected" : "○ Disconnected";
                QString colorStr = src.connected ? "#32D74B" : "#E10600";
                auto* statusLabel = new QLabel(statusStr, frame);
                statusLabel->setStyleSheet(QString("font-size: 13px; font-weight: bold; color: %1; border: none;").arg(colorStr));
                fl->addWidget(statusLabel);

                m_listLayout->addWidget(frame);
            }
        }
    }
    
    // Audio endpoints are handled on Audio page primarily, but we can list them here too
    if (!payload.audioEndpoints.empty()) {
        auto* sectionTitle = new QLabel("AUDIO OUTPUTS");
        sectionTitle->setStyleSheet("font-size: 20px; font-weight: bold; color: #F5F5F5; margin-top: 16px; margin-bottom: 8px;");
        m_listLayout->addWidget(sectionTitle);

        for (const auto& ep : payload.audioEndpoints) {
            auto* frame = new QFrame();
            frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 12px; border: 1px solid #2A2A2A;");
            auto* fl = new QVBoxLayout(frame);
            
            auto* nameLabel = new QLabel(QString::fromStdWString(ep.friendlyName), frame);
            nameLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: #F5F5F5; border: none;");
            fl->addWidget(nameLabel);

            QString statusStr = ep.isAvailable() ? "Status: Connected" : "Status: Unavailable";
            QString colorStr = ep.isAvailable() ? "#32D74B" : "#E10600";
            auto* detailsLabel = new QLabel(statusStr, frame);
            detailsLabel->setStyleSheet(QString("font-size: 13px; color: %1; border: none;").arg(colorStr));
            fl->addWidget(detailsLabel);

            m_listLayout->addWidget(frame);
        }
    }

    m_listLayout->addStretch();
}

} // namespace hydra::ui
