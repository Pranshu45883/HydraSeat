#pragma once

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>

#include "hydra/process_identity.hpp"
#include "ui/engine_poller.hpp"
#include "ui/host_control_client.hpp"

namespace hydra::ui {

class ApplicationsPage : public QWidget {
    Q_OBJECT
public:
    explicit ApplicationsPage(
        std::shared_ptr<HostControlClient> hostControl,
        QWidget* parent = nullptr);

public slots:
    void updateState(const EngineStatePayload& payload);

private slots:
    void onBrowseExecutable();
    void onLaunchRequested();
    void onStopRequested();
    void refreshLaunchControls();

private:
    std::shared_ptr<HostControlClient> m_hostControl;
    EngineStatePayload m_lastPayload;

    QComboBox* m_seatCombo{nullptr};
    QLineEdit* m_executableEdit{nullptr};
    QLineEdit* m_argumentsEdit{nullptr};
    QPushButton* m_launchButton{nullptr};
    QPushButton* m_stopButton{nullptr};
    QLabel* m_launchFeedback{nullptr};
    QLineEdit* m_searchBox{nullptr};
    QComboBox* m_filterCombo{nullptr};
    QVBoxLayout* m_listLayout{nullptr};

    static QString getAssignedSeat(
        const std::optional<hydra::runtime::ProcessIdentity>& identity,
        const std::optional<hydra::hostipc::HostSnapshot>& hostSnapshot);
    const hydra::hostipc::SeatSnapshot* selectedSeatSnapshot() const noexcept;
    std::uint32_t selectedSeatId() const noexcept;
    void setLaunchFeedback(const QString& text, bool error);
};

} // namespace hydra::ui
