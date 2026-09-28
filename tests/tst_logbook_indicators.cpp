// The logbook's row warning and pending cells over the demand layer, in a real
// LogbookView (LogbookCellDelegate on the tree's own header) over a real
// SessionModel, executor (JobQueue) and CalculationDemand, with the synthetic
// calculations of jobfixture.h and the plots of plotfixture.h, offscreen.
// Beside it, in the same window, a reference QTreeView configured as the
// logbook's tree but with the base delegate, over the same model: what the
// logbook looks like without the demand layer.
//
// What is proved here is what the view owns: cells without a failure or a
// pending value are the base delegate's; a row whose recording could not be
// computed shows one glyph, the style's standard warning icon, at the leading
// edge of its first visual cell, whatever column that is, loaded or not,
// without changing any size; its hover is the recording's failures; a click on
// it is a click on the cell; it appears and goes with the failure; the header
// is the tree's own and nothing animates; cells read pending (distinct from
// unavailable and from the row's unreadable-record pending state) without a
// trace in the model, its cached values or index.json; each announcement
// repaints one column. Progress and failures, and what is computed, are
// CalculationDemand's (tst_calculation_demand); background work and the list
// of failures are shown in the status bar (tst_status_bar).
//
// Synchronization: Gate::waitEntered() proves the worker is inside a compute
// function; QTRY_*, waitDemandIdle() and waitForIdle() spin the event loop;
// CalculationDemand::flush() runs a pending pass before a value is read. The
// only wait is the one that proves nothing repaints by itself.

#include <functional>
#include <memory>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHashFunctions>
#include <QHeaderView>
#include <QHelpEvent>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPaintEvent>
#include <QProgressBar>
#include <QRegion>
#include <QSignalSpy>
#include <QStyle>
#include <QStyleFactory>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QToolTip>
#include <QTreeView>
#include <QtTest>

#include "calculationdemand.h"
#include "calculationrecord.h"
#include "demandstate.h"
#include "engine/calculationregistry.h"
#include "jobfixture.h"
#include "jobmodel.h"
#include "jobqueue.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "plotfixture.h"
#include "plotmodel.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testutil.h"
#include "ui/docks/logbook/LogbookCellDelegate.h"
#include "ui/docks/logbook/LogbookView.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

/// Counts the paint events of a widget. With an `area`, only those confined to
/// it: what a repaint of one column causes, not a stray expose of the whole
/// widget.
class PaintCounter : public QObject {
public:
    explicit PaintCounter(QWidget *watched, const QRect &area = QRect())
        : QObject(watched), m_area(area)
    {
        watched->installEventFilter(this);
    }
    int count = 0;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Paint
            && (m_area.isNull() || m_area.contains(static_cast<QPaintEvent *>(event)->region().boundingRect())))
            ++count;
        return QObject::eventFilter(watched, event);
    }

private:
    QRect m_area;
};

/// Unites the regions of the paint events of a widget.
class RegionRecorder : public QObject {
public:
    explicit RegionRecorder(QWidget *watched) : QObject(watched) { watched->installEventFilter(this); }
    QRegion region;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Paint)
            region += static_cast<QPaintEvent *>(event)->region();
        return QObject::eventFilter(watched, event);
    }
};

/// A rect of widget coordinates, cut from an image grabbed from the widget.
QImage cut(const QImage &image, const QRect &rect)
{
    const qreal ratio = image.devicePixelRatio();
    return image.copy(QRect(qRound(rect.x() * ratio), qRound(rect.y() * ratio),
                            qRound(rect.width() * ratio), qRound(rect.height() * ratio)));
}

/// index.json as its bytes on disk.
QByteArray indexBytes()
{
    QFile file(TestEnvironment::instance().indexPath());
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class LogbookIndicatorsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void plainHeaderAndCellsAreIdenticalToBase();
    void headerIsPlainAndNothingAnimates();
    void rowWarningFollowsTheText();
    void rowWarningHoverIsTheSessionsFailures();
    void rowWarningFollowsTheFirstVisualColumn();
    void rowWarningFollowsFailures();
    void failedLoadSessionShowsRowWarningNotPending();
    void failedWriteIsListedInTheHover();
    void clickOnRowWarningIsAClickOnTheCell();
    void pendingCellsAreDistinctFromUnavailable();
    void pendingCellBecomesValueWhenRecordIsWritten_data();
    void pendingCellBecomesValueWhenRecordIsWritten();
    void sortingTreatsPendingAsUnavailable();
    void unreadableRecordPendingIsNotDemandPending_data();
    void unreadableRecordPendingIsNotDemandPending();
    void pendingCellsChangeRepaintsOnlyThatColumn();
    void survivesDemandDestroyedFirst();

private:
    Gate &gate() { return m_world->gate(); }

    /// The logbook view and the reference tree in one window, as described at
    /// the top of the file. False when the window was never exposed.
    [[nodiscard]] bool buildUi();
    void destroyUi();
    /// The executor, the demand layer and the UI over the current model.
    [[nodiscard]] bool buildServices();

    QTreeView *tree() const { return m_logbook->findChild<QTreeView *>(); }
    QHeaderView *header() const { return tree()->header(); }
    LogbookCellDelegate *cells() const { return qobject_cast<LogbookCellDelegate *>(tree()->itemDelegate()); }

    /// The description column plus attribute columns over `keys`, through
    /// LogbookColumnStore::setColumns(): the path of a profile and the editor.
    static void enableColumns(const QStringList &keys)
    {
        QVector<LogbookColumn> columns{descriptionColumn()};
        for (const QString &key : keys)
            columns.append(attributeColumn(key));
        LogbookColumnStore::instance().setColumns(columns);
    }
    /// The model column of the attribute column over `key`; -1 when none.
    int section(const char *key) const
    {
        for (int c = 0; c < m_model->columnCount(); ++c) {
            const LogbookColumn &column = m_model->column(c);
            if (column.type == ColumnType::SessionAttribute && column.attributeKey == QLatin1String(key))
                return c;
        }
        return -1;
    }
    int descriptionSection() const { return section(SessionKeys::Description); }
    int row(const char *id) const { return m_model->getSessionRow(QString::fromLatin1(id)); }
    static QString colId(const char *key) { return CalculationDemand::columnId(attributeColumn(QString::fromLatin1(key))); }
    /// The current progress of the computations: a pending pass runs first.
    DemandProgress progressNow()
    {
        m_demand->flush();
        return m_demand->progress();
    }
    /// The session of the executor's running job; empty when none runs.
    QString runningSession() const { return m_queue->job(m_queue->runningJob()).sessionId; }
    QModelIndex cell(const char *id, const char *key) const
    {
        return m_model->index(row(id), section(key));
    }
    /// The session's cell in the description column, the first visual one
    /// unless a test moves or hides it.
    QModelIndex firstCell(const char *id) const { return m_model->index(row(id), descriptionSection()); }
    /// The session has a current failure: a pending pass runs first.
    bool failed(const char *id)
    {
        m_demand->flush();
        return !m_demand->sessionFailures(QString::fromLatin1(id)).calculations.isEmpty();
    }
    static bool stored(const char *sessionId, const char *calculationId)
    {
        return LogbookManager::instance()
            .knownCalculationRecords(QString::fromLatin1(sessionId))
            .contains(QString::fromLatin1(calculationId));
    }
    /// The value index.json holds for (session, G_OUT) once the in-memory
    /// index is written.
    static QJsonValue flushedIndexValue(const char *sessionId)
    {
        LogbookManager::instance().flushIndex();
        return indexValue(QString::fromLatin1(sessionId), attributeColumn(QStringLiteral("G_OUT")));
    }
    /// Two turns of the event loop and a flush: whatever was going to start by
    /// itself has started, and whatever was going to be painted is painted.
    void spin()
    {
        PlotFixture::spin(m_demand.get());
        QApplication::processEvents();
    }
    [[nodiscard]] bool waitDemandIdle(int timeoutMs = 30000)
    {
        return FlySightTest::waitDemandIdle(*m_queue, *m_demand, timeoutMs);
    }
    /// Every session's wait ends and everything the demand layer wanted has run.
    [[nodiscard]] bool settleAndWait()
    {
        m_demand->endInputSettleWaits();
        return waitDemandIdle() && waitForIdle(*m_model);
    }
    /// G_OUT enabled with the gate held: s1 running with its progress text
    /// delivered, s2 and s4 waiting. False on any other outcome.
    [[nodiscard]] bool makeWorkingColumn()
    {
        enableColumns({QStringLiteral("G_OUT")});
        if (!gate().waitEntered())
            return false;
        if (!QTest::qWaitFor([this] { return progressNow().progressText == QStringLiteral("step 1"); }))
            return false;
        const DemandProgress progress = progressNow();
        if (progress.count != 3 || progress.sessionName != QStringLiteral("Jump 1")
            || runningSession() != QStringLiteral("s1"))
            return false;
        spin();
        return true;
    }
    /// EA_IN -1 on s1, 4 on s2 and s4; the columns over `keys` (EA1 among
    /// them) enabled and finished: s1, and only s1, has a current failure (a
    /// stored rejection of Explicit A).
    [[nodiscard]] bool makeFailedRow(const QStringList &keys = {QStringLiteral("EA1")})
    {
        for (const auto &[id, value] : {std::pair{"s1", -1.0}, std::pair{"s2", 4.0}, std::pair{"s4", 4.0}}) {
            if (!PlotFixture::giveInput(*m_model, QString::fromLatin1(id), QStringLiteral("EA_IN"), value))
                return false;
        }
        m_model->flushPendingInvalidations();
        enableColumns(keys);
        if (!waitDemandIdle() || !waitForIdle(*m_model))
            return false;
        spin();
        const QList<SessionFailures> failures = m_demand->failures();
        return failures.size() == 1 && failures.at(0).sessionId == QStringLiteral("s1");
    }

    /// The visible part of a column of the tree's viewport.
    QRect columnRect(int column) const
    {
        return QRect(tree()->columnViewportPosition(column), 0, tree()->columnWidth(column),
                     tree()->viewport()->height());
    }
    QRect cellRect(const QModelIndex &index) const { return tree()->visualRect(index); }
    QImage grabHeader() const { return header()->viewport()->grab().toImage(); }
    QImage grabReferenceHeader() const { return m_reference->header()->viewport()->grab().toImage(); }
    /// The tree's viewport, laid out afresh as a change of its delegate lays it
    /// out: hiding a section does not recompute the uniform row height.
    QImage grabCells() const
    {
        tree()->doItemsLayout();
        return tree()->viewport()->grab().toImage();
    }
    /// The tree's viewport, same size, selection and focus, with a default delegate.
    QImage grabCellsWithBaseDelegate()
    {
        QStyledItemDelegate base;
        QAbstractItemDelegate *under = tree()->itemDelegate();
        tree()->setItemDelegate(&base);
        QApplication::processEvents();
        const QImage image = grabCells();
        tree()->setItemDelegate(under);
        QApplication::processEvents();
        return image;
    }
    /// Sends a tooltip event to the tree's viewport at `pos` (the centre of
    /// the cell when null), as the application receives one: the view builds
    /// the option and asks the delegate. Whether it was accepted.
    bool cellHelp(const QModelIndex &index, QPoint pos = QPoint())
    {
        if (pos.isNull())
            pos = cellRect(index).center();
        QHelpEvent event(QEvent::ToolTip, pos, tree()->viewport()->mapToGlobal(pos));
        QApplication::sendEvent(tree()->viewport(), &event);
        return event.isAccepted();
    }
    /// A point of the cell far from its leading edge, where no glyph is.
    QPoint trailingPoint(const QModelIndex &index) const
    {
        const QRect rect = cellRect(index);
        return QPoint(rect.right() - 4, rect.center().y());
    }
    /// Hides any tooltip and waits until it is gone.
    [[nodiscard]] static bool hideToolTip()
    {
        QToolTip::hideText();
        return QTest::qWaitFor([] { return !QToolTip::isVisible(); }, 2000);
    }
    /// Row heights equal the reference tree's, and every cell's sizeHint()
    /// the base delegate's.
    void verifySizesAreTheBase()
    {
        QStyleOptionViewItem opt;
        opt.initFrom(tree()->viewport());
        opt.widget = tree();
        opt.font = tree()->font();
        const QStyledItemDelegate base;
        for (int r = 0; r < m_model->rowCount(); ++r) {
            QCOMPARE(tree()->visualRect(m_model->index(r, 0)).height(),
                     m_reference->visualRect(m_model->index(r, 0)).height());
            for (int c = 0; c < m_model->columnCount(); ++c)
                QCOMPARE(cells()->sizeHint(opt, m_model->index(r, c)), base.sizeHint(opt, m_model->index(r, c)));
        }
    }
    /// The glyph is painted in exactly one cell, `index`, inside it, and every
    /// other visible cell that is not pending is the base delegate's.
    void verifyOnlyGlyphAt(const QModelIndex &index)
    {
        QVERIFY(index.isValid());
        const QImage with = grabCells();
        const QImage base = grabCellsWithBaseDelegate();
        for (int r = 0; r < m_model->rowCount(); ++r) {
            for (int c = 0; c < m_model->columnCount(); ++c) {
                const QModelIndex other = m_model->index(r, c);
                const QRect glyph = cells()->warningRect(other);
                if (other == index) {
                    QVERIFY2(!glyph.isNull(), qPrintable(QStringLiteral("row %1 column %2").arg(r).arg(c)));
                    QVERIFY(cellRect(other).contains(glyph));
                    QVERIFY(cut(with, glyph) != cut(base, glyph));
                    continue;
                }
                QVERIFY2(glyph.isNull(), qPrintable(QStringLiteral("row %1 column %2").arg(r).arg(c)));
                if (!tree()->isColumnHidden(c) && !cells()->showsPending(other)) {
                    QVERIFY2(cut(with, cellRect(other)) == cut(base, cellRect(other)),
                             qPrintable(QStringLiteral("row %1 column %2").arg(r).arg(c)));
                }
            }
        }
    }

    std::unique_ptr<JobWorld> m_world;
    std::unique_ptr<PlotFixture> m_fixture;
    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<JobQueue> m_queue;
    std::unique_ptr<PlotModel> m_plots;
    std::unique_ptr<CalculationDemand> m_demand;
    std::unique_ptr<QWidget> m_window;
    LogbookView *m_logbook = nullptr;          // children of the window
    QTreeView *m_reference = nullptr;
    QStringList m_registryBefore;
};

void LogbookIndicatorsTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();

    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);
    LogbookColumnStore::instance().setColumns({descriptionColumn()});
}

// Four loaded, hidden, saved and indexed sessions "s1".."s4" named "Jump 1"..
// "Jump 4"; G_IN is 1, 2 and 4 on s1, s2 and s4 (s3 has none: not applicable).
// So G_OUT is 2, 3 and 5. No plot is checked; the rows are in row order s1..s4.
void LogbookIndicatorsTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
    m_registryBefore = CalculationRegistry::instance().registeredIds();

    m_world = std::make_unique<JobWorld>();
    m_fixture = std::make_unique<PlotFixture>();
    m_model = std::make_unique<SessionModel>();
    m_model->mergeSessions(JobWorld::sessions({"s1", "s2", "s3", "s4"}));
    QCOMPARE(m_model->rowCount(), 4);
    QVERIFY(waitForIdle(*m_model));
    for (int i = 1; i <= 4; ++i) {
        m_model->updateAttribute(QStringLiteral("s%1").arg(i), QString::fromLatin1(SessionKeys::Description),
                                 QStringLiteral("Jump %1").arg(i));
    }
    for (const auto &[id, value] : {std::pair{"s1", 1.0}, std::pair{"s2", 2.0}, std::pair{"s4", 4.0}})
        QVERIFY(PlotFixture::giveInput(*m_model, QString::fromLatin1(id), QStringLiteral("G_IN"), value));
    m_model->flushPendingInvalidations();
    QVERIFY(waitForIdle(*m_model));

    m_plots = std::make_unique<PlotModel>();
    m_plots->setPlots(PlotFixture::plots());
    QVERIFY(buildServices());
}

bool LogbookIndicatorsTest::buildServices()
{
    m_queue = std::make_unique<JobQueue>(m_model.get());
    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
    return buildUi();
}

bool LogbookIndicatorsTest::buildUi()
{
    m_window = std::make_unique<QWidget>();
    auto *layout = new QHBoxLayout(m_window.get());
    m_logbook = new LogbookView(m_model.get(), m_demand.get(), m_window.get());
    m_logbook->setFixedWidth(460);
    layout->addWidget(m_logbook);

    // As LogbookView::setupView() configures its tree, with the base delegate
    m_reference = new QTreeView(m_window.get());
    m_reference->setModel(m_model.get());
    m_reference->setRootIsDecorated(false);
    m_reference->header()->setDefaultSectionSize(100);
    m_reference->header()->setFixedHeight(QFontMetrics(m_reference->header()->font()).height() * 2 + 8);
    m_reference->setUniformRowHeights(true);
    m_reference->setSortingEnabled(true);
    m_reference->setFixedWidth(460);
    layout->addWidget(m_reference);

    m_window->resize(960, 320);
    m_window->show();
    if (!QTest::qWaitForWindowExposed(m_window.get()))
        return false;
    // The same size as the logbook's tree, so that the headers compare
    m_reference->setFixedSize(tree()->size());
    // Both trees sorted by name ascending, so that row order is s1..s4
    tree()->sortByColumn(descriptionSection(), Qt::AscendingOrder);
    m_reference->sortByColumn(descriptionSection(), Qt::AscendingOrder);

    spin();
    return cells() && m_model->rowAt(0).sessionId == QStringLiteral("s1");
}

void LogbookIndicatorsTest::destroyUi()
{
    QToolTip::hideText();
    m_logbook = nullptr;
    m_reference = nullptr;
    m_window.reset();
}

// Note what is to be checked, tear everything down, and only then check: a
// failed check returns at once, and must not leave the next test a live
// world. The views go before the demand layer.
void LogbookIndicatorsTest::cleanup()
{
    if (m_world)
        gate().open(16);
    bool becameIdle = true;
    if (m_queue) {
        becameIdle = m_demand ? waitDemandIdle() : waitIdle(*m_queue);
        m_queue->shutdown();
    }

    destroyUi();
    m_demand.reset();
    QStringList stillPinned;
    if (m_model) {
        for (const char *id : {"s1", "s2", "s3", "s4"}) {
            if (m_model->isSessionPinned(QString::fromLatin1(id)))
                stillPinned.append(QString::fromLatin1(id));
        }
    }
    m_plots.reset();
    m_queue.reset();
    m_model.reset();
    LogbookColumnStore::instance().setColumns({descriptionColumn()});
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 50);

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
    QCOMPARE(afterFixture, withoutFixture);
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

// ---- Plain -----------------------------------------------------------------------------

// Without a failure the logbook is the reference tree: its header, with no
// column over a requested calculation, while one works and once it has
// finished; its cells the base delegate's outside the pending ones; its sizes.
void LogbookIndicatorsTest::plainHeaderAndCellsAreIdenticalToBase()
{
    enableColumns({QStringLiteral("G_IN")});
    spin();
    QCOMPARE(m_model->columnCount(), 2);
    QCOMPARE(grabHeader(), grabReferenceHeader());
    QCOMPARE(grabCells(), grabCellsWithBaseDelegate());
    for (int l = 0; l < header()->count(); ++l)
        QCOMPARE(header()->sectionSizeHint(l), m_reference->header()->sectionSizeHint(l));
    verifySizesAreTheBase();
    if (QTest::currentTestFailed())
        return;

    // Working: only the pending cells differ
    QVERIFY(makeWorkingColumn());
    QVERIFY(m_demand->failures().isEmpty());
    QCOMPARE(grabHeader(), grabReferenceHeader());
    for (int l = 0; l < header()->count(); ++l)
        QCOMPARE(header()->sectionSizeHint(l), m_reference->header()->sectionSizeHint(l));
    {
        const QImage with = grabCells();
        const QImage base = grabCellsWithBaseDelegate();
        int pending = 0;
        for (int r = 0; r < m_model->rowCount(); ++r) {
            for (int c = 0; c < m_model->columnCount(); ++c) {
                const QModelIndex index = m_model->index(r, c);
                QCOMPARE(cells()->warningRect(index), QRect());
                if (cells()->showsPending(index))
                    ++pending;
                else
                    QCOMPARE(cut(with, cellRect(index)), cut(base, cellRect(index)));
            }
        }
        QCOMPARE(pending, 3);
    }
    verifySizesAreTheBase();
    if (QTest::currentTestFailed())
        return;

    // Finished without a failure: nothing differs
    gate().open(16);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    spin();
    QVERIFY(m_demand->failures().isEmpty());
    QCOMPARE(grabHeader(), grabReferenceHeader());
    QCOMPARE(grabCells(), grabCellsWithBaseDelegate());
    verifySizesAreTheBase();
}

// Spec 9: the header is the tree's own and shows nothing of the demand layer,
// while a column works and after one has failed; nothing repaints by itself
// while a column works; the view has no progress line and no cancel button.
void LogbookIndicatorsTest::headerIsPlainAndNothingAnimates()
{
    QCOMPARE(header()->metaObject(), &QHeaderView::staticMetaObject);
    QVERIFY(m_logbook->findChildren<QProgressBar *>().isEmpty());
    QVERIFY(m_logbook->findChildren<QToolButton *>().isEmpty());

    // Working
    QVERIFY(makeWorkingColumn());
    QCOMPARE(grabHeader(), grabReferenceHeader());
    for (int l = 0; l < header()->count(); ++l)
        QCOMPARE(header()->sectionSizeHint(l), m_reference->header()->sectionSizeHint(l));

    // No clock: neither the header nor the cells repaint while the job runs.
    // The scheduler rests first (every row is loaded, so the fill has nothing
    // to step and waits on the job), so no tick of the column worker paints
    // inside the window.
    spin();
    QTRY_VERIFY(!m_model->scheduler().isTicking());
    const auto *headerPaints = new PaintCounter(header()->viewport());
    const auto *cellPaints = new PaintCounter(tree()->viewport());
    QTest::qWait(400);
    QCOMPARE(headerPaints->count, 0);
    QCOMPARE(cellPaints->count, 0);
    QVERIFY(gate().running.load() == 1);    // still working

    // Failed
    gate().open(16);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(makeFailedRow());
    QCOMPARE(grabHeader(), grabReferenceHeader());
    for (int l = 0; l < header()->count(); ++l)
        QCOMPARE(header()->sectionSizeHint(l), m_reference->header()->sectionSizeHint(l));
    QCOMPARE(header()->metaObject(), &QHeaderView::staticMetaObject);
    QVERIFY(m_logbook->findChildren<QProgressBar *>().isEmpty());
    QVERIFY(m_logbook->findChildren<QToolButton *>().isEmpty());
}

// ---- The row warning -------------------------------------------------------------------

// Spec 8: one glyph, the style's warning icon, right after the text of the
// failed row's first visual cell, attached to it rather than pinned to the
// cell's edge, with the text itself where the base delegate puts it; every
// other cell, the failed calculation's blank cell included, is the base
// delegate's, and no size changes. The same for a row that is not loaded,
// which stays unloaded.
void LogbookIndicatorsTest::rowWarningFollowsTheText()
{
    QVERIFY(makeFailedRow());
    // Room for the text and the glyph: the glyph follows the text only when
    // the text fits; in a narrow cell the text is elided to make room for it
    tree()->setColumnWidth(descriptionSection(), 200);
    spin();
    QVERIFY(!cells()->showsPending(cell("s1", "EA1")));
    QVERIFY(cell("s1", "EA1").data().toString().isEmpty());

    const auto verifyGlyph = [this] {
        const QModelIndex first = firstCell("s1");
        verifyOnlyGlyphAt(first);
        if (QTest::currentTestFailed())
            return;
        const QRect glyph = cells()->warningRect(first);
        const QFontMetrics metrics = tree()->fontMetrics();     // the view's, device-aware, as the delegate's
        QVERIFY(glyph.height() <= metrics.height());
        QVERIFY(cellRect(first).contains(glyph));

        // Where the style puts the check box and the text without the glyph
        QStyleOptionViewItem opt;
        opt.initFrom(tree());
        opt.widget = tree();
        opt.rect = cellRect(first);
        opt.features = QStyleOptionViewItem::HasCheckIndicator | QStyleOptionViewItem::HasDisplay;
        opt.text = first.data().toString();
        QStyle *style = tree()->style();
        const QRect check = style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, tree());
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, tree());
        const int textStart = textRect.left() + style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, tree()) + 1;
        const int textEnd = textStart + metrics.horizontalAdvance(opt.text);
        QVERIFY(!opt.text.isEmpty());
        QVERIFY2(textEnd < glyph.left(),
                 qPrintable(QStringLiteral("text '%1' start %2 end %3, textRect %4-%5, glyph %6-%7, cell %8-%9")
                                .arg(opt.text).arg(textStart).arg(textEnd).arg(textRect.left()).arg(textRect.right())
                                .arg(glyph.left()).arg(glyph.right()).arg(cellRect(first).left()).arg(cellRect(first).right())));

        // After the text, and attached to it: nearer than its own width
        QVERIFY(glyph.left() > check.right());
        QVERIFY(glyph.left() >= textEnd);
        QVERIFY(glyph.left() - textEnd < glyph.width());

        // The text has not moved: up to the glyph, the cell is the base
        // delegate's, check box and text included
        const QRect cell = cellRect(first);
        const QRect upToGlyph(cell.left(), cell.top(), glyph.left() - cell.left(), cell.height());
        QVERIFY(cut(grabCells(), upToGlyph) == cut(grabCellsWithBaseDelegate(), upToGlyph));

        verifySizesAreTheBase();
    };

    // Loaded
    QVERIFY(m_model->rowAt(row("s1")).isLoaded());
    verifyGlyph();
    if (QTest::currentTestFailed())
        return;

    // Not loaded: the record set says so, and nothing loads the row
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
    QVERIFY(waitForIdle(*m_model));
    for (const char *id : {"s1", "s2", "s3", "s4"})
        QVERIFY2(!m_model->rowAt(row(id)).isLoaded(), id);
    QVERIFY(waitDemandIdle());
    spin();
    QVERIFY(failed("s1"));
    QVERIFY(!m_model->rowAt(row("s1")).isLoaded());
    verifyGlyph();
    QVERIFY(!m_model->rowAt(row("s1")).isLoaded());
}

// Spec 8: the hover over the glyph is the recording's failures, exactly;
// elsewhere in the cell the cell's own tooltip applies.
void LogbookIndicatorsTest::rowWarningHoverIsTheSessionsFailures()
{
    // s1 fails two calculations, a stored rejection and a computation that
    // throws, before its G_OUT runs and holds the gate
    QVERIFY(PlotFixture::giveInput(*m_model, QStringLiteral("s1"), QStringLiteral("EA_IN"), -1));
    QVERIFY(PlotFixture::giveInput(*m_model, QStringLiteral("s1"), QStringLiteral("T_IN"), 1));
    m_model->flushPendingInvalidations();
    enableColumns({QStringLiteral("EA1"), QStringLiteral("T_OUT"), QStringLiteral("G_OUT")});
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(m_demand->sessionFailures(QStringLiteral("s1")).calculations.size(), 2);
    spin();
    const QString failures = m_demand->sessionFailures(QStringLiteral("s1")).text();
    QCOMPARE(failures, QStringLiteral("Explicit A: negative input\n"
                                      "Thrower: synthetic failure (tried again at the next start)"));

    // Over the glyph: the failures
    const QModelIndex first = firstCell("s1");
    const QRect glyph = cells()->warningRect(first);
    QVERIFY(!glyph.isNull());
    QVERIFY(hideToolTip());
    QVERIFY(cellHelp(first, glyph.center()));
    QCOMPARE(QToolTip::text(), failures);

    // Elsewhere in the description cell: its own tooltip, which is none
    QVERIFY(hideToolTip());
    QVERIFY(!cellHelp(first, trailingPoint(first)));

    // A row without a failure accepts nothing where the glyph would be
    const QModelIndex s2First = firstCell("s2");
    QVERIFY(hideToolTip());
    QVERIFY(!cellHelp(s2First, QPoint(glyph.center().x(), cellRect(s2First).center().y())));
    spin();
    QVERIFY(!QToolTip::isVisible());

    // A pending first cell: G_OUT at the front, s1's still running. The glyph
    // shows the failures, the rest of the cell the pending tooltip.
    const int g = section("G_OUT");
    header()->moveSection(header()->visualIndex(g), 0);
    spin();
    const QModelIndex pendingFirst = cell("s1", "G_OUT");
    QVERIFY(cells()->showsPending(pendingFirst));
    const QRect pendingGlyph = cells()->warningRect(pendingFirst);
    QVERIFY(!pendingGlyph.isNull());
    QCOMPARE(cells()->warningRect(first), QRect());
    QVERIFY(hideToolTip());
    QVERIFY(cellHelp(pendingFirst, pendingGlyph.center()));
    QCOMPARE(QToolTip::text(), failures);
    QVERIFY(hideToolTip());
    QVERIFY(cellHelp(pendingFirst, trailingPoint(pendingFirst)));
    QCOMPARE(QToolTip::text(), LogbookCellDelegate::pendingToolTip());

    // s2's pending first cell has no glyph: the pending tooltip throughout
    const QModelIndex s2Pending = cell("s2", "G_OUT");
    QVERIFY(cells()->showsPending(s2Pending));
    QVERIFY(hideToolTip());
    QVERIFY(cellHelp(s2Pending, QPoint(pendingGlyph.center().x(), cellRect(s2Pending).center().y())));
    QCOMPARE(QToolTip::text(), LogbookCellDelegate::pendingToolTip());
    QVERIFY(hideToolTip());
}

// Spec 8: the glyph belongs to the row, in whatever cell is first: moving a
// section to the front, hiding the first section, sorting and rebuilding the
// columns put it in the new first visual cell, and only there.
void LogbookIndicatorsTest::rowWarningFollowsTheFirstVisualColumn()
{
    QVERIFY(makeFailedRow({QStringLiteral("G_IN"), QStringLiteral("EA1")}));
    const int d = descriptionSection();
    const int ea = section("EA1");
    QCOMPARE(d, 0);
    verifyOnlyGlyphAt(firstCell("s1"));
    if (QTest::currentTestFailed())
        return;

    // EA1 moved to the front
    header()->moveSection(header()->visualIndex(ea), 0);
    spin();
    verifyOnlyGlyphAt(cell("s1", "EA1"));
    if (QTest::currentTestFailed())
        return;

    // The first section hidden: the next in visual order, then the one after
    header()->hideSection(ea);
    spin();
    verifyOnlyGlyphAt(firstCell("s1"));
    if (QTest::currentTestFailed())
        return;
    header()->hideSection(d);
    spin();
    verifyOnlyGlyphAt(cell("s1", "G_IN"));
    if (QTest::currentTestFailed())
        return;
    header()->showSection(d);
    header()->showSection(ea);
    spin();
    verifyOnlyGlyphAt(cell("s1", "EA1"));
    if (QTest::currentTestFailed())
        return;

    // Sorted: the glyph goes with the row
    tree()->sortByColumn(d, Qt::DescendingOrder);
    spin();
    QCOMPARE(row("s1"), 3);
    verifyOnlyGlyphAt(cell("s1", "EA1"));
    if (QTest::currentTestFailed())
        return;

    // Rebuilt columns (a reset), in the logical order again: EA1 first, as a
    // new logical index
    header()->moveSection(header()->visualIndex(ea), ea);
    spin();
    QCOMPARE(header()->logicalIndex(0), d);
    LogbookColumnStore::instance().setColumns({attributeColumn(QStringLiteral("EA1")), descriptionColumn(),
                                               attributeColumn(QStringLiteral("G_IN"))});
    QVERIFY(waitForIdle(*m_model));
    spin();
    QCOMPARE(section("EA1"), 0);
    QCOMPARE(header()->logicalIndex(0), 0);
    QVERIFY(failed("s1"));
    verifyOnlyGlyphAt(cell("s1", "EA1"));
}

// Spec 7 and 8: the glyph appears when a failure becomes current and goes
// when it clears: a successful retry after an input change, and the last
// source over the calculation disabled.
void LogbookIndicatorsTest::rowWarningFollowsFailures()
{
    const auto shown = [this] { return !cells()->warningRect(firstCell("s1")).isNull(); };
    const auto painted = [this] {
        const QRect rect = cellRect(firstCell("s1"));
        return cut(grabCells(), rect) != cut(grabCellsWithBaseDelegate(), rect);
    };

    // s1's job runs out of memory once: the glyph appears when it fails
    QVERIFY(PlotFixture::giveInput(*m_model, QStringLiteral("s1"), QStringLiteral("X_IN"), 1));
    m_model->flushPendingInvalidations();
    QVERIFY(!shown());
    enableColumns({QStringLiteral("X_OUT")});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    spin();
    QVERIFY(failed("s1"));
    QVERIFY(shown());
    QVERIFY(painted());
    verifyOnlyGlyphAt(firstCell("s1"));
    if (QTest::currentTestFailed())
        return;

    // An input change: the retry succeeds, and the glyph goes
    QVERIFY(PlotFixture::giveInput(*m_model, QStringLiteral("s1"), QStringLiteral("X_IN"), 2));
    m_model->flushPendingInvalidations();
    QVERIFY(settleAndWait());
    spin();
    QTRY_COMPARE(cell("s1", "X_OUT").data().toString(), QStringLiteral("3"));
    QVERIFY(!failed("s1"));
    QVERIFY(!shown());
    QVERIFY(!painted());

    // A failure again, then the last source over its calculation disabled
    QVERIFY(PlotFixture::giveInput(*m_model, QStringLiteral("s1"), QStringLiteral("EA_IN"), -1));
    m_model->flushPendingInvalidations();
    enableColumns({QStringLiteral("X_OUT"), QStringLiteral("EA1")});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    spin();
    QVERIFY(failed("s1"));
    QVERIFY(shown());
    QVERIFY(painted());
    enableColumns({QStringLiteral("X_OUT")});
    spin();
    QVERIFY(!failed("s1"));
    QVERIFY(!shown());
    QVERIFY(!painted());
}

// Presentation half of tst_calculation_demand's
// visibleFailedLoadIsSettledAsFailed: a visible session whose file cannot be
// loaded is failed, never pending, and its row warns.
void LogbookIndicatorsTest::failedLoadSessionShowsRowWarningNotPending()
{
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
    QVERIFY(waitForIdle(*m_model));
    for (const char *id : {"s1", "s2", "s3", "s4"})
        QVERIFY2(!m_model->rowAt(row(id)).isLoaded(), id);
    QVERIFY(QFile::remove(sessionFilePath(QStringLiteral("s2"))));
    PlotFixture::show(*m_model, {"s2"});
    const SessionRow &s2 = m_model->rowAt(row("s2"));
    QVERIFY(s2.isLoaded());
    QVERIFY(s2.loadFailed);
    QVERIFY(s2.visible);

    gate().open(16);
    enableColumns({QStringLiteral("G_OUT")});
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    spin();

    const QModelIndex s2Cell = cell("s2", "G_OUT");
    QVERIFY(!cells()->showsPending(s2Cell));
    QCOMPARE(cut(grabCells(), cellRect(s2Cell)), cut(grabCellsWithBaseDelegate(), cellRect(s2Cell)));

    QVERIFY(failed("s2"));
    const QModelIndex first = firstCell("s2");
    verifyOnlyGlyphAt(first);
    if (QTest::currentTestFailed())
        return;
    const QString text = m_demand->sessionFailures(QStringLiteral("s2")).text();
    QVERIFY2(text.contains(QStringLiteral("The session file could not be loaded")), qPrintable(text));
    QVERIFY(hideToolTip());
    QVERIFY(cellHelp(first, cells()->warningRect(first).center()));
    QCOMPARE(QToolTip::text(), text);
    QVERIFY(hideToolTip());

    // Settled: nothing repaints the first column any more
    const auto *paints = new PaintCounter(tree()->viewport(), columnRect(descriptionSection()));
    for (int i = 0; i < 3; ++i)
        spin();
    QCOMPARE(paints->count, 0);
}

// Spec 12, "A stored result that could not be written" (the presentation
// half; the demand side is tst_calculation_demand's): the row warns, and its
// hover names the calculation with the store's reason, tried again at the
// next start.
void LogbookIndicatorsTest::failedWriteIsListedInTheHover()
{
    // A directory where s1's record would be written
    const QString path = TestEnvironment::instance().cacheDir() + QLatin1Char('/')
        + recordFileName(sessionFileStem(QStringLiteral("s1")), QStringLiteral("gated"));
    QVERIFY(QDir().mkpath(path));
    const auto removeDirectory = qScopeGuard([path] { QDir(path).removeRecursively(); });

    enableColumns({QStringLiteral("G_OUT")});
    gate().open(16);
    {
        WarningCapture warnings;            // the store logs "not written"
        QVERIFY(waitDemandIdle());
        QVERIFY(waitForIdle(*m_model));
    }
    spin();

    // s2 and s4 were written, s3 is not applicable
    QCOMPARE(m_demand->failures().size(), 1);
    QCOMPARE(m_demand->sessionFailures(QStringLiteral("s1")).calculations.size(), 1);
    const QString text = m_demand->sessionFailures(QStringLiteral("s1")).text();
    QVERIFY2(text.contains(QStringLiteral("Gated: Couldn't write file")), qPrintable(text));
    QVERIFY2(text.endsWith(QStringLiteral("(tried again at the next start)")), qPrintable(text));

    const QModelIndex first = firstCell("s1");
    verifyOnlyGlyphAt(first);
    if (QTest::currentTestFailed())
        return;
    QVERIFY(hideToolTip());
    QVERIFY(cellHelp(first, cells()->warningRect(first).center()));
    QCOMPARE(QToolTip::text(), text);
    QVERIFY(hideToolTip());
}

// No gesture: a click on the glyph selects the row as a click elsewhere in
// the cell does, toggles no check box, and starts or cancels nothing.
void LogbookIndicatorsTest::clickOnRowWarningIsAClickOnTheCell()
{
    QVERIFY(makeFailedRow());
    const QModelIndex first = firstCell("s1");
    const QRect glyph = cells()->warningRect(first);
    QVERIFY(!glyph.isNull());
    const bool visible = m_model->rowAt(first.row()).visible;

    const Quiet quiet(*m_queue);
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);
    QItemSelectionModel *selection = tree()->selectionModel();

    QTest::mouseClick(tree()->viewport(), Qt::LeftButton, {}, glyph.center());
    spin();
    QCOMPARE(tree()->currentIndex(), first);
    QVERIFY(selection->isRowSelected(first.row(), QModelIndex()));
    const QModelIndexList afterGlyph = selection->selectedRows();
    QCOMPARE(afterGlyph.size(), 1);

    // Another row, then s1's cell away from the glyph: the same selection.
    // (s1 is not selected when it is clicked again, so no edit starts.)
    const QModelIndex s2First = firstCell("s2");
    QTest::mouseClick(tree()->viewport(), Qt::LeftButton, {}, trailingPoint(s2First));
    spin();
    QVERIFY(selection->isRowSelected(s2First.row(), QModelIndex()));
    QTest::mouseClick(tree()->viewport(), Qt::LeftButton, {}, trailingPoint(first));
    spin();
    QCOMPARE(tree()->currentIndex(), first);
    QCOMPARE(selection->selectedRows(), afterGlyph);

    QCOMPARE(m_model->rowAt(first.row()).visible, visible);
    QVERIFY(!tree()->findChild<QLineEdit *>());
    QVERIFY(quiet.holds());
    QCOMPARE(cancelSpy.count(), 0);
    QVERIFY(failed("s1"));
    QVERIFY(!cells()->warningRect(first).isNull());
}

// ---- The cells ------------------------------------------------------------------------

// Spec 10: "not yet" reads differently from "never", and pending is nowhere
// but in the view.
void LogbookIndicatorsTest::pendingCellsAreDistinctFromUnavailable()
{
    QVERIFY(makeWorkingColumn());
    QCOMPARE(runningSession(), QStringLiteral("s1"));
    const int g = section("G_OUT");
    const int d = descriptionSection();

    for (const char *id : {"s1", "s2", "s4"})
        QVERIFY2(cells()->showsPending(cell(id, "G_OUT")), id);
    QVERIFY(!cells()->showsPending(cell("s3", "G_OUT")));
    for (int r = 0; r < m_model->rowCount(); ++r)
        QVERIFY(!cells()->showsPending(m_model->index(r, d)));

    const QImage with = grabCells();
    const QImage base = grabCellsWithBaseDelegate();
    QVERIFY(cut(with, cellRect(cell("s2", "G_OUT"))) != cut(base, cellRect(cell("s2", "G_OUT"))));
    QCOMPARE(cut(with, cellRect(cell("s3", "G_OUT"))), cut(base, cellRect(cell("s3", "G_OUT"))));
    for (int r = 0; r < m_model->rowCount(); ++r) {
        const QRect rect = cellRect(m_model->index(r, d));
        QCOMPARE(cut(with, rect), cut(base, rect));
    }

    // Nothing of it in the model, its cached values or the index
    const LogbookColumn gColumn = attributeColumn(QStringLiteral("G_OUT"));
    for (const char *id : {"s1", "s2", "s3", "s4"}) {
        const QModelIndex index = cell(id, "G_OUT");
        QVERIFY2(!index.data(Qt::DisplayRole).isValid(), id);
        QVERIFY2(!index.data(Qt::ToolTipRole).isValid(), id);
        const SessionRow &row = m_model->rowAt(index.row());
        QVERIFY2(!row.cachedValues.value(g).isValid(), id);
        QVERIFY2(!row.pendingColumns.contains(g), id);
        const QJsonValue value = flushedIndexValue(id);
        QVERIFY2(value.isNull() || value.isUndefined(), id);
    }
    QVERIFY(!indexBytes().contains(LogbookCellDelegate::pendingText().toUtf8()));
    QVERIFY(!QJsonDocument(readIndex()).toJson().contains(LogbookCellDelegate::pendingText().toUtf8()));

    // Its own tooltip; an unavailable cell has none
    QVERIFY(hideToolTip());
    QVERIFY(cellHelp(cell("s2", "G_OUT")));
    QCOMPARE(QToolTip::text(), LogbookCellDelegate::pendingToolTip());
    QVERIFY(hideToolTip());
    QVERIFY(!cellHelp(cell("s3", "G_OUT")));
}

void LogbookIndicatorsTest::pendingCellBecomesValueWhenRecordIsWritten_data()
{
    QTest::addColumn<bool>("stubs");
    QTest::newRow("loaded") << false;
    QTest::newRow("stub") << true;
}

// Spec 13: the pending cell becomes the value when the record is written, and
// nothing of it reaches index.json.
void LogbookIndicatorsTest::pendingCellBecomesValueWhenRecordIsWritten()
{
    QFETCH(bool, stubs);
    if (stubs) {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
        QVERIFY(waitForIdle(*m_model));
        for (const char *id : {"s1", "s2", "s3", "s4"})
            QVERIFY2(!m_model->rowAt(m_model->getSessionRow(QString::fromLatin1(id))).isLoaded(), id);
    }

    QSignalSpy resetSpy(m_model.get(), &QAbstractItemModel::modelAboutToBeReset);
    QStringList indexSnapshots;
    const auto noteIndex = [&indexSnapshots] {
        LogbookManager::instance().flushIndex();
        indexSnapshots.append(QString::fromUtf8(indexBytes()));
    };

    enableColumns({QStringLiteral("G_OUT")});
    resetSpy.clear();                       // the column change itself resets the model
    QVERIFY(gate().waitEntered());          // s1
    QTRY_VERIFY(cells()->showsPending(cell("s2", "G_OUT")));
    QVERIFY(cells()->showsPending(cell("s1", "G_OUT")));
    noteIndex();

    // s1's record is written: its cell becomes the value
    gate().open(1);
    QTRY_VERIFY(!cells()->showsPending(cell("s1", "G_OUT")));
    QTRY_COMPARE(cell("s1", "G_OUT").data().toString(), QStringLiteral("2"));
    QVERIFY(stored("s1", "gated"));
    QTRY_COMPARE(flushedIndexValue("s1").toDouble(), 2.0);
    QVERIFY(cells()->showsPending(cell("s2", "G_OUT")));
    noteIndex();

    gate().open(16);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    spin();
    for (int r = 0; r < m_model->rowCount(); ++r) {
        for (int c = 0; c < m_model->columnCount(); ++c)
            QVERIFY(!cells()->showsPending(m_model->index(r, c)));
    }
    QCOMPARE(cell("s1", "G_OUT").data().toString(), QStringLiteral("2"));
    QCOMPARE(cell("s2", "G_OUT").data().toString(), QStringLiteral("3"));
    QVERIFY(cell("s3", "G_OUT").data().toString().isEmpty());
    QCOMPARE(cell("s4", "G_OUT").data().toString(), QStringLiteral("5"));

    m_model->flushDirtySessions();          // the index as the model knows it
    noteIndex();
    const QJsonObject index = readIndex();
    const LogbookColumn gColumn = attributeColumn(QStringLiteral("G_OUT"));
    QCOMPARE(indexValue(index, QStringLiteral("s1"), gColumn), QJsonValue(2.0));
    QCOMPARE(indexValue(index, QStringLiteral("s2"), gColumn), QJsonValue(3.0));
    QCOMPARE(indexValue(index, QStringLiteral("s4"), gColumn), QJsonValue(5.0));
    const QJsonValue s3 = indexValue(index, QStringLiteral("s3"), gColumn);
    QVERIFY(s3.isNull() || s3.isUndefined());
    for (const QString &snapshot : std::as_const(indexSnapshots))
        QVERIFY(!snapshot.contains(LogbookCellDelegate::pendingText()));

    // A loaded session's value arrives without a reset of the model
    if (!stubs)
        QCOMPARE(resetSpy.count(), 0);
}

// Sorting treats a pending cell as unavailable: missing values go to the
// bottom both ways, and the index does not change.
void LogbookIndicatorsTest::sortingTreatsPendingAsUnavailable()
{
    QVERIFY(makeWorkingColumn());
    gate().open(1);                         // s1 computed; s2 running, s4 waiting
    QVERIFY(gate().waitEntered());
    QTRY_COMPARE(cell("s1", "G_OUT").data().toString(), QStringLiteral("2"));
    spin();
    const int g = section("G_OUT");
    const auto indexValues = [] {
        QStringList values;
        for (const char *id : {"s1", "s2", "s3", "s4"})
            values.append(QString::fromUtf8(QJsonDocument(QJsonObject{{"v", flushedIndexValue(id)}}).toJson()));
        return values;
    };
    const QStringList before = indexValues();

    const auto verifyOrder = [this] {
        QCOMPARE(m_model->rowAt(0).sessionId, QStringLiteral("s1"));
        for (int r = 0; r < m_model->rowCount(); ++r) {
            const QString id = m_model->rowAt(r).sessionId;
            const bool pending = id == QStringLiteral("s2") || id == QStringLiteral("s4");
            QVERIFY2(cells()->showsPending(m_model->index(r, section("G_OUT"))) == pending, qPrintable(id));
        }
    };

    tree()->sortByColumn(g, Qt::AscendingOrder);
    spin();
    verifyOrder();
    if (QTest::currentTestFailed())
        return;
    tree()->sortByColumn(g, Qt::DescendingOrder);
    spin();
    verifyOrder();
    if (QTest::currentTestFailed())
        return;
    QCOMPARE(m_model->getSessionRow(QStringLiteral("s2")) > 0, true);
    QCOMPARE(indexValues(), before);
}

void LogbookIndicatorsTest::unreadableRecordPendingIsNotDemandPending_data()
{
    QTest::addColumn<int>("mechanism");
    QTest::newRow("locked without sharing") << int(UnreadableFile::Mechanism::LockedWithoutSharing);
    QTest::newRow("no read permission") << int(UnreadableFile::Mechanism::NoReadPermission);
}

// The row's own pending state (a record the column worker could not read)
// keeps today's empty look and is never demand-pending: its record is known.
void LogbookIndicatorsTest::unreadableRecordPendingIsNotDemandPending()
{
    QFETCH(int, mechanism);
    enableColumns({QStringLiteral("G_OUT")});
    gate().open(16);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    QVERIFY(stored("s1", "gated"));
    const QString path = TestEnvironment::instance().cacheDir() + QLatin1Char('/') + sessionFileStem(QStringLiteral("s1"))
        + QStringLiteral(".gated.fvresult");
    QVERIFY2(QFile::exists(path), qPrintable(path));

    // Everything down, as tst_result_columns::workerSkipsUnreadableRecord
    m_queue->shutdown();
    destroyUi();
    m_demand.reset();
    m_queue.reset();
    m_model.reset();
    {
        QJsonObject root = readIndex();
        const QString columnId = indexColumnId(root, attributeColumn(QStringLiteral("G_OUT")));
        QVERIFY(!columnId.isEmpty());
        QJsonObject sessions = root[QStringLiteral("sessions")].toObject();
        QJsonObject entry = sessions[QStringLiteral("s1")].toObject();
        QJsonObject values = entry[QStringLiteral("values")].toObject();
        values.remove(columnId);
        entry[QStringLiteral("values")] = values;
        sessions[QStringLiteral("s1")] = entry;
        root[QStringLiteral("sessions")] = sessions;
        QVERIFY(writeIndex(root));
    }
    UnreadableFile unreadable(path, UnreadableFile::Mechanism(mechanism));
    if (!unreadable.skipReason().isEmpty())
        QSKIP(qPrintable(unreadable.skipReason()));

    // The next start
    LogbookManager &logbook = LogbookManager::instance();
    TestEnvironment::instance().reopenLogbook();
    logbook.initialize();
    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    m_queue = std::make_unique<JobQueue>(m_model.get());
    const Quiet quiet(*m_queue);
    m_demand = std::make_unique<CalculationDemand>(m_model.get(), m_plots.get(), m_queue.get());
    QVERIFY(buildUi());

    {
        WarningCapture warnings;    // the skip warns
        m_model->startColumnWorker();
        QVERIFY(waitDemandIdle());
        QVERIFY(waitForIdle(*m_model));
    }
    spin();

    const int g = section("G_OUT");
    const QModelIndex s1 = cell("s1", "G_OUT");
    QVERIFY(m_model->rowAt(s1.row()).pendingColumns.contains(g));
    QVERIFY(!m_demand->isCellPending(s1.row(), g));
    QVERIFY(!m_demand->isCellPending(QStringLiteral("s1"), colId("G_OUT")));
    QVERIFY(!cells()->showsPending(s1));
    QVERIFY(s1.data().toString().isEmpty());
    QCOMPARE(cut(grabCells(), cellRect(s1)), cut(grabCellsWithBaseDelegate(), cellRect(s1)));
    QCOMPARE(progressNow().count, 0);
    QVERIFY(quiet.holds());
    QVERIFY(unreadable.release());
}

// Criterion 6 of the row warning and the pending cells: one repaint of the
// visible part of one column per announcement, and no model signal.
// pendingCellsChanged(id) repaints that column; failuresChanged() the first
// visual column, wherever it is.
void LogbookIndicatorsTest::pendingCellsChangeRepaintsOnlyThatColumn()
{
    enableColumns({QStringLiteral("G_OUT"), QStringLiteral("G_IN")});
    gate().open(16);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    spin();
    spin();
    const int g = section("G_OUT");

    auto *treeRegion = new RegionRecorder(tree()->viewport());
    auto *headerRegion = new RegionRecorder(header()->viewport());
    QSignalSpy dataSpy(m_model.get(), &QAbstractItemModel::dataChanged);
    QSignalSpy resetSpy(m_model.get(), &QAbstractItemModel::modelReset);
    QSignalSpy layoutSpy(m_model.get(), &QAbstractItemModel::layoutChanged);
    QSignalSpy headerDataSpy(m_model.get(), &QAbstractItemModel::headerDataChanged);

    emit m_demand->pendingCellsChanged(colId("G_OUT"));
    QTRY_VERIFY(!treeRegion->region.isEmpty());
    QVERIFY((treeRegion->region - QRegion(columnRect(g))).isEmpty());
    spin();
    QVERIFY(headerRegion->region.isEmpty());

    // The first visual column, the description ...
    treeRegion->region = QRegion();
    emit m_demand->failuresChanged();
    QTRY_VERIFY(!treeRegion->region.isEmpty());
    QVERIFY((treeRegion->region - QRegion(columnRect(descriptionSection()))).isEmpty());

    // ... or whatever is first
    header()->moveSection(header()->visualIndex(section("G_IN")), 0);
    spin();
    spin();
    treeRegion->region = QRegion();
    emit m_demand->failuresChanged();
    QTRY_VERIFY(!treeRegion->region.isEmpty());
    QVERIFY((treeRegion->region - QRegion(columnRect(section("G_IN")))).isEmpty());

    QCOMPARE(dataSpy.count(), 0);
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(layoutSpy.count(), 0);
    QCOMPARE(headerDataSpy.count(), 0);

    // An unknown id is harmless
    emit m_demand->pendingCellsChanged(QStringLiteral("no such column"));
    spin();
}

// MainWindow deletes the demand layer before the docks: with a row warning
// and pending cells shown, the viewport is repainted, the cells then are the
// base delegate's, no failure tooltip remains, and sorting works.
void LogbookIndicatorsTest::survivesDemandDestroyedFirst()
{
    // s1 failed (a stored rejection) before its G_OUT runs and holds the gate
    QVERIFY(PlotFixture::giveInput(*m_model, QStringLiteral("s1"), QStringLiteral("EA_IN"), -1));
    m_model->flushPendingInvalidations();
    enableColumns({QStringLiteral("EA1"), QStringLiteral("G_OUT")});
    QVERIFY(gate().waitEntered());
    QTRY_VERIFY(failed("s1") && cells()->showsPending(cell("s2", "G_OUT")));
    spin();
    const int g = section("G_OUT");
    const QModelIndex first = firstCell("s1");
    const QRect glyph = cells()->warningRect(first);
    QVERIFY(!glyph.isNull());
    QVERIFY(cells()->showsPending(cell("s1", "G_OUT")));

    auto *repainted = new RegionRecorder(tree()->viewport());
    m_demand.reset();
    QTRY_VERIFY(!repainted->region.isEmpty());
    spin();

    QCOMPARE(grabHeader(), grabReferenceHeader());
    QCOMPARE(grabCells(), grabCellsWithBaseDelegate());
    for (int r = 0; r < m_model->rowCount(); ++r) {
        for (int c = 0; c < m_model->columnCount(); ++c) {
            QCOMPARE(cells()->warningRect(m_model->index(r, c)), QRect());
            QVERIFY(!cells()->showsPending(m_model->index(r, c)));
        }
    }

    QVERIFY(hideToolTip());
    QVERIFY(!cellHelp(first, glyph.center()));
    QVERIFY(!cellHelp(cell("s2", "G_OUT")));
    spin();
    QVERIFY(!QToolTip::isVisible());

    tree()->sortByColumn(g, Qt::AscendingOrder);
    spin();
    QCOMPARE(header()->sortIndicatorSection(), g);
}

// A Widgets test writes its own main() (tests/README.md section 8): the same
// order as FLYSIGHT_TEST_MAIN, with a QApplication and the application's style.
int main(int argc, char **argv)
{
    QHashSeed::setDeterministicGlobalSeed();
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    FlySightTest::TestEnvironment env(QStringLiteral("LogbookIndicatorsTest"));
    LogbookIndicatorsTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_logbook_indicators.moc"
