#pragma once

#include <QObject>
#include <QThread>
#include <memory>
#include <vector>
#include <optional>

#include "hydra/hardware_detector.hpp"
#include "hydra/audio_endpoint_inventory.hpp"
#include "hydra/audio_session_observer.hpp"

namespace hydra::ui {

// Represents the payload for UI updates
struct EngineStatePayload {
    std::vector<hydra::DeviceInfo> displays;
    std::vector<hydra::DeviceInfo> keyboards;
    std::vector<hydra::DeviceInfo> mice;
    std::vector<hydra::DeviceInfo> controllers;
    std::vector<hydra::windows::AudioRenderEndpoint> audioEndpoints;
    std::vector<hydra::windows::AudioSessionObservation> audioSessions;
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
    EnginePollerWorker* m_worker{nullptr}; // Owned by m_workerThread
    QTimer* m_triggerTimer{nullptr};
};

} // namespace hydra::ui

// We need to declare the struct as a Qt metatype to pass it across threads via signals
Q_DECLARE_METATYPE(hydra::ui::EngineStatePayload)
