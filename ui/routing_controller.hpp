#pragma once

#include <QObject>
#include <QThread>
#include <QString>
#include <memory>
#include <cstdint>

#include "hydra/runtime_authority.hpp"
#include "hydra/audio_router.hpp"

namespace hydra::ui {

class RoutingWorker : public QObject {
    Q_OBJECT
public:
    explicit RoutingWorker(std::shared_ptr<hydra::runtime::SessionController> sessionController);
    ~RoutingWorker() override = default;

public slots:
    void doRoute(uint32_t pid, const QString& endpointId);
    void doReset(uint32_t pid);

signals:
    void routingCompleted(uint32_t pid, bool success, const QString& errorMessage);
    void resetCompleted(uint32_t pid, bool success, const QString& errorMessage);

private:
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
};

class RoutingController : public QObject {
    Q_OBJECT
public:
    explicit RoutingController(std::shared_ptr<hydra::runtime::SessionController> sessionController, QObject* parent = nullptr);
    ~RoutingController() override;

    void requestRoute(uint32_t pid, const QString& endpointId);
    void requestReset(uint32_t pid);

signals:
    void triggerRoute(uint32_t pid, const QString& endpointId);
    void triggerReset(uint32_t pid);

    void routingCompleted(uint32_t pid, bool success, const QString& errorMessage);
    void resetCompleted(uint32_t pid, bool success, const QString& errorMessage);

private:
    QThread m_workerThread;
    RoutingWorker* m_worker{nullptr};
};

} // namespace hydra::ui
