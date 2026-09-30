#pragma once
#include <QWidget>
#include <memory>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QMap>

#include "hydra/runtime_authority.hpp"
#include "ui/engine_poller.hpp"
#include "ui/routing_controller.hpp"

namespace hydra::ui {

struct AudioSessionCard {
    QFrame* frame{nullptr};
    QLabel* nameLabel{nullptr};
    QLabel* stateLabel{nullptr};
    QLabel* pidLabel{nullptr};
    QLabel* currentOutputLabel{nullptr};
    QComboBox* routeCombo{nullptr};
    QPushButton* routeBtn{nullptr};
    QPushButton* resetBtn{nullptr};
    QLabel* feedbackLabel{nullptr};
    
    uint32_t pid{0};
    uint64_t creationIdentity{0};
    std::wstring currentEndpointId;
};

class AudioPage : public QWidget {
    Q_OBJECT
public:
    explicit AudioPage(std::shared_ptr<hydra::runtime::SessionController> sessionController, RoutingController* router, QWidget* parent = nullptr);
public slots:
    void updateState(const EngineStatePayload& payload);
private slots:
    void onRoutingCompleted(uint32_t pid, RouteVerificationResult result, const QString& errorMessage);
    void onResetCompleted(uint32_t pid, bool success, const QString& errorMessage);
private:
    std::shared_ptr<hydra::runtime::SessionController> m_sessionController;
    RoutingController* m_router{nullptr};
    
    QVBoxLayout* m_sessionsLayout{nullptr};
    QVBoxLayout* m_outputsLayout{nullptr};
    
    QLabel* m_sessionsCountLabel{nullptr};
    QLabel* m_outputsCountLabel{nullptr};
    
    QLineEdit* m_searchBox{nullptr};
    QComboBox* m_filterCombo{nullptr};
    
    EngineStatePayload m_lastPayload;
    
    QList<AudioSessionCard> m_sessionCards;
    
    static QString stateText(hydra::windows::AudioSessionState state);
    static QString stateColor(hydra::windows::AudioSessionState state);
    
    QString resolveEndpointFriendlyName(const std::wstring& endpointId) const;
    void buildSessionCard(const hydra::windows::AudioSessionObservation& session);
    bool tryUpdateExistingCard(const hydra::windows::AudioSessionObservation& session);
    bool sessionMatchesFilter(const hydra::windows::AudioSessionObservation& session, const QString& filterText, int filterType) const;
};

} // namespace hydra::ui
