#include "ui/routing_controller.hpp"

namespace hydra::ui {
namespace {

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
            RouteVerificationResult::ProcessIdentityValidationFailure,
            "A valid host-owned process identity is required.");
        return;
    }

    std::string error;
    const auto status = m_hostControl->routeAudio(
        pid,
        creationIdentity,
        endpointId.toStdString(),
        &error);
    if (!status) {
        emit routingCompleted(
            pid,
            RouteVerificationResult::ProcessIdentityValidationFailure,
            error.empty()
                ? "Could not reach the canonical HydraSeat host."
                : QString::fromStdString(error));
        return;
    }

    if (*status == hydra::hostipc::AudioMutationStatus::Success) {
        emit routingCompleted(pid, RouteVerificationResult::Success, {});
        return;
    }

    emit routingCompleted(
        pid,
        RouteVerificationResult::FailedRollbackFailed,
        audioStatusMessage(*status));
}

void RoutingController::requestReset(
    std::uint32_t pid,
    std::uint64_t creationIdentity) {
    if (!m_hostControl || pid == 0 || creationIdentity == 0) {
        emit resetCompleted(
            pid,
            false,
            "A valid host-owned process identity is required.");
        return;
    }

    std::string error;
    const auto status =
        m_hostControl->resetAudio(pid, creationIdentity, &error);
    if (!status) {
        emit resetCompleted(
            pid,
            false,
            error.empty()
                ? "Could not reach the canonical HydraSeat host."
                : QString::fromStdString(error));
        return;
    }

    if (*status == hydra::hostipc::AudioMutationStatus::Success) {
        emit resetCompleted(pid, true, {});
        return;
    }

    emit resetCompleted(pid, false, audioStatusMessage(*status));
}

} // namespace hydra::ui
