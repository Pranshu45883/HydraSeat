#pragma once

#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QList>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdint>

#include "ui/engine_poller.hpp"

namespace hydra::ui {

class DiagnosticsPage : public QWidget {
    Q_OBJECT
public:
    explicit DiagnosticsPage(QWidget* parent = nullptr);

public slots:
    void updateState(const EngineStatePayload& payload);

private:
    QVBoxLayout* m_listLayout{nullptr};
    QList<QFrame*> m_seatFrames;

    QFrame* buildSeatDiagnostics(std::uint32_t seatId);
    void updateSeatDiagnostics(
        const hydra::hostipc::SeatSnapshot* snapshot,
        QFrame* frame);
};

} // namespace hydra::ui
