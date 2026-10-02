#ifndef SETGROUNDTOOL_H
#define SETGROUNDTOOL_H

#include "plottool.h"
#include "ui/docks/plot/PlotWidget.h"
#include "../graphinfo.h"

namespace FlySight {

/*!
 * \brief SetGroundTool: Now it only updates the session’s GroundElev
 * when the user clicks, and doesn't manage tracers at all.
 */
class SetGroundTool : public PlotTool {
public:
    SetGroundTool(const PlotWidget::PlotContext &ctx);

    bool mousePressEvent(QMouseEvent *event) override;
    bool mouseMoveEvent(QMouseEvent *event) override;
    void activateTool() override;
    void closeTool() override;

    bool isPrimary() override { return false; }

private:
    PlotWidget* m_widget;
    QCustomPlot* m_plot;
    SessionModel* m_model;
};

} // namespace FlySight

#endif // SETGROUNDTOOL_H
