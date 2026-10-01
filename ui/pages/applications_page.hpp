#pragma once

#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>
#include <optional>

#include "hydra/process_identity.hpp"
#include "ui/engine_poller.hpp"

namespace hydra::ui {

class ApplicationsPage : public QWidget {
    Q_OBJECT
public:
    explicit ApplicationsPage(QWidget* parent = nullptr);

public slots:
    void updateState(const EngineStatePayload& payload);

private:
    QVBoxLayout* m_listLayout{nullptr};

    static QString getAssignedSeat(
        const std::optional<hydra::runtime::ProcessIdentity>& identity,
        const std::optional<hydra::hostipc::HostSnapshot>& hostSnapshot);
};

} // namespace hydra::ui
