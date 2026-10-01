#pragma once

#include <QObject>
#include <QThread>
#include <QTimer>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hydra/audio_endpoint_inventory.hpp"
#include "hydra/audio_session_observer.hpp"
#include "hydra/controller_inventory.hpp"
#include "hydra/hardware_detector.hpp"
#include "hydra/host_transport.hpp"

namespace hydra::ui {

struct EngineStatePayload {
    std::vector<hydra::DeviceInfo> displays;
    std::vector<hydra::DeviceInfo> keyboards;
    std::vector<hydra::DeviceInfo> mice;
    std::vector<hydra::DeviceInfo> controllers;
    std::vector<hydra::windows::AudioRenderEndpoint> audioEndpoints;
    std::vector<hydra::windows::AudioSessionObservation> audioSessions;
    hydra::controller::InventorySnapshot controllerInventory;
    std::optional<hydra::hostipc::HostSnapshot> hostSnapshot;
    std::string hostError;
    bool hostConnected{false};
    bool hardwareError{false};
    bool audioEndpointError{false};
    bool audioSessionError{false};
};

class EnginePollerWorker : public QObject {
    Q_OBJECT
public:
    explicit EnginePollerWorker(std::shared_ptr<hydra::HardwareDetector> hardwareDetector);
    ~EnginePollerWorker() override = default;

public slots:
    void doPoll();

signals:
    void pollCompleted(hydra::ui::EngineStatePayload payload);

private:
    std::shared_ptr<hydra::HardwareDetector> m_hardwareDetector;
    hydra::controller::ControllerInventory m_controllerInventory;
    hydra::hostipc::HostPipeClient m_hostClient;
};

class EnginePoller : public QObject {
    Q_OBJECT

public:
    explicit EnginePoller(std::shared_ptr<hydra::HardwareDetector> hardwareDetector, QObject* parent = nullptr);
    ~EnginePoller() override;

    void startPolling(int intervalMs = 2000);
    void stopPolling();

signals:
    void stateUpdated(hydra::ui::EngineStatePayload payload);
    void triggerPoll();

private:
    QThread m_workerThread;
    EnginePollerWorker* m_worker{nullptr};
    QTimer* m_triggerTimer{nullptr};
};

} // namespace hydra::ui

Q_DECLARE_METATYPE(hydra::ui::EngineStatePayload)
