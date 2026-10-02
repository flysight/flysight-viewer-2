#include "setgroundtool.h"
#include "ui/docks/plot/PlotWidget.h"
#include "../crosshairmanager.h"
#include "../sessiondata.h"
#include "../plotutils.h"
#include <QCustomPlot/qcustomplot.h>

#include <QMouseEvent>

#include <cmath>

namespace FlySight {

SetGroundTool::SetGroundTool(const PlotWidget::PlotContext &ctx)
    : m_widget(ctx.widget)
    , m_plot(ctx.plot)
    , m_model(ctx.model)
{
}

bool SetGroundTool::mousePressEvent(QMouseEvent *event)
{
    // only left-button clicks, and only if plot+model exist
    if (!m_plot || !m_model || event->button() != Qt::LeftButton)
        return false;

    // 1) pixel → data coordinate
    double xCoord = m_plot->xAxis->pixelToCoord(event->pos().x());

    // 2) Get traced sessions from CrosshairManager via PlotWidget
    CrosshairManager* crosshairMgr = m_widget->crosshairManager();
    if (!crosshairMgr) return false; // Safety check

    QSet<QString> tracedIds = crosshairMgr->getTracedSessionIds();

    for (const QString& sessionId : tracedIds) {
        int row = m_model->getSessionRow(sessionId);
        if (row >= 0) {
            SessionData &session = m_model->sessionRef(row);
            // A point read like the measure tool's (plotutils.h): no value
            // at the clicked time outside the samples or strictly inside a
            // hole of the GNSS samples, and then the click sets nothing
            const double newElev = groundElevationAt(session, m_widget->xVariable(),
                                                     m_widget->referenceMarkerKey(), xCoord);
            if (!std::isnan(newElev))
                m_model->updateAttribute(sessionId, SessionKeys::GroundElev, newElev);
        }
    }

    // 3) clean up
    m_widget->revertToPrimaryTool();
    return true;
}

bool SetGroundTool::mouseMoveEvent(QMouseEvent *event)
{
    // We can do nothing here, because the CrosshairManager is already
    // showing single or multi tracers.
    // Or we can do extra stuff if you want.
    Q_UNUSED(event);
    return true;
}

void SetGroundTool::activateTool()
{
    PlotTool::activateTool();
    // We might do a synthetic mouse move if desired,
    // or just do nothing
}

void SetGroundTool::closeTool()
{
    // We do nothing, because the manager handles tracer removal
    PlotTool::closeTool();
}

} // namespace FlySight
