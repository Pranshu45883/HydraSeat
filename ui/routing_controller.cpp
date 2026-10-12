#include "ui/routing_controller.hpp"

#include <QByteArray>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <optional>
#include <string>

namespace hydra::ui {
namespace {

struct AudioTaskResult {
    std::optional<hydra::hostipc::AudioMutationStatus> status;
    std::string error;
};

QString audioStatusMessage(hydra::hostipc::AudioMutationStatus status) {
    using Status = hydra::hostipc::AudioMutationStatus;
    switch (status) {
    case Status::Success:
        return {};
    case Status::InvalidProcess:
        return "This application is no longer available.";
    case Status::ProcessNotFound:
        return "This application is no longer available.";
    case Status::AudioSessionNotFound:
        return "No audio session exists for this process yet.";
    case Status::EndpointNotFound:
        return "The selected audio device is unavailable.";
    case Status::EndpointUnavailable:
        return "The selected audio device is unavailable.";
    case Status::IdentityMismatch:
        return "The configuration changed before the operation completed.";
    case Status::RoutingFailed:
        return "Audio routing failed. Verify the current output before trying again.";
    case Status::OsApiError:
        return "Something went wrong. Check Diagnostics for more information.";
    }
    return "Something went wrong. Check Diagnostics for more information.";
}

} // namespace

RoutingController::RoutingController(
    std::shared_ptr<HostControlClient> hostControl,
    QObject* parent)
    : QObject(parent),
      m_hostControl(std::move(hostControl)) {}

void RoutingController::requestRoute(
    std::uint32_t pid,
    std::uint64_t creationIdentity,
    const QString& endpointId) {
    if (!m_hostControl || pid == 0 || creationIdentity == 0) {
        emit routingCompleted(
            pid,
            creationIdentity,
            RouteVerificationResult::ProcessIdentityValidationFailure,
            "A valid host-owned process identity is required.");
        return;
    }

    const QByteArray endpointUtf8 = endpointId.toUtf8();
    const std::string endpoint(
        endpointUtf8.constData(),
        static_cast<std::size_t>(endpointUtf8.size()));
    auto hostControl = m_hostControl;

    auto* watcher = new QFutureWatcher<AudioTaskResult>(this);
    connect(
        watcher,
        &QFutureWatcher<AudioTaskResult>::finished,
        this,
        [this, watcher, pid, creationIdentity]() {
            const auto result = watcher->result();
            watcher->deleteLater();

            if (!result.status) {
                emit routingCompleted(
                    pid,
                    creationIdentity,
                    RouteVerificationResult::ProcessIdentityValidationFailure,
                    result.error.empty()
                        ? "Could not reach the canonical HydraSeat host."
                        : QString::fromStdString(result.error));
                return;
            }
            if (*result.status ==
                hydra::hostipc::AudioMutationStatus::Success) {
                emit routingCompleted(
                    pid,
                    creationIdentity,
                    RouteVerificationResult::Success,
                    {});
                return;
            }
            emit routingCompleted(
                pid,
                creationIdentity,
                RouteVerificationResult::FailedRollbackFailed,
                audioStatusMessage(*result.status));
        });

    watcher->setFuture(QtConcurrent::run(
        [hostControl, pid, creationIdentity, endpoint]() {
            AudioTaskResult result;
            result.status = hostControl->routeAudio(
                pid,
                creationIdentity,
                endpoint,
                &result.error);
            return result;
        }));
}

void RoutingController::requestReset(
    std::uint32_t pid,
    std::uint64_t creationIdentity) {
    if (!m_hostControl || pid == 0 || creationIdentity == 0) {
        emit resetCompleted(
            pid,
            creationIdentity,
            false,
            "A valid host-owned process identity is required.");
        return;
    }

    auto hostControl = m_hostControl;
    auto* watcher = new QFutureWatcher<AudioTaskResult>(this);
    connect(
        watcher,
        &QFutureWatcher<AudioTaskResult>::finished,
        this,
        [this, watcher, pid, creationIdentity]() {
            const auto result = watcher->result();
            watcher->deleteLater();

            if (!result.status) {
                emit resetCompleted(
                    pid,
                    creationIdentity,
                    false,
                    result.error.empty()
                        ? "Could not reach the canonical HydraSeat host."
                        : QString::fromStdString(result.error));
                return;
            }
            if (*result.status ==
                hydra::hostipc::AudioMutationStatus::Success) {
                emit resetCompleted(pid, creationIdentity, true, {});
                return;
            }
            emit resetCompleted(
                pid,
                creationIdentity,
                false,
                audioStatusMessage(*result.status));
        });

    watcher->setFuture(QtConcurrent::run(
        [hostControl, pid, creationIdentity]() {
            AudioTaskResult result;
            result.status = hostControl->resetAudio(
                pid,
                creationIdentity,
                &result.error);
            return result;
        }));
}

} // namespace hydra::ui
