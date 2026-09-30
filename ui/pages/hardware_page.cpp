#include "ui/pages/hardware_page.hpp"
#include <QFrame>

namespace hydra::ui {

HardwarePage::HardwarePage(std::shared_ptr<hydra::runtime::SessionController> sessionController, QWidget* parent)
    : QWidget(parent), m_sessionController(std::move(sessionController)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 32, 32, 32);
    layout->setSpacing(24);

    auto* title = new QLabel("Hardware Inventory", this);
    title->setStyleSheet("font-size: 28px; font-weight: bold; color: #F5F5F5; font-family: 'Segoe UI', sans-serif;");
    layout->addWidget(title);
    
    auto* subtitle = new QLabel("Detected multi-seat peripherals and controller devices", this);
    subtitle->setStyleSheet("font-size: 14px; color: #B5B5B5; font-family: 'Segoe UI', sans-serif; margin-bottom: 8px;");
    layout->addWidget(subtitle);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background-color: transparent; }");

    auto* listContainer = new QWidget(scrollArea);
    listContainer->setStyleSheet("background-color: transparent;");
    m_listLayout = new QVBoxLayout(listContainer);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(24);
    m_listLayout->addStretch();

    scrollArea->setWidget(listContainer);
    layout->addWidget(scrollArea);
}

void HardwarePage::addSection(const QString& title, const std::vector<hydra::DeviceInfo>& devices) {
    if (devices.empty()) return;

    auto* sectionWidget = new QWidget();
    auto* sl = new QVBoxLayout(sectionWidget);
    sl->setContentsMargins(0,0,0,0);
    sl->setSpacing(8);

    auto* sectionTitle = new QLabel(title);
    sectionTitle->setStyleSheet("font-size: 14px; font-weight: bold; color: #777777; font-family: 'Segoe UI', sans-serif; text-transform: uppercase;");
    sl->addWidget(sectionTitle);

    auto* gridLayout = new QGridLayout();
    gridLayout->setSpacing(12);

    int row = 0;
    int col = 0;
    for (const auto& dev : devices) {
        auto* frame = new QFrame();
        frame->setMinimumWidth(300);
        frame->setMaximumWidth(400);
        frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 12px 16px; border: 1px solid #292929;");
        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(0,0,0,0);
        
        auto* nameLabel = new QLabel(QString::fromStdWString(dev.name), frame);
        nameLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: #F5F5F5; border: none;");
        fl->addWidget(nameLabel);

        auto* detailsLabel = new QLabel("● Connected", frame);
        detailsLabel->setStyleSheet("font-size: 13px; color: #777777; border: none;");
        fl->addWidget(detailsLabel);

        gridLayout->addWidget(frame, row, col);
        col++;
        if (col >= 2) { col = 0; row++; }
    }
    
    gridLayout->setColumnStretch(2, 1);
    sl->addLayout(gridLayout);
    m_listLayout->insertWidget(m_listLayout->count() - 1, sectionWidget);
}

void HardwarePage::addControllerSection(const hydra::controller::InventorySnapshot& inventory) {
    if (inventory.physicalControllers.empty() && inventory.sources.empty()) return;
    
    auto* sectionWidget = new QWidget();
    auto* sl = new QVBoxLayout(sectionWidget);
    sl->setContentsMargins(0,0,0,0);
    sl->setSpacing(8);

    auto* sectionTitle = new QLabel("CONTROLLERS");
    sectionTitle->setStyleSheet("font-size: 14px; font-weight: bold; color: #777777; font-family: 'Segoe UI', sans-serif; text-transform: uppercase;");
    sl->addWidget(sectionTitle);

    auto* gridLayout = new QGridLayout();
    gridLayout->setSpacing(12);

    int row = 0;
    int col = 0;
    for (const auto& phys : inventory.physicalControllers) {
        auto* frame = new QFrame();
        frame->setMinimumWidth(300);
        frame->setMaximumWidth(400);
        frame->setStyleSheet("background-color: #151515; border-radius: 6px; padding: 12px 16px; border: 1px solid #292929;");
        auto* fl = new QVBoxLayout(frame);
        fl->setContentsMargins(0,0,0,0);
        fl->setSpacing(4);
        
        auto* nameLabel = new QLabel(QString::fromStdWString(phys.displayName), frame);
        nameLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: #F5F5F5; border: none;");
        fl->addWidget(nameLabel);

        auto* identityLabel = new QLabel(QString::fromStdWString(phys.persistentId), frame);
        identityLabel->setStyleSheet("font-size: 12px; color: #777777; font-family: 'Consolas', monospace; border: none;");
        fl->addWidget(identityLabel);

        auto* detailsLabel = new QLabel("● Connected", frame);
        detailsLabel->setStyleSheet("font-size: 13px; color: #777777; border: none; margin-top: 4px;");
        fl->addWidget(detailsLabel);

        gridLayout->addWidget(frame, row, col);
        col++;
        if (col >= 2) { col = 0; row++; }
    }
    
    gridLayout->setColumnStretch(2, 1);
    sl->addLayout(gridLayout);
    m_listLayout->insertWidget(m_listLayout->count() - 1, sectionWidget);
}

void HardwarePage::updateState(const EngineStatePayload& payload) {
    while (QLayoutItem* item = m_listLayout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    m_listLayout->addStretch();

    if (payload.hardwareError) {
        auto* err = new QLabel("Failed to enumerate hardware.");
        err->setStyleSheet("color: #E10600; font-size: 14px; font-weight: bold;");
        m_listLayout->insertWidget(0, err);
        return;
    }

    addSection("DISPLAYS", payload.displays);
    addSection("KEYBOARDS", payload.keyboards);
    addSection("MICE", payload.mice);
    addControllerSection(payload.controllerInventory);
}

} // namespace hydra::ui
