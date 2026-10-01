#include "ui/routing_controller.hpp"

namespace hydra::ui {
namespace {

QString audioStatusMessage(hydra::hostipc::AudioMutationStatus status) {
    using Status = hydra::hostipc::AudioMutationStatus;
    switch (status) {
    case Status::Success:
        return {};
    case Status::InvalidProcess:
        return "The host rejected the process identity or Seat lease.";
    case Status::ProcessNotFound:
        return "The target process no longer exists.";
    case Status::AudioSessionNotFound:
        return "No audio session exists for this process yet.";
    case Status::EndpointNotFound:
        return "The selected audio endpoint no longer exists.";
    case Status::EndpointUnavailable:
        return "The selected audio endpoint is currently unavailable.";
    case Status::IdentityMismatch:
        return "The process identity changed; refresh and try again.";
    case Status::RoutingFailed:
        return "Windows rejected the requested audio route.";
    case Status::OsApiError:
        return "Windows audio policy returned an operating-system error.";
    }
    return "Unknown host audio-routing failure.";
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
