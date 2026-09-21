// The plot list's row delegate (PlotRowDelegate) in an offscreen QTreeView, on
// a real PlotRequests, PlotModel, JobQueue and SessionModel, with the synthetic
// plots of plotfixture.h, driven by synthesized mouse and key events. This is
// the only test that links Qt Widgets (FLYSIGHT_BUILD_WIDGET_TESTS).
// Sensor-fusion-jobs spec 9.2-9.4 (the view half); acceptance 16 (wiring half):
// the view adds no path from a model change to a request, and a check made by
// direct interaction with the row is the one thing it reports.
//
// What is proved here is what the view owns: plain rows are the base
// delegate's, hit-testing, the check gesture, the tooltip, the repaint. State,
// counts and what is requested are PlotRequests' (tst_plot_requests).
//
// Synchronization: Gate::waitEntered() proves the worker is inside a compute
// function; QTRY_* and waitIdle() spin the event loop; PlotRequests::flush()
// runs a pending pass before a row state is read or an image is grabbed. There
// are no sleeps.

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

#include "engine/calculationregistry.h"
#include "jobfixture.h"
#include "jobmodel.h"
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "logbookprobe.h"
#include "plotfixture.h"
#include "plotmodel.h"
#include "plotrequests.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testutil.h"
#include "ui/docks/plotselection/PlotRowDelegate.h"

using namespace FlySight;
using namespace FlySightTest;

using Control = PlotRowState::Control;

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

} // namespace

class PlotRowDelegateTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void plainRowsAreIdenticalToBaseDelegate();
    void missingRowPaintsControl();
    void longNameIsElidedNotTheCluster();
    void checkBoxClickIsGesture();
    void spaceKeyIsGesture();
    void uncheckIsNotAGesture();
    void programmaticCheckStartsNothingWithViewAttached();
    void startupStyleRestoreStartsNothingWithViewAttached();
    void refreshClickRequests();
    void cancelClickCancelsAndLeavesChecked();
    void controlClickDoesNotToggleOrSelect();
    void pressInsideReleaseOutsideDoesNothing();
    void pressOutsideReleaseInsideDoesNothing();
    void clickOnLabelOrBadgeDoesNothing();
    void rightClickDoesNothing();
    void doubleClickOnRefreshRequestsOnceAndCancelsNothing();
    void toolTipComesFromRowState();
    void rowStateChangeRepaintsRow();
    void survivesRequestsDestroyedFirst();

private:
    Gate &gate() { return m_world->gate(); }

    /// PlotModel, PlotRequests, the view and the delegate, as the application
    /// builds them: the component before the view. `plots` go into the model
    /// at once unless `plotsLater`. False when the view was never exposed (the
    /// rest is then not built); check it with QVERIFY in the test function.
    [[nodiscard]] bool buildUi(const QVector<PlotValue> &plots, QSettings *settings = nullptr, bool plotsLater = false);
    void destroyUi();

    /// A programmatic check: never a gesture.
    void check(const char *measurement, bool enabled = true)
    {
        m_plots->setPlotEnabled(QStringLiteral("Syn"), QString::fromLatin1(measurement), enabled);
    }
    bool isChecked(const QModelIndex &index) const { return index.data(Qt::CheckStateRole).toInt() == Qt::Checked; }
    /// The current row state: a pending pass runs first.
    PlotRowState row(const char *plotId)
    {
        m_requests->flush();
        return m_requests->rowState(QString::fromLatin1(plotId));
    }
    /// Two turns of the event loop and a flush: whatever was going to start by
    /// itself has started, and whatever was going to be painted is painted.
    void spin()
    {
        PlotFixture::spin(m_requests.get());
        QApplication::processEvents();
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

    /// Syn/g checked from code with s1 and s2 missing: the row shows refresh, 2.
    QModelIndex makeRefreshRow()
    {
        check("g");
        const PlotRowState state = row("Syn/g");
        if (state.control() != Control::Refresh || state.controlCount() != 2)
            return QModelIndex();
        spin();
        return indexOf("Syn/g");
    }

    std::unique_ptr<JobWorld> m_world;
    std::unique_ptr<PlotFixture> m_fixture;
    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    std::unique_ptr<PlotModel> m_plots;
    std::unique_ptr<PlotRequests> m_requests;
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
    m_requests = std::make_unique<PlotRequests>(m_model.get(), m_plots.get(), m_queue.get());

    // As PlotSelectionDockFeature configures its view
    m_view = std::make_unique<QTreeView>();
    m_view->setModel(m_plots.get());
    m_view->setHeaderHidden(true);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->resize(300, 300);
    m_delegate = new PlotRowDelegate(m_requests.get(), m_view.get());
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
    m_requests.reset();
    m_plots.reset();
}

// Note what is to be checked, tear everything down, and only then check: a
// failing check returns from cleanup(), and whatever were still alive then
// would be alive under the next init() (see tst_jobqueue).
void PlotRowDelegateTest::cleanup()
{
    // Release whatever a test left in the gate, then let nothing linger
    // inside a compute function
    if (m_world)
        gate().open(8);
    bool becameIdle = true;
    if (m_queue) {
        becameIdle = waitIdle(*m_queue);
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

// Spec 9.2, last bullet: a row with nothing pending, missing or failed, every
// row of a plot that is not backed by an explicit calculation, and every
// unchecked row look exactly as they do today - pixel for pixel.
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
    QCOMPARE(m_delegate->controlRect(indexOf("Syn/plain")), QRect());

    // Without a component every row is plain, whatever is checked
    check("g");
    QVERIFY(!row("Syn/g").isPlain());
    PlotRowDelegate inert(nullptr, m_view.get());
    m_view->setItemDelegate(&inert);
    QApplication::processEvents();
    const QImage withInert = m_view->viewport()->grab().toImage();
    m_view->setItemDelegate(m_delegate);
    QCOMPARE(withInert, grabViewportWithBaseDelegate());
    QCOMPARE(inert.controlRect(indexOf("Syn/g")), QRect());
    QVERIFY(inert.toolTipFor(indexOf("Syn/g")).isEmpty());
}

void PlotRowDelegateTest::missingRowPaintsControl()
{
    const int plainHeight = m_view->visualRect(indexOf("Syn/g")).height();
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());

    const QRect control = m_delegate->controlRect(index);
    QVERIFY(!control.isNull());
    const QRect rowRect = m_view->visualRect(index);
    QVERIFY(rowRect.contains(control));
    QCOMPARE(control.right(), rowRect.right());
    QCOMPARE(control.height(), rowRect.height());

    const QImage with = grabViewport();
    const QImage base = grabViewportWithBaseDelegate();
    QVERIFY(cut(with, control) != cut(base, control));                              // the refresh glyph
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
    m_view->resize(120, 300);
    spin();

    check("g");
    check("g2");
    QCOMPARE(row("Syn/g").controlCount(), 2);
    QCOMPARE(row("Syn/g2").controlCount(), 2);
    const QModelIndex longRow = indexOf("Syn/g");
    const QModelIndex shortRow = indexOf("Syn/g2");

    // The cluster is fully visible
    const QRect viewport = m_view->viewport()->rect();
    const QRect control = m_delegate->controlRect(longRow);
    QVERIFY(!control.isNull());
    QVERIFY(viewport.contains(control));
    QCOMPARE(m_view->visualRect(longRow).right(), viewport.right());

    // A band over the label and the control of each row (same x, other y)
    const int labelWidth = m_view->fontMetrics().horizontalAdvance(QStringLiteral("2"));
    const auto clusterBand = [&](const QModelIndex &index) {
        QRect band = m_delegate->controlRect(index);
        band.setLeft(band.left() - labelWidth - 3);
        return band;
    };
    QCOMPARE(clusterBand(longRow).left(), clusterBand(shortRow).left());

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

// ---- The check gesture ------------------------------------------------------------------

void PlotRowDelegateTest::checkBoxClickIsGesture()
{
    const QModelIndex index = indexOf("Syn/g");
    QVERIFY(!isChecked(index));
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);

    click(checkBoxCentre(index));
    QVERIFY(isChecked(index));
    QCOMPARE(queuedSpy.count(), 2);             // s1 and s2; s3 is hidden
    QVERIFY(gate().waitEntered());

    const PlotRowState state = row("Syn/g");
    QCOMPARE(state.control(), Control::Cancel);
    QCOMPARE(state.progressLabel, QStringLiteral("0 of 2"));

    gate().open(2);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(queuedSpy.count(), 2);
}

void PlotRowDelegateTest::spaceKeyIsGesture()
{
    const QModelIndex index = indexOf("Syn/g");
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);

    pressSpaceOn(index);
    QVERIFY(isChecked(index));
    QCOMPARE(queuedSpy.count(), 2);

    gate().open(2);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(row("Syn/g").isPlain());
}

void PlotRowDelegateTest::uncheckIsNotAGesture()
{
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());
    const Quiet quiet(*m_queue);

    click(checkBoxCentre(index));
    QVERIFY(!isChecked(index));
    spin();
    QVERIFY(row("Syn/g").isPlain());
    QVERIFY(quiet.holds());

    check("g");
    pressSpaceOn(index);
    QVERIFY(!isChecked(index));
    spin();
    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());
}

// The wiring half of acceptance 16: with the view and the delegate attached,
// no way of checking a plot from code starts anything.
void PlotRowDelegateTest::programmaticCheckStartsNothingWithViewAttached()
{
    const Quiet quiet(*m_queue);
    const QModelIndex index = indexOf("Syn/g");

    // setPlotEnabled: applyProfile() and restored settings
    m_plots->setPlotEnabled(QStringLiteral("Syn"), QStringLiteral("g"), true);
    spin();
    QCOMPARE(row("Syn/g").control(), Control::Refresh);
    QCOMPARE(row("Syn/g").controlCount(), 2);
    m_plots->setPlotEnabled(QStringLiteral("Syn"), QStringLiteral("g"), false);

    // togglePlot: the Plots menu and its shortcuts
    QVERIFY(m_plots->togglePlot(QStringLiteral("Syn"), QStringLiteral("g")));
    spin();
    QCOMPARE(row("Syn/g").controlCount(), 2);
    QVERIFY(!m_plots->togglePlot(QStringLiteral("Syn"), QStringLiteral("g")));

    // setData(CheckStateRole): what the check box writes. The write is not the
    // gesture; the delegate's explicit call is.
    QVERIFY(m_plots->setData(index, Qt::Checked, Qt::CheckStateRole));
    spin();
    QVERIFY(isChecked(index));
    QCOMPARE(row("Syn/g").control(), Control::Refresh);
    QCOMPARE(row("Syn/g").controlCount(), 2);
    QVERIFY(!m_delegate->controlRect(index).isNull());

    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());
}

// Plots restored as checked from the settings come up through modelReset, after
// the view and the delegate exist - as in MainWindow's constructor.
void PlotRowDelegateTest::startupStyleRestoreStartsNothingWithViewAttached()
{
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
    const PlotRowState state = row("Syn/g");
    QCOMPARE(state.control(), Control::Refresh);
    QCOMPARE(state.controlCount(), 2);
    QVERIFY(!m_delegate->controlRect(index).isNull());
    spin();
    QVERIFY(quiet.holds());
    QVERIFY(m_queue->isIdle());

    // The settings object must outlive the model that writes to it
    destroyUi();
}

// ---- The control ------------------------------------------------------------------------

void PlotRowDelegateTest::refreshClickRequests()
{
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);

    click(m_delegate->controlRect(index).center());
    QCOMPARE(queuedSpy.count(), 2);
    QVERIFY(isChecked(index));
    QVERIFY(gate().waitEntered());
    QCOMPARE(row("Syn/g").control(), Control::Cancel);

    gate().open(2);
    QVERIFY(waitIdle(*m_queue));
    QVERIFY(row("Syn/g").isPlain());
    QCOMPARE(m_delegate->controlRect(index), QRect());
}

void PlotRowDelegateTest::cancelClickCancelsAndLeavesChecked()
{
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());
    click(m_delegate->controlRect(index).center());
    QVERIFY(gate().waitEntered());
    QCOMPARE(row("Syn/g").control(), Control::Cancel);
    QCOMPARE(m_queue->activeJobs().size(), 2);
    spin();

    const Quiet quiet(*m_queue);
    click(m_delegate->controlRect(index).center());

    // At once: refresh is back, and the plot is still checked
    PlotRowState state = row("Syn/g");
    QCOMPARE(state.control(), Control::Refresh);
    QCOMPARE(state.controlCount(), 2);
    QVERIFY(isChecked(index));

    QVERIFY(waitIdle(*m_queue));
    const JobModel *jobs = m_queue->model();
    QCOMPARE(jobs->rowCount(), 2);
    for (int r = 0; r < jobs->rowCount(); ++r)
        QCOMPARE(jobs->record(r).state, JobState::Cancelled);
    state = row("Syn/g");
    QCOMPARE(state.control(), Control::Refresh);
    QVERIFY(isChecked(index));
    QVERIFY(quiet.holds());
}

void PlotRowDelegateTest::controlClickDoesNotToggleOrSelect()
{
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());
    const QModelIndex other = indexOf("Syn/plain");
    m_view->setCurrentIndex(other);
    const QModelIndexList selectedBefore = m_view->selectionModel()->selectedIndexes();
    QCOMPARE(selectedBefore, QModelIndexList({other}));
    spin();

    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    click(m_delegate->controlRect(index).center());
    QCOMPARE(queuedSpy.count(), 2);

    QVERIFY(isChecked(index));
    QCOMPARE(m_view->currentIndex(), other);
    QCOMPARE(m_view->selectionModel()->selectedIndexes(), selectedBefore);

    // The same for cancel
    QVERIFY(gate().waitEntered());
    QCOMPARE(row("Syn/g").control(), Control::Cancel);
    spin();
    click(m_delegate->controlRect(index).center());
    QCOMPARE(row("Syn/g").control(), Control::Refresh);
    QVERIFY(isChecked(index));
    QCOMPARE(m_view->currentIndex(), other);
    QCOMPARE(m_view->selectionModel()->selectedIndexes(), selectedBefore);
    QVERIFY(waitIdle(*m_queue));
}

void PlotRowDelegateTest::pressInsideReleaseOutsideDoesNothing()
{
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());
    const Quiet quiet(*m_queue);
    const QPoint inside = m_delegate->controlRect(index).center();

    // Released on the name ...
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, inside);
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, namePoint(index));
    // ... and on the check box, which a release would otherwise toggle
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, inside);
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, checkBoxCentre(index));
    // ... and on another row
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, inside);
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, namePoint(indexOf("Syn/plain")));

    spin();
    QVERIFY(isChecked(index));
    QCOMPARE(row("Syn/g").control(), Control::Refresh);
    QVERIFY(quiet.holds());

    // The control still works afterwards
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    click(inside);
    QCOMPARE(queuedSpy.count(), 2);
}

void PlotRowDelegateTest::pressOutsideReleaseInsideDoesNothing()
{
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());
    const Quiet quiet(*m_queue);
    const QPoint inside = m_delegate->controlRect(index).center();

    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, namePoint(index));
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, inside);
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, checkBoxCentre(index));
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, inside);
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, namePoint(indexOf("Syn/plain")));
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, inside);

    spin();
    QVERIFY(isChecked(index));
    QCOMPARE(row("Syn/g").control(), Control::Refresh);
    QVERIFY(quiet.holds());
}

void PlotRowDelegateTest::clickOnLabelOrBadgeDoesNothing()
{
    // The label: just left of the control's hit rectangle
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());
    const QRect control = m_delegate->controlRect(index);
    {
        const Quiet quiet(*m_queue);
        const int labelWidth = m_view->fontMetrics().horizontalAdvance(QStringLiteral("2"));
        for (int x = control.left() - 1; x >= control.left() - labelWidth - 2; --x)
            click(QPoint(x, control.center().y()));
        spin();
        QVERIFY(isChecked(index));
        QCOMPARE(row("Syn/g").control(), Control::Refresh);
        QVERIFY(quiet.holds());
    }

    // The badge: Syn/ea after its job ran and rejected EA_IN = -1
    QVERIFY(PlotFixture::giveInput(*m_model, QStringLiteral("s1"), QStringLiteral("EA_IN"), -1));
    const QModelIndex badgeRow = indexOf("Syn/ea");
    click(checkBoxCentre(badgeRow));            // the gesture: one job, for s1
    QVERIFY(waitIdle(*m_queue));
    const PlotRowState state = row("Syn/ea");
    QCOMPARE(state.failedCount, 1);
    QVERIFY(state.showsWarning());
    QCOMPARE(state.control(), Control::None);
    QCOMPARE(m_delegate->controlRect(badgeRow), QRect());
    spin();

    // It is painted ...
    const QRect rowRect = m_view->visualRect(badgeRow);
    const QRect cluster(rowRect.right() - 39, rowRect.top(), 40, rowRect.height());
    QVERIFY(cut(grabViewport(), cluster) != cut(grabViewportWithBaseDelegate(), cluster));

    // ... and inert, all the way to the row's edge
    const Quiet quiet(*m_queue);
    for (int x = rowRect.right(); x > rowRect.right() - 40; x -= 3)
        click(QPoint(x, rowRect.center().y()));
    spin();
    QVERIFY(isChecked(badgeRow));
    QVERIFY(row("Syn/ea") == state);
    QVERIFY(quiet.holds());
}

void PlotRowDelegateTest::rightClickDoesNothing()
{
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());
    const Quiet quiet(*m_queue);

    click(m_delegate->controlRect(index).center(), Qt::RightButton);
    click(m_delegate->controlRect(index).center(), Qt::MiddleButton);
    spin();
    QVERIFY(isChecked(index));
    QCOMPARE(row("Syn/g").control(), Control::Refresh);
    QVERIFY(quiet.holds());

    // A right press does not arm the control, and it disarms a left press
    const QPoint inside = m_delegate->controlRect(index).center();
    QTest::mousePress(m_view->viewport(), Qt::RightButton, {}, inside);
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, inside);
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, inside);
    QTest::mousePress(m_view->viewport(), Qt::RightButton, {}, inside);
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, inside);
    spin();
    QVERIFY(quiet.holds());
}

// The first click requests; the second half of the double click must not land
// on the cancel control that replaced the refresh control.
void PlotRowDelegateTest::doubleClickOnRefreshRequestsOnceAndCancelsNothing()
{
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());
    QSignalSpy queuedSpy(m_queue.get(), &JobQueue::jobQueued);
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);
    QSignalSpy finishedSpy(m_queue.get(), &JobQueue::jobFinished);

    // What a widget receives for a double click: press, release, double
    // click, release. (QTest::mouseDClick() on a widget sends the third only.)
    const QPoint inside = m_delegate->controlRect(index).center();
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, inside);
    QCOMPARE(queuedSpy.count(), 2);
    QTest::mouseDClick(m_view->viewport(), Qt::LeftButton, {}, inside);
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, inside);

    QCOMPARE(queuedSpy.count(), 2);
    QVERIFY(gate().waitEntered());
    QCOMPARE(cancelSpy.count(), 0);
    QCOMPARE(finishedSpy.count(), 0);
    QCOMPARE(m_queue->activeJobs().size(), 2);
    QCOMPARE(row("Syn/g").control(), Control::Cancel);
    QVERIFY(isChecked(index));

    gate().open(2);
    QVERIFY(waitIdle(*m_queue));
    QCOMPARE(queuedSpy.count(), 2);
    const JobModel *jobs = m_queue->model();
    for (int r = 0; r < jobs->rowCount(); ++r)
        QCOMPARE(jobs->record(r).state, JobState::Succeeded);
}

// ---- Tooltip ----------------------------------------------------------------------------

void PlotRowDelegateTest::toolTipComesFromRowState()
{
    // A missing row
    const QModelIndex missingRow = makeRefreshRow();
    QVERIFY(missingRow.isValid());
    const QString missingText = row("Syn/g").toolTip;
    QVERIFY(missingText.contains(QStringLiteral("Jump 1")));
    QVERIFY(missingText.contains(QStringLiteral("Jump 2")));
    QCOMPARE(m_delegate->toolTipFor(missingRow), missingText);

    // A failed row
    QVERIFY(PlotFixture::giveInput(*m_model, QStringLiteral("s1"), QStringLiteral("EA_IN"), -1));
    const QModelIndex failedRow = indexOf("Syn/ea");
    click(checkBoxCentre(failedRow));
    QVERIFY(waitIdle(*m_queue));
    const QString failedText = row("Syn/ea").toolTip;
    QVERIFY(failedText.contains(QStringLiteral("Explicit A: negative input")));
    QCOMPARE(m_delegate->toolTipFor(failedRow), failedText);

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
    QVERIFY(help(missingRow));
    QCOMPARE(QToolTip::text(), missingText);
    QVERIFY(help(failedRow));
    QCOMPARE(QToolTip::text(), failedText);
    QVERIFY(!help(plainRow));
    QVERIFY(!help(category));
    QToolTip::hideText();
}

// ---- Repaint ----------------------------------------------------------------------------

void PlotRowDelegateTest::rowStateChangeRepaintsRow()
{
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());
    auto *paints = new PaintCounter(m_view->viewport());
    spin();
    spin();

    // Nothing the view listens to changes: the plot model is untouched
    QSignalSpy plotModelSpy(m_plots.get(), &QAbstractItemModel::dataChanged);
    QSignalSpy changedSpy(m_requests.get(), &PlotRequests::rowStateChanged);
    const int before = paints->count;
    PlotFixture::show(*m_model, {"s3"});
    m_requests->flush();
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(row("Syn/g").controlCount(), 3);
    QTRY_VERIFY(paints->count > before);
    QCOMPARE(plotModelSpy.count(), 0);

    // An id that is not in the model is harmless
    emit m_requests->rowStateChanged(QStringLiteral("Syn/nothing"));
}

// ---- Lifetime ---------------------------------------------------------------------------

// MainWindow deletes the component before the docks: the delegate then is the
// base delegate.
void PlotRowDelegateTest::survivesRequestsDestroyedFirst()
{
    const QModelIndex index = makeRefreshRow();
    QVERIFY(index.isValid());
    const QPoint control = m_delegate->controlRect(index).center();
    const Quiet quiet(*m_queue);

    m_requests.reset();

    QCOMPARE(m_delegate->controlRect(index), QRect());
    QVERIFY(m_delegate->toolTipFor(index).isEmpty());
    click(control);
    QTest::mouseDClick(m_view->viewport(), Qt::LeftButton, {}, control);
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, control);
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
    QHelpEvent event(QEvent::ToolTip, control, m_view->viewport()->mapToGlobal(control));
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
