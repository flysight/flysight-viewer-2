#include "LogbookDockFeature.h"
#include "ui/docks/AppContext.h"
#include "idlescheduler.h"
#include "LogbookView.h"
#include "sessionmodel.h"

namespace FlySight {

LogbookDockFeature::LogbookDockFeature(const AppContext& ctx, QObject* parent)
    : DockFeature(parent)
{
    // Create dock widget with unique name for layout persistence
    m_dock = new KDDockWidgets::QtWidgets::DockWidget(QStringLiteral("Logbook"));

    // Create LogbookView; the demand layer supplies the column headers'
    // indicators and the pending cells (null: plain)
    m_logbookView = new LogbookView(ctx.sessionModel, ctx.calculationDemand, m_dock);
    m_dock->setWidget(m_logbookView);

    // Forward LogbookView signals to feature signals
    connect(m_logbookView, &LogbookView::showSelectedRequested,
            this, &LogbookDockFeature::showSelectedRequested);
    connect(m_logbookView, &LogbookView::hideSelectedRequested,
            this, &LogbookDockFeature::hideSelectedRequested);
    connect(m_logbookView, &LogbookView::hideOthersRequested,
            this, &LogbookDockFeature::hideOthersRequested);
    connect(m_logbookView, &LogbookView::deleteRequested,
            this, &LogbookDockFeature::deleteRequested);
    connect(m_logbookView, &LogbookView::focusSessionRequested,
            this, &LogbookDockFeature::focusSessionRequested);

    // Forward focused session changes to the model
    connect(m_logbookView, &LogbookView::currentSessionChanged,
            ctx.sessionModel, &SessionModel::setFocusedSessionId);

    // Connect scheduler signals directly to unified progress slots
    IdleScheduler &sched = ctx.sessionModel->scheduler();

    connect(&sched, &IdleScheduler::activeTaskChanged,
            m_logbookView, &LogbookView::onActiveTaskChanged);
    connect(&sched, &IdleScheduler::progressChanged,
            m_logbookView, &LogbookView::onProgressChanged);
    connect(&sched, &IdleScheduler::schedulerIdle,
            m_logbookView, &LogbookView::onSchedulerIdle);

    // Cancel button connection (delegate through scheduler)
    connect(m_logbookView, &LogbookView::cancelRequested,
            ctx.sessionModel, [model = ctx.sessionModel](int taskId) {
        model->scheduler().cancel(taskId);
    });
}

QString LogbookDockFeature::id() const
{
    return QStringLiteral("Logbook");
}

QString LogbookDockFeature::title() const
{
    return QStringLiteral("Logbook");
}

KDDockWidgets::QtWidgets::DockWidget* LogbookDockFeature::dock() const
{
    return m_dock;
}

KDDockWidgets::Location LogbookDockFeature::defaultLocation() const
{
    return KDDockWidgets::Location_OnRight;
}

} // namespace FlySight
