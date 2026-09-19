#ifndef MEASURETOOL_H
#define MEASURETOOL_H

#include "plottool.h"
#include "ui/docks/plot/PlotWidget.h"
#include <QCustomPlot/qcustomplot.h>

namespace FlySight {

class MeasureModel;
class PlotModel;
struct PlotValue;

class MeasureTool : public PlotTool
{
public:
    explicit MeasureTool(const PlotWidget::PlotContext &ctx);

    bool mousePressEvent(QMouseEvent *event) override;
    bool mouseMoveEvent(QMouseEvent *event) override;
    bool mouseReleaseEvent(QMouseEvent *event) override;
    bool leaveEvent(QEvent *event) override;

    void closeTool() override;

    bool isPrimary() override { return true; }

private:
    void updateMeasurement(const QPoint &currentPixel);

    struct Measurement;
    /// Computes the measurement from the model's live sessions. The caller
    /// holds a SessionModel::RowStabilityGuard for the whole call.
    Measurement measure(double currentX,
                        const QString &xVariable,
                        const QString &referenceMarkerKey,
                        const QVector<PlotValue> &enabledPlots) const;

    void applyLinePenFromPreferences();

    PlotWidget   *m_widget;
    QCustomPlot  *m_plot;
    QMap<QCPGraph*, GraphInfo> *m_graphMap;
    SessionModel *m_model;
    PlotModel    *m_plotModel;
    MeasureModel *m_measureModel;

    QCPItemRect  *m_rect;
    QCPItemLine  *m_lineLeft;
    QCPItemLine  *m_lineRight;
    bool          m_measuring = false;
    bool          m_multiTrack = false;
    QPoint        m_startPixel;
    double        m_startX = 0.0;
    QString       m_lockedSessionId;
};

} // namespace FlySight

#endif // MEASURETOOL_H
