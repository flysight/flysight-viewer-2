#include "StatusBarFeature.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QToolButton>

#include "calculationdemand.h"
#include "demandstate.h"
#include "idlescheduler.h"
#include "sessionmodel.h"
#include "ui/docks/AppContext.h"

namespace FlySight {

StatusBarFeature::StatusBarFeature(const AppContext &ctx, QStatusBar *statusBar, QObject *parent)
    : QObject(parent)
    , m_scheduler(&ctx.sessionModel->scheduler())
    , m_demand(ctx.calculationDemand)
{
    QStyle *style = statusBar->style();

    // The activity area: label, bar and cancel button
    auto *activity = new QWidget(statusBar);
    m_activity = activity;
    activity->setObjectName(QStringLiteral("statusActivity"));
    auto *activityLayout = new QHBoxLayout(activity);
    activityLayout->setContentsMargins(0, 0, 0, 0);

    m_activityLabel = new QLabel(activity);
    m_activityLabel->setObjectName(QStringLiteral("statusActivityLabel"));
    activityLayout->addWidget(m_activityLabel);

    m_activityBar = new QProgressBar(activity);
    m_activityBar->setObjectName(QStringLiteral("statusActivityBar"));
    m_activityBar->setTextVisible(false);        // the label carries the count
    m_activityBar->setFixedWidth(120);           // compact, beside its label
    activityLayout->addWidget(m_activityBar);

    m_cancelButton = new QToolButton(activity);
    m_cancelButton->setObjectName(QStringLiteral("statusCancelButton"));
    m_cancelButton->setIcon(style->standardIcon(QStyle::SP_TitleBarCloseButton));
    m_cancelButton->setAutoRaise(true);
    activityLayout->addWidget(m_cancelButton);

    // The warning: the style's warning icon and the count of recordings
    auto *warning = new QWidget(statusBar);
    m_warning = warning;
    warning->setObjectName(QStringLiteral("statusWarning"));
    auto *warningLayout = new QHBoxLayout(warning);
    warningLayout->setContentsMargins(0, 0, 0, 0);

    auto *warningIcon = new QLabel(warning);
    warningIcon->setObjectName(QStringLiteral("statusWarningIcon"));
    const int extent = style->pixelMetric(QStyle::PM_SmallIconSize, nullptr, warningIcon);
    warningIcon->setPixmap(style->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, warningIcon)
                               .pixmap(QSize(extent, extent), warningIcon->devicePixelRatioF()));
    warningLayout->addWidget(warningIcon);

    m_warningText = new QLabel(warning);
    m_warningText->setObjectName(QStringLiteral("statusWarningText"));
    warningLayout->addWidget(m_warningText);

    // The activity container is always shown and keeps the height of the
    // tallest thing the bar can show, so that the bar's height never changes;
    // its widgets and the warning take width only while they are shown.
    // Measured with texts in place: an empty label is shorter than one line.
    m_activityLabel->setText(countText(tr("Computing results"), 100, 100));
    m_warningText->setText(tr("%1 sessions could not be computed").arg(100));
    int tallest = 0;
    for (const QWidget *widget : {static_cast<QWidget *>(m_activityLabel), static_cast<QWidget *>(m_activityBar),
                                  static_cast<QWidget *>(m_cancelButton), static_cast<QWidget *>(warningIcon),
                                  static_cast<QWidget *>(m_warningText)})
        tallest = qMax(tallest, widget->sizeHint().height());
    activity->setMinimumHeight(tallest);
    m_activityLabel->clear();
    m_warningText->clear();
    // Each container is exactly as wide as what it shows: the bar's layout
    // shares its spare width with a widget that can grow, and neither may
    activity->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    warning->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    // The activity at the left, in the bar's normal area, where an
    // application says what it is doing; the warning at the right, as a
    // permanent widget, where state indicators sit. Nothing here shows a
    // temporary message (no action has a status tip), so the normal area is
    // never hidden under one.
    statusBar->addWidget(activity);
    statusBar->addPermanentWidget(warning);

    // Follow the scheduler from now on: it has no query for its active task
    IdleScheduler *scheduler = m_scheduler;
    connect(scheduler, &IdleScheduler::activeTaskChanged, activity, [this](int taskId, bool cancellable) {
        onActiveTaskChanged(taskId, cancellable);
    });
    connect(scheduler, &IdleScheduler::progressChanged, activity, [this](int taskId, int remaining, int total) {
        onTaskProgress(taskId, remaining, total);
    });
    connect(scheduler, &IdleScheduler::schedulerIdle, activity, [this] { onSchedulerIdle(); });
    connect(m_cancelButton, &QToolButton::clicked, activity, [this] { cancelShownTask(); });

    if (CalculationDemand *demand = m_demand) {
        connect(demand, &CalculationDemand::progressChanged, activity, [this] { showActivity(); });
        connect(demand, &CalculationDemand::failuresChanged, warning, [this] { showWarning(); });
        // The pointer is null by the time destroyed() is emitted, so both
        // parts read "no demand layer"
        connect(demand, &QObject::destroyed, activity, [this] {
            showActivity();
            showWarning();
        });
    }

    showActivity();
    showWarning();
}

StatusBarFeature::~StatusBarFeature()
{
    // The widgets are the bar's children; they go with the component that
    // fills them, unless the bar took them already
    delete m_activity.data();
    delete m_warning.data();
}

QString StatusBarFeature::taskLabel(int taskId)
{
    switch (taskId) {
    case SessionModel::SaveTask:
        return tr("Saving sessions");
    case SessionModel::LoadTask:
        return tr("Loading sessions");
    case SessionModel::BulkEditTask:
        return tr("Updating sessions");
    case SessionModel::ColumnTask:
        return tr("Computing columns");
    case SessionModel::ColumnFillTask:
        // The fill's loads serve the computations and are counted by them:
        // while it is the active task, the computations are shown
        return QString();
    }
    return QString();
}

QString StatusBarFeature::countText(const QString &label, int done, int total)
{
    return tr("%1: %2 / %3").arg(label).arg(done).arg(total);
}

void StatusBarFeature::onActiveTaskChanged(int taskId, bool cancellable)
{
    m_taskId = taskId;
    m_taskCancellable = cancellable;
    // Its report follows in the same tick
    m_taskRemaining = 0;
    m_taskTotal = 0;
    showActivity();
}

void StatusBarFeature::onTaskProgress(int taskId, int remaining, int total)
{
    if (taskId != m_taskId)
        return;
    m_taskRemaining = remaining;
    m_taskTotal = total;
    showActivity();
}

void StatusBarFeature::onSchedulerIdle()
{
    m_taskId = -1;
    m_taskCancellable = false;
    m_taskRemaining = 0;
    m_taskTotal = 0;
    showActivity();
}

void StatusBarFeature::cancelShownTask()
{
    // The button shows only while the shown item is the active task
    if (m_scheduler && !taskLabel(m_taskId).isEmpty() && m_taskCancellable)
        m_scheduler->cancel(m_taskId);
}

void StatusBarFeature::showActivity()
{
    if (!m_activity)
        return;

    QStringList hover;
    QString shownText;
    int shownDone = 0;
    int shownTotal = 0;
    bool cancellable = false;

    // The rule: the active task when it is an item, otherwise the computations
    if (const QString label = taskLabel(m_taskId); !label.isEmpty()) {
        shownDone = m_taskTotal - m_taskRemaining;
        shownTotal = m_taskTotal;
        shownText = countText(label, shownDone, shownTotal);
        cancellable = m_taskCancellable;
        hover.append(shownText);
    }

    const DemandProgress progress = m_demand ? m_demand->progress() : DemandProgress();
    if (progress.count > 0) {
        const int done = progress.highWater - progress.count;
        const QString text = countText(tr("Computing results"), done, progress.highWater);
        if (shownText.isEmpty()) {
            shownText = text;
            shownDone = done;
            shownTotal = progress.highWater;
        }
        hover.append(text);
        if (!progress.sessionName.isEmpty()) {
            hover.append(QStringLiteral("  ") + progress.sessionName
                         + (progress.progressText.isEmpty() ? QString()
                                                            : QStringLiteral(": ") + progress.progressText));
        }
    }

    const bool shown = !shownText.isEmpty();
    m_activityLabel->setText(shownText);
    m_activityLabel->setVisible(shown);
    m_activityBar->setRange(0, shownTotal);
    m_activityBar->setValue(shownDone);
    m_activityBar->setVisible(shown);
    m_cancelButton->setVisible(shown && cancellable);

    const QString toolTip = hover.join(QLatin1Char('\n'));
    m_activityLabel->setToolTip(toolTip);
    m_activityBar->setToolTip(toolTip);
}

void StatusBarFeature::showWarning()
{
    if (!m_warning)
        return;

    const QList<SessionFailures> failures = m_demand ? m_demand->failures() : QList<SessionFailures>();
    const int count = int(failures.size());
    if (count == 0)
        m_warningText->clear();
    else if (count == 1)
        m_warningText->setText(tr("1 session could not be computed"));
    else
        m_warningText->setText(tr("%1 sessions could not be computed").arg(count));
    m_warning->setToolTip(SessionFailures::listText(failures));
    m_warning->setVisible(count > 0);
}

} // namespace FlySight
