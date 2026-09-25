// The plot list's row delegate (PlotRowDelegate) in an offscreen QTreeView, on
// a real CalculationDemand, PlotModel, executor (JobQueue) and SessionModel,
// with the synthetic plots of plotfixture.h, driven by synthesized mouse and
// key events. This is the only test that links Qt Widgets
// (FLYSIGHT_BUILD_WIDGET_TESTS).
// Sensor-fusion-jobs spec 9.2-9.4 (the view half); acceptance 116 (wiring
// half): the view adds no path of its own from the user to work. Checking a
// row is the base class's write to PlotModel, which the demand layer observes
// like every other check change; nothing in the row is clickable beyond what
// QStyledItemDelegate makes clickable.
//
// What is proved here is what the view owns: plain rows are the base
// delegate's, the working indicator and the warning badge are painted, a
// click anywhere is the base delegate's click, the tooltip, the repaint.
// State, counts and what is computed are CalculationDemand's
// (tst_calculation_demand).
//
// Synchronization: Gate::waitEntered() proves the worker is inside a compute
// function; QTRY_*, waitIdle() and waitDemandIdle() spin the event loop;
// CalculationDemand::flush() runs a pending pass before a row state is read or
// an image is grabbed. There are no sleeps.

#include <functional>
#include <memory>

#include <QApplication>
#include <QHashFunctions>
#include <QHelpEvent>
#include <QImage>
#include <QItemSelectionModel>
#include <QSettings>
#include <QSignalSpy>
#include <QStyleFactory>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QTreeView>
#include <QtTest>

#include "calculationdemand.h"
#include "engine/calculationregistry.h"
#include "jobfixture.h"
#include "jobmodel.h"
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "logbookprobe.h"
#include "plotfixture.h"
#include "plotmodel.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testutil.h"
#include "ui/docks/plotselection/PlotRowDelegate.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

/// Counts the paint events of a widget.
class PaintCounter : public QObject {
public:
    explicit PaintCounter(QWidget *watched) : QObject(watched) { watched->installEventFilter(this); }
    int count = 0;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Paint)
            ++count;
        return QObject::eventFilter(watched, event);
    }
};

/// A rect of viewport coordinates, cut from an image grabbed from the viewport.
QImage cut(const QImage &image, const QRect &rect)
{
    const qreal ratio = image.devicePixelRatio();
    return image.copy(QRect(qRound(rect.x() * ratio), qRound(rect.y() * ratio),
                            qRound(rect.width() * ratio), qRound(rect.height() * ratio)));
}

/// The kinds of click compared with the base delegate's.
enum class Click { Left, Right, Middle, Double };

/// Where a click left the view.
struct ClickOutcome {
    QModelIndex current;
    QModelIndexList selected;
};

} // namespace

class PlotRowDelegateTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void plainRowsAreIdenticalToBaseDelegate();
    void workingRowPaintsIndicator();
    void longNameIsElidedNotTheCluster();
    void checkBoxClickChecksThroughTheModel();
    void spaceKeyChecksThroughTheModel();
    void uncheckByClickDropsWaitingWork();
    void programmaticCheckIsTheSameAsAClick();
    void startupRestoreWithHiddenSessionsStartsNothingWithViewAttached();
    void clickOnClusterIsAClickOnTheRow();
    void toolTipComesFromPlotState();
    void plotStateChangeRepaintsRow();
    void survivesDemandDestroyedFirst();

private:
    Gate &gate() { return m_world->gate(); }

    /// PlotModel, CalculationDemand, the view and the delegate, as the
    /// application builds them: the component before the view. `plots` go into
    /// the model at once unless `plotsLater`. False when the view was never
    /// exposed (the rest is then not built); check it with QVERIFY in the test
    /// function.
    [[nodiscard]] bool buildUi(const QVector<PlotValue> &plots, QSettings *settings = nullptr, bool plotsLater = false);
    void destroyUi();

    /// A programmatic check, not through the view: PlotModel::setPlotEnabled(),
    /// as applyProfile() and the Plots menu's model calls do.
    void check(const char *measurement, bool enabled = true)
    {
        m_plots->setPlotEnabled(QStringLiteral("Syn"), QString::fromLatin1(measurement), enabled);
    }
    bool isChecked(const QModelIndex &index) const { return index.data(Qt::CheckStateRole).toInt() == Qt::Checked; }
    /// The current plot state: a pending pass runs first.
    DemandState row(const char *plotId)
    {
        m_demand->flush();
        return m_demand->plotState(QString::fromLatin1(plotId));
    }
    /// Two turns of the event loop and a flush: whatever was going to start by
    /// itself has started, and whatever was going to be painted is painted.
    void spin()
    {
        PlotFixture::spin(m_demand.get());
        QApplication::processEvents();
    }
    /// "<session> <calculation> <state>" of every job from row `from` of the
    /// job model on, in offer order.
    QStringList jobsFrom(int from) const
    {
        QStringList jobs;
        const JobModel *model = m_queue->model();
        for (int r = from; r < model->rowCount(); ++r) {
            const JobRecord record = model->record(r);
            jobs.append(record.sessionId + QLatin1Char(' ') + record.calculationId + QLatin1Char(' ')
                        + JobModel::stateText(record.state));
        }
        return jobs;
    }

    /// The same lookup the delegate uses.
    QModelIndex indexOf(const char *plotId) const
    {
        const QModelIndexList found = m_plots->match(m_plots->index(0, 0), PlotModel::PlotValueIdRole,
                                                     QString::fromLatin1(plotId), 1,
                                                     Qt::MatchExactly | Qt::MatchRecursive);
        return found.isEmpty() ? QModelIndex() : found.first();
    }
    /// An option as the view hands it to the delegate, as far as layout goes.
    QStyleOptionViewItem optionFor(const QModelIndex &index) const
    {
        QStyleOptionViewItem opt;
        opt.initFrom(m_view.get());
        opt.widget = m_view.get();
        opt.rect = m_view->visualRect(index);
        opt.index = index;
        opt.features = QStyleOptionViewItem::HasCheckIndicator | QStyleOptionViewItem::HasDisplay;
        opt.text = index.data(Qt::DisplayRole).toString();
        opt.checkState = static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
        return opt;
    }
    QRect checkBoxRect(const QModelIndex &index) const
    {
        const QStyleOptionViewItem opt = optionFor(index);
        return m_view->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, m_view.get());
    }
    QPoint checkBoxCentre(const QModelIndex &index) const { return checkBoxRect(index).center(); }
    /// A point of the row on the plot's name: not the check box, not the cluster.
    QPoint namePoint(const QModelIndex &index) const
    {
        const QRect box = checkBoxRect(index);
        return QPoint(box.right() + 8, box.center().y());
    }

    void click(const QPoint &point, Qt::MouseButton button = Qt::LeftButton)
    {
        QTest::mouseClick(m_view->viewport(), button, {}, point);
    }
    void pressSpaceOn(const QModelIndex &index)
    {
        m_view->setCurrentIndex(index);
        QTest::keyClick(m_view.get(), Qt::Key_Space);
    }
    /// With `delegate` installed: make `start` the current and only selected
    /// row, click at `point`, and report where that left the view. The
    /// delegate under test is installed again afterwards.
    ClickOutcome clickWith(QAbstractItemDelegate *delegate, const QModelIndex &start, const QPoint &point, Click kind)
    {
        m_view->setItemDelegate(delegate);
        m_view->selectionModel()->setCurrentIndex(start, QItemSelectionModel::ClearAndSelect);
        switch (kind) {
        case Click::Left:
            click(point);
            break;
        case Click::Right:
            click(point, Qt::RightButton);
            break;
        case Click::Middle:
            click(point, Qt::MiddleButton);
            break;
        case Click::Double:
            // What a widget receives for a double click: press, release, double
            // click, release. (QTest::mouseDClick() on a widget sends the third only.)
            QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, point);
            QTest::mouseDClick(m_view->viewport(), Qt::LeftButton, {}, point);
            QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, point);
            break;
        }
        const ClickOutcome outcome{m_view->currentIndex(), m_view->selectionModel()->selectedIndexes()};
        m_view->setItemDelegate(m_delegate);
        return outcome;
    }

    /// The viewport as painted now, with the delegate under test ...
    QImage grabViewport()
    {
        spin();
        return m_view->viewport()->grab().toImage();
    }
    /// ... and the same view, same size, expansion, selection and focus, with
    /// a default delegate.
    QImage grabViewportWithBaseDelegate()
    {
        QStyledItemDelegate base;
        QAbstractItemDelegate *under = m_view->itemDelegate();
        m_view->setItemDelegate(&base);
        QApplication::processEvents();
        const QImage image = m_view->viewport()->grab().toImage();
        m_view->setItemDelegate(under);
        QApplication::processEvents();
        return image;
    }

    /// Syn/g checked from code with s1 and s2 visible: the demand layer starts
    /// s1 (held in the gate, its progress text delivered) and chooses s2 next.
    /// The row is working, "0 of 2". Invalid on any other outcome.
    QModelIndex makeWorkingRow()
    {
        check("g");
        if (!gate().waitEntered())
            return QModelIndex();
        // The progress text arrives after the entry: wait for it, so that no
        // later change of the state is left pending
        if (!QTest::qWaitFor([this] {
                return row("Syn/g").running.value(0).progressText == QStringLiteral("step 1");
            }))
            return QModelIndex();
        const DemandState state = row("Syn/g");
        if (!state.isWorking() || state.progressLabel != QStringLiteral("0 of 2")
            || sessionIdsOf(state.running) != QStringList({"s1"})
            || sessionIdsOf(state.waiting) != QStringList({"s2"}))
            return QModelIndex();
        spin();
        return indexOf("Syn/g");
    }

    std::unique_ptr<JobWorld> m_world;
    std::unique_ptr<PlotFixture> m_fixture;
    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    std::unique_ptr<PlotModel> m_plots;
    std::unique_ptr<CalculationDemand> m_demand;
    std::unique_ptr<QTreeView> m_view;
    PlotRowDelegate *m_delegate = nullptr;      // a child of the view
    QStringList m_registryBefore;
};

void PlotRowDelegateTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();

    // One logbook column that reads stored data only (see tst_jobqueue)
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({descriptionColumn()});
}

// Three loaded sessions "s1".."s3" named "Jump 1".."Jump 3", each with
// G_IN = 4; s1 and s2 are visible. No plot is checked. The view is shown.
void PlotRowDelegateTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_world = std::make_unique<JobWorld>();
    m_fixture = std::make_unique<PlotFixture>();
    m_model = std::make_unique<SessionModel>();
    m_model->mergeSessions(JobWorld::sessions({"s1", "s2", "s3"}));
    QCOMPARE(m_model->rowCount(), 3);
    for (int i = 1; i <= 3; ++i) {
        const QString id = QStringLiteral("s%1").arg(i);
        m_model->updateAttribute(id, QString::fromLatin1(SessionKeys::Description), QStringLiteral("Jump %1").arg(i));
        QVERIFY(PlotFixture::giveInput(*m_model, id, QStringLiteral("G_IN"), 4));
    }
    m_model->flushPendingInvalidations();
    PlotFixture::show(*m_model, {"s1", "s2"});

    m_queue = std::make_unique<JobQueue>(m_model.get());
    QVERIFY(buildUi(PlotFixture::plots()));
}

bool PlotRowDelegateTest::buildUi(const QVector<PlotValue> &plots, QSettings *settings, bool plotsLater)
{
    m_plots = std::make_unique<PlotModel>();
    m_plots->setSettings(settings);
    if (!plotsLater)
        m_plots->setPlots(plots);
    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());

    // As PlotSelectionDockFeature configures its view
    m_view = std::make_unique<QTreeView>();
    m_view->setModel(m_plots.get());
    m_view->setHeaderHidden(true);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->resize(300, 300);
    m_delegate = new PlotRowDelegate(m_demand.get(), m_view.get());
    m_view->setItemDelegate(m_delegate);
    m_view->expandAll();
    m_view->show();
    if (!QTest::qWaitForWindowExposed(m_view.get()))
        return false;

    if (plotsLater) {
        m_plots->setPlots(plots);
        m_view->expandAll();
    }
    spin();
    return true;
}

void PlotRowDelegateTest::destroyUi()
{
    QToolTip::hideText();
    m_delegate = nullptr;
    m_view.reset();
    m_demand.reset();
    m_plots.reset();
}

// Note what is to be checked, tear everything down, and only then check: a
// failing check returns from cleanup(), and whatever were still alive then
// would be alive under the next init() (see tst_jobqueue).
void PlotRowDelegateTest::cleanup()
{
    // Release whatever a test left in the gate, then let nothing linger
    // inside a compute function: whatever the demand layer still wanted runs
    // to its end first
    if (m_world)
        gate().open(8);
    bool becameIdle = true;
    if (m_queue) {
        becameIdle = m_demand ? waitDemandIdle(*m_queue, *m_demand) : waitIdle(*m_queue);
        m_queue->shutdown();
    }
    QStringList stillPinned;
    if (m_model) {
        for (const char *id : {"s1", "s2", "s3"}) {
            if (m_model->isSessionPinned(id))
                stillPinned.append(QString::fromLatin1(id));
        }
    }

    destroyUi();
    m_queue.reset();
    m_model.reset();

    const QStringList withoutFixture = [this] {
        QStringList ids = CalculationRegistry::instance().registeredIds();
        for (const QString &id : m_fixture ? m_fixture->registeredIds() : QStringList())
            ids.removeAll(id);
        return ids;
    }();
    m_fixture.reset();
    const QStringList afterFixture = CalculationRegistry::instance().registeredIds();
    m_world.reset();

    QVERIFY(becameIdle);
    QCOMPARE(stillPinned, QStringList());
    // The fixture removed exactly what it added
    QCOMPARE(afterFixture, withoutFixture);
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// ---- Painting -------------------------------------------------------------------------

// Spec 9.2, last bullet: a row with nothing waiting, running or failed, every
// row of a plot that is not over a requested calculation, and every unchecked
// row look exactly as they do today - pixel for pixel.
void PlotRowDelegateTest::plainRowsAreIdenticalToBaseDelegate()
{
    // Nothing checked
    QImage with = m_view->grab().toImage();
    QVERIFY(!with.isNull());
    {
        QStyledItemDelegate base;
        m_view->setItemDelegate(&base);
        QApplication::processEvents();
        QCOMPARE(m_view->grab().toImage(), with);
        m_view->setItemDelegate(m_delegate);
        QApplication::processEvents();
    }
    QCOMPARE(grabViewport(), grabViewportWithBaseDelegate());

    // An ordinary plot, checked, with visible tracks
    check("plain");
    QVERIFY(row("Syn/plain").isPlain());
    with = grabViewport();
    QCOMPARE(with, grabViewportWithBaseDelegate());
    QCOMPARE(m_delegate->clusterRect(indexOf("Syn/plain")), QRect());

    // Without a component every row is plain, whatever is checked. (Checking
    // Syn/g starts s1 in the gate; cleanup() opens it.)
    check("g");
    QVERIFY(!row("Syn/g").isPlain());
    PlotRowDelegate inert(nullptr, m_view.get());
    m_view->setItemDelegate(&inert);
    QApplication::processEvents();
    const QImage withInert = m_view->viewport()->grab().toImage();
    m_view->setItemDelegate(m_delegate);
    QCOMPARE(withInert, grabViewportWithBaseDelegate());
    QCOMPARE(inert.clusterRect(indexOf("Syn/g")), QRect());
    QVERIFY(inert.toolTipFor(indexOf("Syn/g")).isEmpty());
}

void PlotRowDelegateTest::workingRowPaintsIndicator()
{
    const int plainHeight = m_view->visualRect(indexOf("Syn/g")).height();
    const QModelIndex index = makeWorkingRow();
    QVERIFY(index.isValid());

    // The cluster sits inside the row, right-aligned, clear of the check box
    const QRect cluster = m_delegate->clusterRect(index);
    QVERIFY(!cluster.isNull());
    const QRect rowRect = m_view->visualRect(index);
    QVERIFY(rowRect.contains(cluster));
    QVERIFY(cluster.left() > checkBoxRect(index).right());
    QVERIFY(cluster.center().x() > rowRect.center().x());

    const QImage with = grabViewport();
    const QImage base = grabViewportWithBaseDelegate();
    QVERIFY(cut(with, cluster) != cut(base, cluster));                              // label and indicator
    // The indicator is the right-most element: the arc's right-hand side
    const int side = qMax(2, cluster.height() / 4);
    const QRect indicatorEdge(cluster.right() - side + 1, cluster.top(), side, cluster.height());
    QVERIFY(cut(with, indicatorEdge) != cut(base, indicatorEdge));
    QCOMPARE(cut(with, checkBoxRect(index)), cut(base, checkBoxRect(index)));       // the check box is the style's
    // Every other row is untouched
    const QRect other = m_view->visualRect(indexOf("Syn/g2"));
    QCOMPARE(cut(with, other), cut(base, other));

    // The row height does not depend on the state
    QCOMPARE(rowRect.height(), plainHeight);
    QCOMPARE(m_view->visualRect(indexOf("Syn/plain")).height(), plainHeight);
}

// Two rows in the same state, one with a name that does not fit: the name
// gives way, and the cluster is painted exactly as on the row with room.
void PlotRowDelegateTest::longNameIsElidedNotTheCluster()
{
    QVector<PlotValue> plots = PlotFixture::plots();
    plots[0].plotName = QStringLiteral("A plot with a name that certainly does not fit in this narrow view");
    destroyUi();
    QVERIFY(buildUi(plots));
    // Room for the check box, the cluster ("0 of 2" and the indicator) and a
    // little of the name, which is far longer
    m_view->resize(220, 300);
    spin();

    // Both rows over the same jobs: s1 running, s2 chosen next
    check("g");
    check("g2");
    QVERIFY(gate().waitEntered());
    const DemandState g = row("Syn/g");
    QCOMPARE(g.progressLabel, QStringLiteral("0 of 2"));
    QCOMPARE(row("Syn/g2").progressLabel, g.progressLabel);
    QCOMPARE(row("Syn/g2").wantedCount, 2);
    const QModelIndex longRow = indexOf("Syn/g");
    const QModelIndex shortRow = indexOf("Syn/g2");

    // The cluster is fully visible
    const QRect viewport = m_view->viewport()->rect();
    const QRect cluster = m_delegate->clusterRect(longRow);
    QVERIFY(!cluster.isNull());
    QVERIFY(viewport.contains(cluster));
    QVERIFY(cluster.left() > checkBoxRect(longRow).right());
    QCOMPARE(m_view->visualRect(longRow).right(), viewport.right());

    // The band over the label and the indicator of each row (same x, other y)
    const auto clusterBand = [&](const QModelIndex &index) { return m_delegate->clusterRect(index); };
    QCOMPARE(clusterBand(longRow).left(), clusterBand(shortRow).left());
    QCOMPARE(clusterBand(longRow).width(), clusterBand(shortRow).width());

    const QImage with = grabViewport();
    const QImage base = grabViewportWithBaseDelegate();
    // The base delegate runs the long name through the band ...
    QVERIFY(cut(base, clusterBand(longRow)) != cut(base, clusterBand(shortRow)));
    // ... this one keeps it clear: both clusters are the same picture
    QCOMPARE(cut(with, clusterBand(longRow)), cut(with, clusterBand(shortRow)));
    QVERIFY(cut(with, clusterBand(longRow)) != cut(base, clusterBand(longRow)));
    // The check box is where it was, and what is left of the name is still
    // painted between the two
    QCOMPARE(cut(with, checkBoxRect(longRow)), cut(base, checkBoxRect(longRow)));
    QRect name = m_view->visualRect(longRow);
    name.setLeft(checkBoxRect(longRow).right() + 1);
    name.setRight(clusterBand(longRow).left() - 1);
    QVERIFY(name.width() > 0);
    const QImage nameImage = cut(with, name);
    bool painted = false;
    for (int y = 0; y < nameImage.height() && !painted; ++y) {
        for (int x = 0; x < nameImage.width() && !painted; ++x)
            painted = nameImage.pixel(x, y) != nameImage.pixel(0, 0);
    }
    QVERIFY(painted);
}

// ---- Checking goes through the model ------------------------------------------------------

// Acceptance 116: a click on the check box is the base class's write to
// PlotModel; the demand layer, observing the model, starts the work. A
// programmatic check of another row has the same effect: the delegate adds no
// path of its own.
void PlotRowDelegateTest::checkBoxClickChecksThroughTheModel()
{
    // Explicit A's input for the second half. No plot is checked yet, so the
    // edit is nobody's input and starts no settle wait.
    for (const char *id : {"s1", "s2"})
        QVERIFY(PlotFixture::giveInput(*m_model, QString::fromLatin1(id), QStringLiteral("EA_IN"), 4));
    m_model->flushPendingInvalidations();
    QVERIFY(!m_demand->hasSettlingSessions());

    const QModelIndex index = indexOf("Syn/g");
    QVERIFY(!isChecked(index));
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);

    click(checkBoxCentre(index));
    QVERIFY(isChecked(index));
    QCOMPARE(m_plots->data(index, Qt::CheckStateRole).toInt(), int(Qt::Checked));

    // s1 runs and s2 is chosen next, with no other call; s3 is hidden
    QVERIFY(gate().waitEntered());
    const DemandState state = row("Syn/g");
    QVERIFY(state.isWorking());
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 2"));
    QCOMPARE(sessionIdsOf(state.running), QStringList({"s1"}));
    QCOMPARE(sessionIdsOf(state.waiting), QStringList({"s2"}));
    QCOMPARE(queuedSpy.count(), 2);

    gate().open();
    QVERIFY(gate().waitEntered());
    gate().open();
    QVERIFY(waitDemandIdle(*m_queue, *m_demand));
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(queuedSpy.count(), 2);
    QCOMPARE(jobsFrom(0), QStringList({"s1 gated Succeeded", "s2 gated Succeeded"}));

    // A programmatic check of another row: the same sessions, started the same way
    const int from = m_queue->model()->rowCount();
    check("ea");
    QVERIFY(waitDemandIdle(*m_queue, *m_demand));
    QCOMPARE(jobsFrom(from), QStringList({"s1 expA Succeeded", "s2 expA Succeeded"}));
    QVERIFY(row("Syn/ea").isPlain());
    QCOMPARE(row("Syn/ea").wantedCount, 2);
}

void PlotRowDelegateTest::spaceKeyChecksThroughTheModel()
{
    const QModelIndex index = indexOf("Syn/g");
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);

    pressSpaceOn(index);
    QVERIFY(isChecked(index));

    QVERIFY(gate().waitEntered());
    const DemandState state = row("Syn/g");
    QVERIFY(state.isWorking());
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 2"));
    QCOMPARE(queuedSpy.count(), 2);

    gate().open();
    QVERIFY(gate().waitEntered());
    gate().open();
    QVERIFY(waitDemandIdle(*m_queue, *m_demand));
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(jobsFrom(0), QStringList({"s1 gated Succeeded", "s2 gated Succeeded"}));
}

// Unchecking by the check box (or Space) drops the waiting pair at once - before
// any turn of the event loop - and lets the running job finish.
void PlotRowDelegateTest::uncheckByClickDropsWaitingWork()
{
    const QModelIndex index = makeWorkingRow();
    QVERIFY(index.isValid());
    const JobId running = m_queue->runningJob();
    const JobId waiting = m_queue->chosenNextJob();
    QCOMPARE(m_queue->job(running).sessionId, QStringLiteral("s1"));
    QCOMPARE(m_queue->job(waiting).sessionId, QStringLiteral("s2"));
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);

    click(checkBoxCentre(index));
    QVERIFY(!isChecked(index));
    JobRecord dropped = m_queue->job(waiting);
    QCOMPARE(dropped.state, JobState::Cancelled);
    QCOMPARE(dropped.reason, QStringLiteral("No longer needed"));
    QVERIFY(!dropped.startedAt.isValid());
    QCOMPARE(m_queue->chosenNextJob(), JobId(0));
    QCOMPARE(m_queue->job(running).state, JobState::Running);
    QVERIFY(!m_queue->job(running).cancelRequested);
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(m_delegate->clusterRect(index), QRect());

    // Checked again, s2 is chosen next again; Space drops it the same way
    check("g");
    spin();
    const JobId again = m_queue->chosenNextJob();
    QVERIFY(again != 0);
    QVERIFY(again != waiting);
    QCOMPARE(m_queue->job(again).sessionId, QStringLiteral("s2"));
    pressSpaceOn(index);
    QVERIFY(!isChecked(index));
    dropped = m_queue->job(again);
    QCOMPARE(dropped.state, JobState::Cancelled);
    QCOMPARE(dropped.reason, QStringLiteral("No longer needed"));
    QVERIFY(!dropped.startedAt.isValid());
    QVERIFY(row("Syn/g").isPlain());

    // The running job was never asked to stop: it finishes, and nothing follows
    const Quiet quiet(*m_queue);
    gate().open();
    QVERIFY(waitDemandIdle(*m_queue, *m_demand));
    QCOMPARE(m_queue->job(running).state, JobState::Succeeded);
    QCOMPARE(cancelSpy.count(), 0);
    QVERIFY(quiet.holds());
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(jobsFrom(0), QStringList({"s1 gated Succeeded", "s2 gated Cancelled", "s2 gated Cancelled"}));
}

// Acceptance 116, with the view and the delegate attached: every way of
// checking a plot from code creates exactly the demand a click creates.
void PlotRowDelegateTest::programmaticCheckIsTheSameAsAClick()
{
    const QModelIndex index = indexOf("Syn/g");
    struct Path {
        const char *name;
        std::function<void(bool)> set;
    };
    const QList<Path> paths = {
        // The check box: the base class's write to the model
        {"click", [&](bool on) {
            if (isChecked(index) != on)
                click(checkBoxCentre(index));
        }},
        // setPlotEnabled: applyProfile() and restored settings
        {"setPlotEnabled", [&](bool on) {
            m_plots->setPlotEnabled(QStringLiteral("Syn"), QStringLiteral("g"), on);
        }},
        // togglePlot: the Plots menu and its shortcuts
        {"togglePlot", [&](bool on) {
            if (isChecked(index) != on)
                m_plots->togglePlot(QStringLiteral("Syn"), QStringLiteral("g"));
        }},
        // setData(CheckStateRole): what the check box writes, without the view
        {"setData", [&](bool on) {
            m_plots->setData(index, on ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        }},
    };

    QList<int> expectedOrder;
    for (int round = 0; round < paths.size(); ++round) {
        const Path &path = paths.at(round);
        const int from = m_queue->model()->rowCount();
        expectedOrder << (round == 0 ? 4 : 10 * round + 1) << (round == 0 ? 4 : 10 * round + 2);

        path.set(true);
        QVERIFY2(isChecked(index), path.name);
        QVERIFY2(gate().waitEntered(), path.name);                      // s1
        const DemandState state = row("Syn/g");
        QVERIFY2(state.isWorking(), path.name);
        QCOMPARE(state.progressLabel, QStringLiteral("0 of 2"));
        QVERIFY2(!m_delegate->clusterRect(index).isNull(), path.name);
        gate().open();
        QVERIFY2(gate().waitEntered(), path.name);                      // s2
        gate().open();
        QVERIFY2(waitDemandIdle(*m_queue, *m_demand), path.name);
        QVERIFY2(row("Syn/g").isPlain(), path.name);
        QCOMPARE(jobsFrom(from), QStringList({"s1 gated Succeeded", "s2 gated Succeeded"}));

        path.set(false);
        QVERIFY2(!isChecked(index), path.name);
        // New inputs, so that the next path finds both results missing again.
        // The plot is unchecked: the edits are nobody's input, no wait starts.
        for (int i = 1; i <= 2; ++i) {
            QVERIFY(PlotFixture::giveInput(*m_model, QStringLiteral("s%1").arg(i), QStringLiteral("G_IN"),
                                           10 * (round + 1) + i));
        }
        m_model->flushPendingInvalidations();
        QVERIFY2(!m_demand->hasSettlingSessions(), path.name);
        spin();
        QVERIFY2(m_queue->isIdle(), path.name);
    }
    // Each round computed that round's inputs
    QCOMPARE(gate().startOrder(), expectedOrder);
}

// Plots restored as checked from the settings come up through modelReset,
// after the view and the delegate exist - as in MainWindow's constructor -
// while every session is hidden: nothing is started and the row is plain.
// Showing a session is what starts its work.
void PlotRowDelegateTest::startupRestoreWithHiddenSessionsStartsNothingWithViewAttached()
{
    PlotFixture::show(*m_model, {"s1", "s2"}, false);
    const Quiet quiet(*m_queue);

    const QString path = TestEnvironment::instance().newTempDir(QStringLiteral("plots")) + QStringLiteral("/plots.ini");
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("state/plots/Syn/g"), true);
    // Whichever way this function is left, no PlotModel keeps pointing at `settings`
    const auto detachSettings = qScopeGuard([this] {
        if (m_plots)
            m_plots->setSettings(nullptr);
    });

    destroyUi();
    QVERIFY(buildUi(PlotFixture::plots(), &settings, /*plotsLater=*/true));

    const QModelIndex index = indexOf("Syn/g");
    QVERIFY(index.isValid());
    QVERIFY(isChecked(index));
    const DemandState state = row("Syn/g");
    QVERIFY(state.isPlain());
    QCOMPARE(state.wantedCount, 0);
    QCOMPARE(m_delegate->clusterRect(index), QRect());
    spin();
    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());
    QCOMPARE(grabViewport(), grabViewportWithBaseDelegate());

    // Showing s1 starts it with no other call
    PlotFixture::show(*m_model, {"s1"});
    QVERIFY(gate().waitEntered());
    const DemandState working = row("Syn/g");
    QVERIFY(working.isWorking());
    QCOMPARE(working.progressLabel, QStringLiteral("0 of 1"));
    QCOMPARE(sessionIdsOf(working.running), QStringList({"s1"}));
    QVERIFY(!m_delegate->clusterRect(index).isNull());
    gate().open();
    QVERIFY(waitDemandIdle(*m_queue, *m_demand));
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(jobsFrom(0), QStringList({"s1 gated Succeeded"}));

    // The settings object must outlive the model that writes to it
    destroyUi();
}

// ---- Nothing in the row is clickable ------------------------------------------------------

// Left, right, middle and double clicks over the working indicator and its
// label, and over the warning badge and its count, out to the row's edge: no
// job is created or cancelled, no check is toggled, and the current index and
// the selection are exactly what the base delegate leaves for the same clicks.
void PlotRowDelegateTest::clickOnClusterIsAClickOnTheRow()
{
    // Syn/ea badged: s1 rejects EA_IN = -1 (s2 has no EA_IN, s3 is hidden)
    QVERIFY(PlotFixture::giveInput(*m_model, QStringLiteral("s1"), QStringLiteral("EA_IN"), -1));
    m_model->flushPendingInvalidations();
    check("ea");
    QVERIFY(waitDemandIdle(*m_queue, *m_demand));
    const QModelIndex badgeRow = indexOf("Syn/ea");
    const DemandState badged = row("Syn/ea");
    QCOMPARE(badged.failedCount, 1);
    QVERIFY(badged.showsWarning());

    // Syn/g working: s1 held in the gate, s2 chosen next
    const QModelIndex workingRow = makeWorkingRow();
    QVERIFY(workingRow.isValid());
    const DemandState working = row("Syn/g");

    // Both clusters are painted
    {
        const QImage with = grabViewport();
        const QImage base = grabViewportWithBaseDelegate();
        for (const QModelIndex &index : {workingRow, badgeRow}) {
            const QRect cluster = m_delegate->clusterRect(index);
            QVERIFY(!cluster.isNull());
            QVERIFY(cut(with, cluster) != cut(base, cluster));
        }
    }

    const Quiet quiet(*m_queue);
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);
    QSignalSpy finishedSpy(m_queue.get(), &JobQueue::jobFinished);
    const QModelIndex start = indexOf("Syn/plain");
    QStyledItemDelegate base;

    for (const QModelIndex &index : {workingRow, badgeRow}) {
        const QRect cluster = m_delegate->clusterRect(index);
        const QRect rowRect = m_view->visualRect(index);
        const int y = cluster.center().y();
        const QList<QPoint> points = {cluster.center(), QPoint(cluster.left(), y), QPoint(cluster.right(), y),
                                      QPoint(cluster.right(), cluster.top()), QPoint(rowRect.right(), y)};
        for (const QPoint &point : points) {
            for (const Click kind : {Click::Left, Click::Right, Click::Middle, Click::Double}) {
                const QByteArray what = QByteArray::number(int(kind)) + " at "
                    + QByteArray::number(point.x()) + "," + QByteArray::number(point.y());
                const ClickOutcome withDelegate = clickWith(m_delegate, start, point, kind);
                const ClickOutcome withBase = clickWith(&base, start, point, kind);
                QVERIFY2(withDelegate.current == withBase.current, what.constData());
                QVERIFY2(withDelegate.selected == withBase.selected, what.constData());
                QVERIFY2(isChecked(workingRow), what.constData());
                QVERIFY2(isChecked(badgeRow), what.constData());
            }
        }
    }

    spin();
    QVERIFY(quiet.holds());
    QCOMPARE(cancelSpy.count(), 0);
    QCOMPARE(finishedSpy.count(), 0);
    QVERIFY(row("Syn/g") == working);
    QVERIFY(row("Syn/ea") == badged);
}

// ---- Tooltip ----------------------------------------------------------------------------

void PlotRowDelegateTest::toolTipComesFromPlotState()
{
    // A failed row: s1 rejects EA_IN = -1 (s2 has no EA_IN, s3 is hidden)
    QVERIFY(PlotFixture::giveInput(*m_model, QStringLiteral("s1"), QStringLiteral("EA_IN"), -1));
    m_model->flushPendingInvalidations();
    check("ea");
    QVERIFY(waitDemandIdle(*m_queue, *m_demand));
    const QModelIndex failedRow = indexOf("Syn/ea");
    const QString failedText = row("Syn/ea").toolTip;
    QVERIFY(failedText.contains(QStringLiteral("Explicit A: negative input")));
    QCOMPARE(m_delegate->toolTipFor(failedRow), failedText);

    // A working row
    const QModelIndex workingRow = makeWorkingRow();
    QVERIFY(workingRow.isValid());
    const QString workingText = row("Syn/g").toolTip;
    QVERIFY(workingText.contains(QStringLiteral("Computing: 0 of 2 done")));
    QVERIFY(workingText.contains(QStringLiteral("Jump 1 - Gated: step 1")));
    QCOMPARE(m_delegate->toolTipFor(workingRow), workingText);

    // A plain row, a category, no index
    check("plain");
    const QModelIndex plainRow = indexOf("Syn/plain");
    const QModelIndex category = m_plots->index(0, 0);
    QVERIFY(row("Syn/plain").isPlain());
    QVERIFY(m_delegate->toolTipFor(plainRow).isEmpty());
    QVERIFY(m_delegate->toolTipFor(indexOf("Syn/h")).isEmpty());       // unchecked
    QVERIFY(m_delegate->toolTipFor(category).isEmpty());
    QVERIFY(m_delegate->toolTipFor(QModelIndex()).isEmpty());

    // helpEvent() shows it, over the whole row
    const auto help = [this](const QModelIndex &index) {
        const QStyleOptionViewItem opt = optionFor(index);
        const QPoint position = namePoint(index);
        QHelpEvent event(QEvent::ToolTip, position, m_view->viewport()->mapToGlobal(position));
        return m_delegate->helpEvent(&event, m_view.get(), opt, index);
    };
    QVERIFY(help(workingRow));
    QCOMPARE(QToolTip::text(), workingText);
    QVERIFY(help(failedRow));
    QCOMPARE(QToolTip::text(), failedText);
    QVERIFY(!help(plainRow));
    QVERIFY(!help(category));
    QToolTip::hideText();
}

// ---- Repaint ----------------------------------------------------------------------------

void PlotRowDelegateTest::plotStateChangeRepaintsRow()
{
    const QModelIndex index = makeWorkingRow();
    QVERIFY(index.isValid());
    auto *paints = new PaintCounter(m_view->viewport());
    spin();
    spin();

    // Nothing the view listens to changes: the plot model is untouched
    QSignalSpy plotModelSpy(m_plots.get(), &QAbstractItemModel::dataChanged);
    QSignalSpy changedSpy(m_demand.get(), &CalculationDemand::plotStateChanged);
    const int before = paints->count;
    PlotFixture::show(*m_model, {"s3"});
    m_demand->flush();
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(changedSpy.at(0).at(0).toString(), QStringLiteral("Syn/g"));
    QCOMPARE(row("Syn/g").wantedCount, 3);
    QCOMPARE(row("Syn/g").progressLabel, QStringLiteral("0 of 3"));
    QTRY_VERIFY(paints->count > before);
    QCOMPARE(plotModelSpy.count(), 0);

    // An id that is not in the model is harmless
    emit m_demand->plotStateChanged(QStringLiteral("Syn/nothing"));
}

// ---- Lifetime ---------------------------------------------------------------------------

// MainWindow deletes the component before the docks: the delegate then is the
// base delegate.
void PlotRowDelegateTest::survivesDemandDestroyedFirst()
{
    const QModelIndex index = makeWorkingRow();
    QVERIFY(index.isValid());
    const QPoint inCluster = m_delegate->clusterRect(index).center();
    const Quiet quiet(*m_queue);

    m_demand.reset();

    QCOMPARE(m_delegate->clusterRect(index), QRect());
    QVERIFY(m_delegate->toolTipFor(index).isEmpty());
    click(inCluster);
    QTest::mouseDClick(m_view->viewport(), Qt::LeftButton, {}, inCluster);
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, inCluster);
    QVERIFY(isChecked(index));

    click(checkBoxCentre(index));               // the check box still works
    QVERIFY(!isChecked(index));
    click(checkBoxCentre(index));
    QVERIFY(isChecked(index));
    pressSpaceOn(index);
    QVERIFY(!isChecked(index));
    pressSpaceOn(index);
    QVERIFY(isChecked(index));

    const QStyleOptionViewItem opt = optionFor(index);
    QHelpEvent event(QEvent::ToolTip, inCluster, m_view->viewport()->mapToGlobal(inCluster));
    QVERIFY(!m_delegate->helpEvent(&event, m_view.get(), opt, index));

    QApplication::processEvents();
    QCOMPARE(m_view->viewport()->grab().toImage(), grabViewportWithBaseDelegate());
    QVERIFY(quiet.holds());
}

// FLYSIGHT_TEST_MAIN with a QApplication: a Widgets test writes its own main().
// The macro builds a QCoreApplication, and testmain.h, which every test
// includes, must not come to need Qt Widgets for the sake of this one.
// The order is the same - deterministic hash seed, application object,
// TestEnvironment, test object - and the style is the application's
// (src/main.cpp), so that what is measured here is what the user sees.
int main(int argc, char **argv)
{
    QHashSeed::setDeterministicGlobalSeed();
    // CTest sets this for every test; a run by hand must not open windows either
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    FlySightTest::TestEnvironment env(QStringLiteral("PlotRowDelegateTest"));
    PlotRowDelegateTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_plot_row_delegate.moc"
