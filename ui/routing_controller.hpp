#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <memory>

#include "ui/host_control_client.hpp"

namespace hydra::ui {

enum class RouteVerificationResult {
    Success,
    FailedRollbackSuccess,
    FailedRollbackFailed,
    CrossSeatIsolationFailure,
    ProcessIdentityValidationFailure
};

// Thin UI adapter over the persistent host Control connection.
// It never constructs or owns SessionController/RuntimeHost authority.
class RoutingController : public QObject {
    Q_OBJECT
public:
    explicit RoutingController(
        std::shared_ptr<HostControlClient> hostControl,
        QObject* parent = nullptr);
    ~RoutingController() override = default;

    void requestRoute(
        std::uint32_t pid,
        std::uint64_t creationIdentity,
        const QString& endpointId);
    void requestReset(
        std::uint32_t pid,
        std::uint64_t creationIdentity);

signals:
    void routingCompleted(
        std::uint32_t pid,
        RouteVerificationResult result,
        const QString& errorMessage);
    void resetCompleted(
        std::uint32_t pid,
        bool success,
        const QString& errorMessage);

private:
    std::shared_ptr<HostControlClient> m_hostControl;
};

} // namespace hydra::ui
