#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QFrame>
#include <QPixmap>
#include <QMap>
#include <QHash>
#include <memory>
#include <vector>
#include <string>

#include "hydra/runtime_authority.hpp"
#include "hydra/audio_endpoint_inventory.hpp"
#include "hydra/audio_session_observer.hpp"
#include "ui/engine_poller.hpp"
#include "ui/routing_controller.hpp"

namespace hydra::ui {

// Represents one fully-built session card and its dynamic sub-widgets.
// Stored so we can diff-update rather than fully rebuild every 2s.
struct AudioSessionCard {
    uint32_t pid{0};
    uint64_t creationIdentity{0};
    hydra::windows::AudioSessionState state{hydra::windows::AudioSessionState::Unknown};
    std::wstring endpointId;
    std::optional<std::wstring> displayName;

    QFrame* frame{nullptr};
    QLabel* stateLabel{nullptr};
    QLabel* currentOutputLabel{nullptr};
    QComboBox* endpointCombo{nullptr};
    QPushButton* routeBtn{nullptr};
    QPushButton* resetBtn{nullptr};
    QLabel* feedbackLabel{nullptr};
};

class AudioPage : public QWidget {
    Q_OBJECT
public:
    explicit AudioPage(
        std::shared_ptr<hydra::runtime::SessionController> sessionController,
        RoutingController* routingController,
        QWidget* parent = nullptr);

public slots:
    void updateState(const EngineStatePayload& payload);

private slots:
    void onRouteRequested(uint32_t pid, uint64_t creationIdentity, const QString& endpointId);
    void onResetRequested(uint32_t pid, uint64_t creationIdentity);
    void onRoutingCompleted(uint32_t pid, bool success, const QString& errorMessage);
    void onResetCompleted(uint32_t pid, bool success, const QString& errorMessage);
    void onSearchOrFilterChanged();

private:
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
    RoutingController* m_routingController{nullptr};

    QVBoxLayout* m_sessionsLayout{nullptr};
    QVBoxLayout* m_outputsLayout{nullptr};

    QLabel* m_sessionCountLabel{nullptr};
    QLabel* m_endpointCountLabel{nullptr};

    QLineEdit* m_searchBox{nullptr};
    QComboBox* m_filterCombo{nullptr};

    EngineStatePayload m_lastPayload;
    QMap<uint32_t, bool> m_routingInProgress;
    QList<AudioSessionCard> m_sessionCards;
    QHash<uint32_t, QString> m_processNameCache;

    QString resolveProcessNameCached(uint32_t pid);
    static QString resolveProcessName(uint32_t pid);
    static QPixmap resolveProcessIcon(uint32_t pid);
    static QString resolveEndpointFriendlyName(
        const std::wstring& endpointId,
        const std::vector<hydra::windows::AudioRenderEndpoint>& endpoints);
    static QString stateText(hydra::windows::AudioSessionState state);

    void renderSessions();
    void renderOutputs();
    void updateCounts();
    void populateEndpointCombo(QComboBox* combo,
        const std::vector<hydra::windows::AudioRenderEndpoint>& endpoints);
    void buildSessionCard(const hydra::windows::AudioSessionObservation& session,
                          const QString& processName, const QPixmap& icon);
    bool tryUpdateExistingCard(const hydra::windows::AudioSessionObservation& session);

    QString currentSearchText() const;
    int currentFilterIndex() const;
    bool sessionMatchesFilter(const hydra::windows::AudioSessionObservation& session,
                               const QString& searchText, int filterIndex,
                               const QString& resolvedName) const;
};

} // namespace hydra::ui

