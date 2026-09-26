// The logbook's column headers and cells over the demand layer, in a real
// LogbookView (LogbookHeaderView, LogbookCellDelegate) over a real
// SessionModel, executor (JobQueue) and CalculationDemand, with the synthetic
// calculations of jobfixture.h and the plots of plotfixture.h, offscreen.
// Beside it, in the same window, a reference QTreeView configured as the
// logbook's tree but with QTreeView's own header, over the same model: what
// the logbook looked like before it presented demand.
//
// What is proved here is what the view owns: plain sections and cells are the
// base classes', a working column's header shows the turning indicator at the
// right of its text and a finished column with failures the badge, the hover
// detail, the clock that runs only while a column works, that a click on the
// glyph is a click on the section, and cells that read pending (distinct from
// unavailable and from the row's unreadable-record pending state) without a
// trace in the model, its cached values or index.json. State, counts and what
// is computed are CalculationDemand's (tst_calculation_demand).
//
// Synchronization: Gate::waitEntered() proves the worker is inside a compute
// function; QTRY_*, waitDemandIdle() and waitForIdle() spin the event loop;
// CalculationDemand::flush() runs a pending pass before a state is read. The
// header's clock is frozen at frame 0 unless a test unfreezes it; the only
// waits are those that prove the clock stays still.

#include <functional>
#include <memory>

#include <QApplication>
#include <QFile>
#include <QHBoxLayout>
#include <QHashFunctions>
#include <QHeaderView>
#include <QHelpEvent>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPaintEvent>
#include <QRegion>
#include <QSignalSpy>
#include <QStyleFactory>
#include <QStyleOptionHeader>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QToolTip>
#include <QTreeView>
#include <QtTest>

#include "calculationdemand.h"
#include "engine/calculationregistry.h"
#include "idlescheduler.h"
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
#include "ui/docks/DemandIndicator.h"
#include "ui/docks/logbook/LogbookCellDelegate.h"
#include "ui/docks/logbook/LogbookHeaderView.h"
#include "ui/docks/logbook/LogbookView.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

const QRgb kAmber = qRgb(0xE6, 0x9F, 0x00);

/// Counts the paint events of a widget. With an `area`, only those confined to
/// it: what a repaint of one section or row causes (a frame of the working
/// clock, a state change), not a stray expose of the whole widget.
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

/// Whether some pixel of `rect` in `image` has the colour `rgb` (alpha ignored).
bool hasPixel(const QImage &image, const QRect &rect, QRgb rgb)
{
    const QImage part = cut(image, rect);
    for (int y = 0; y < part.height(); ++y) {
        for (int x = 0; x < part.width(); ++x) {
            if ((part.pixel(x, y) & 0x00FFFFFF) == (rgb & 0x00FFFFFF))
                return true;
        }
    }
    return false;
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
    void workingColumnShowsIndicatorRightOfText();
    void indicatorAnimatesOnlyWhileWorking();
    void badgeReplacesIndicatorWhenFinished();
    void failedLoadSessionShowsBadgeNotPending();
    void headerToolTipFollowsDemandState();
    void indicatorFollowsColumnWhenMovedHiddenOrReordered();
    void indicatorClearsSortArrowAndNarrowSections();
    void clickOnIndicatorIsAClickOnTheSection();
    void pendingCellsAreDistinctFromUnavailable();
    void pendingCellBecomesValueWhenRecordIsWritten_data();
    void pendingCellBecomesValueWhenRecordIsWritten();
    void sortingTreatsPendingAsUnavailable();
    void unreadableRecordPendingIsNotDemandPending_data();
    void unreadableRecordPendingIsNotDemandPending();
    void columnStateChangeRepaintsOnlyThatColumn();
    void survivesDemandDestroyedFirst();

private:
    Gate &gate() { return m_world->gate(); }

    /// The logbook view and the reference tree in one window, as described at
    /// the top of the file; the header's clock frozen. False when the window
    /// was never exposed.
    [[nodiscard]] bool buildUi();
    void destroyUi();
    /// The executor, the demand layer and the UI over the current model.
    [[nodiscard]] bool buildServices();

    QTreeView *tree() const { return m_logbook->findChild<QTreeView *>(); }
    LogbookHeaderView *header() const { return qobject_cast<LogbookHeaderView *>(tree()->header()); }
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
    static QString colId(const char *key) { return CalculationDemand::columnId(attributeColumn(QString::fromLatin1(key))); }
    /// The current state of the column over `key`: a pending pass runs first.
    DemandState col(const char *key)
    {
        m_demand->flush();
        return m_demand->columnState(colId(key));
    }
    QModelIndex cell(const char *id, const char *key) const
    {
        return m_model->index(m_model->getSessionRow(QString::fromLatin1(id)), section(key));
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
    /// G_OUT enabled with the gate held: s1 running with its progress text
    /// delivered, s2 and s4 waiting. False on any other outcome.
    [[nodiscard]] bool makeWorkingColumn()
    {
        enableColumns({QStringLiteral("G_OUT")});
        if (!gate().waitEntered())
            return false;
        if (!QTest::qWaitFor([this] {
                return col("G_OUT").running.value(0).progressText == QStringLiteral("step 1");
            }))
            return false;
        const DemandState state = col("G_OUT");
        if (!state.isWorking() || state.wantedCount != 3 || sessionIdsOf(state.running) != QStringList({"s1"}))
            return false;
        spin();
        return true;
    }
    /// EA_IN -1 on s1, 4 on s2 and s4; EA1 enabled and finished: s1 failed.
    [[nodiscard]] bool makeBadgedColumn()
    {
        for (const auto &[id, value] : {std::pair{"s1", -1.0}, std::pair{"s2", 4.0}, std::pair{"s4", 4.0}}) {
            if (!PlotFixture::giveInput(*m_model, QString::fromLatin1(id), QStringLiteral("EA_IN"), value))
                return false;
        }
        m_model->flushPendingInvalidations();
        enableColumns({QStringLiteral("EA1")});
        if (!waitDemandIdle())
            return false;
        spin();
        return col("EA1").showsWarning();
    }

    QRect sectionRect(int logical) const
    {
        return QRect(header()->sectionViewportPosition(logical), 0, header()->sectionSize(logical),
                     header()->viewport()->height());
    }
    QRect cellRect(const QModelIndex &index) const { return tree()->visualRect(index); }
    QImage grabHeader() const { return header()->viewport()->grab().toImage(); }
    QImage grabReferenceHeader() const { return m_reference->header()->viewport()->grab().toImage(); }
    QImage grabCells() const { return tree()->viewport()->grab().toImage(); }
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
    /// Sends a tooltip event to the header's viewport at `pos`; whether it was accepted.
    bool headerHelp(const QPoint &pos)
    {
        QHelpEvent event(QEvent::ToolTip, pos, header()->viewport()->mapToGlobal(pos));
        QApplication::sendEvent(header()->viewport(), &event);
        return event.isAccepted();
    }
    /// The cell delegate's helpEvent() over `index`, as the view calls it.
    bool cellHelp(const QModelIndex &index)
    {
        QStyleOptionViewItem opt;
        opt.initFrom(tree()->viewport());
        opt.widget = tree();
        opt.rect = cellRect(index);
        const QPoint pos = opt.rect.center();
        QHelpEvent event(QEvent::ToolTip, pos, tree()->viewport()->mapToGlobal(pos));
        return cells()->helpEvent(&event, tree(), opt, index);
    }
    /// Hides any tooltip and waits until it is gone.
    [[nodiscard]] static bool hideToolTip()
    {
        QToolTip::hideText();
        return QTest::qWaitFor([] { return !QToolTip::isVisible(); }, 2000);
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

    // As LogbookView::setupView() configures its tree, with QTreeView's own header
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

    header()->animation()->setFrozen(true);
    spin();
    return header() && cells() && m_model->rowAt(0).sessionId == QStringLiteral("s1");
}

void LogbookIndicatorsTest::destroyUi()
{
    QToolTip::hideText();
    m_logbook = nullptr;
    m_reference = nullptr;
    m_window.reset();
}

// Note what is to be checked, tear everything down, and only then check (see
// tst_plot_row_delegate). The views go before the demand layer.
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

// ---- The header -----------------------------------------------------------------------

// No column over a requested calculation: header and cells are exactly the
// base classes', pixel for pixel.
void LogbookIndicatorsTest::plainHeaderAndCellsAreIdenticalToBase()
{
    enableColumns({QStringLiteral("G_IN")});
    spin();
    QCOMPARE(m_model->columnCount(), 2);

    QCOMPARE(grabHeader(), grabReferenceHeader());
    QCOMPARE(grabCells(), grabCellsWithBaseDelegate());
    for (int l = 0; l < header()->count(); ++l) {
        QCOMPARE(header()->indicatorRect(l), QRect());
        QVERIFY(header()->toolTipForSection(l).isEmpty());
        QCOMPARE(header()->sectionSizeHint(l), m_reference->header()->sectionSizeHint(l));
    }
    QVERIFY(!header()->animation()->isActive());
    QVERIFY(m_demand->workingColumnIds().isEmpty());
}

void LogbookIndicatorsTest::workingColumnShowsIndicatorRightOfText()
{
    const int plainHeight = header()->height();
    QVERIFY(makeWorkingColumn());
    const int g = section("G_OUT");
    QVERIFY(g >= 0);

    const QRect r = header()->indicatorRect(g);
    QVERIFY(!r.isNull());
    const QRect sr = sectionRect(g);
    QVERIFY(sr.contains(r));
    QVERIFY(r.left() > sr.center().x());
    QVERIFY(header()->viewport()->rect().contains(r));

    const QImage with = grabHeader();
    const QImage reference = grabReferenceHeader();
    QCOMPARE(with.size(), reference.size());
    QVERIFY(cut(with, r) != cut(reference, r));
    for (int l = 0; l < header()->count(); ++l) {
        if (l != g)
            QCOMPARE(cut(with, sectionRect(l)), cut(reference, sectionRect(l)));
    }

    QCOMPARE(header()->height(), plainHeight);
    QCOMPARE(header()->height(), m_reference->header()->height());
    QVERIFY(header()->sectionSizeHint(g) > m_reference->header()->sectionSizeHint(g));
}

// The clock runs exactly while a column works; each frame repaints, and once
// nothing works nothing is repainted.
void LogbookIndicatorsTest::indicatorAnimatesOnlyWhileWorking()
{
    QVERIFY(makeWorkingColumn());
    WorkingAnimation *clock = header()->animation();
    QVERIFY(clock->isActive());
    QVERIFY(!clock->isTicking());       // frozen
    const int g = section("G_OUT");
    const QRect r = header()->indicatorRect(g);
    QVERIFY(!r.isNull());

    const QImage frame0 = grabHeader();
    auto *paints = new PaintCounter(header()->viewport());
    const int before = paints->count;
    clock->advance();
    QTRY_VERIFY(paints->count > before);
    const QImage frame1 = grabHeader();
    QVERIFY(cut(frame1, r) != cut(frame0, r));
    // Only the working section changed
    for (int l = 0; l < header()->count(); ++l) {
        if (l != g)
            QCOMPARE(cut(frame1, sectionRect(l)), cut(frame0, sectionRect(l)));
    }

    clock->setFrozen(false);
    QVERIFY(clock->isTicking());
    gate().open(3);
    QVERIFY(waitDemandIdle());
    QVERIFY(waitForIdle(*m_model));
    spin();
    QVERIFY(col("G_OUT").isPlain());
    QVERIFY(!clock->isActive());
    QVERIFY(!clock->isTicking());
    QCOMPARE(clock->frame(), 0);
    QCOMPARE(header()->indicatorRect(g), QRect());

    // No idle repaint: the clock is stopped (above), and the section is not
    // repainted by itself
    const auto *sectionPaints = new PaintCounter(header()->viewport(), sectionRect(g));
    QTest::qWait(4 * WorkingAnimation::kFrameIntervalMs);
    QCOMPARE(sectionPaints->count, 0);
}

void LogbookIndicatorsTest::badgeReplacesIndicatorWhenFinished()
{
    QVERIFY(makeBadgedColumn());
    const DemandState state = col("EA1");
    QVERIFY(state.showsWarning());
    QCOMPARE(state.failedCount, 1);
    const int ea = section("EA1");
    const QRect r = header()->indicatorRect(ea);
    QVERIFY(!r.isNull());
    QVERIFY(sectionRect(ea).contains(r));

    const QImage with = grabHeader();
    QVERIFY(hasPixel(with, r, kAmber));
    QVERIFY(!hasPixel(grabReferenceHeader(), sectionRect(ea), kAmber));
    QVERIFY(!header()->animation()->isActive());

    // The badge is not a control
    const Quiet quiet(*m_queue);
    QTest::mouseClick(header()->viewport(), Qt::LeftButton, {}, r.center());
    spin();
    QVERIFY(quiet.holds());
    QVERIFY(col("EA1") == state);
}

// Presentation half of tst_calculation_demand's
// visibleFailedLoadIsSettledAsFailed: a visible session whose file cannot be
// loaded is failed, never pending, and the column finishes with the badge.
void LogbookIndicatorsTest::failedLoadSessionShowsBadgeNotPending()
{
    PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
    QVERIFY(waitForIdle(*m_model));
    for (const char *id : {"s1", "s2", "s3", "s4"})
        QVERIFY2(!m_model->rowAt(m_model->getSessionRow(QString::fromLatin1(id))).isLoaded(), id);
    QVERIFY(QFile::remove(sessionFilePath(QStringLiteral("s2"))));
    PlotFixture::show(*m_model, {"s2"});
    const SessionRow &s2 = m_model->rowAt(m_model->getSessionRow(QStringLiteral("s2")));
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

    const int g = section("G_OUT");
    QVERIFY(col("G_OUT").showsWarning());
    const QRect r = header()->indicatorRect(g);
    QVERIFY(!r.isNull());
    QVERIFY(hasPixel(grabHeader(), r, kAmber));
    QVERIFY(!header()->animation()->isActive());
    const QString text = header()->toolTipForSection(g);
    QVERIFY(text.contains(QStringLiteral("Could not be computed:")));
    QVERIFY(text.contains(QStringLiteral("The session file could not be loaded")));

    // Settled: nothing repaints the section any more
    const auto *paints = new PaintCounter(header()->viewport(), sectionRect(g));
    for (int i = 0; i < 3; ++i)
        spin();
    QCOMPARE(paints->count, 0);
}

void LogbookIndicatorsTest::headerToolTipFollowsDemandState()
{
    QVERIFY(makeWorkingColumn());
    const int g = section("G_OUT");
    DemandState state = col("G_OUT");
    QCOMPARE(header()->toolTipForSection(g), state.toolTip);
    QCOMPARE(state.toolTip, CalculationDemand::buildToolTip(state));
    QVERIFY(state.toolTip.startsWith(QStringLiteral("Computing: 0 of 3 done")));
    QVERIFY(state.toolTip.contains(QStringLiteral("  Jump 1 - Gated: step 1")));

    // The whole section shows it: the indicator and the section's left edge
    const QRect r = header()->indicatorRect(g);
    QVERIFY(!r.isNull());
    for (const QPoint &pos : {r.center(), QPoint(sectionRect(g).left() + 4, sectionRect(g).center().y())}) {
        QVERIFY(hideToolTip());
        QVERIFY(headerHelp(pos));
        QCOMPARE(QToolTip::text(), state.toolTip);
    }

    // s1 done, s2 running
    gate().open(1);
    QVERIFY(gate().waitEntered());
    QTRY_VERIFY(header()->toolTipForSection(g).contains(QStringLiteral("Jump 2 - Gated: step 1")));
    state = col("G_OUT");
    QVERIFY(state.toolTip.startsWith(QStringLiteral("Computing: 1 of 3 done")));
    QCOMPARE(header()->toolTipForSection(g), state.toolTip);

    // Failed
    gate().open(16);
    QVERIFY(waitDemandIdle());
    QVERIFY(makeBadgedColumn());
    const int ea = section("EA1");
    QCOMPARE(header()->toolTipForSection(ea),
             QStringLiteral("Could not be computed:\n  Jump 1 - Explicit A: negative input"));
    QVERIFY(hideToolTip());
    QVERIFY(headerHelp(header()->indicatorRect(ea).center()));
    QCOMPARE(QToolTip::text(), header()->toolTipForSection(ea));

    // A plain section shows nothing of its own
    QVERIFY(hideToolTip());
    const int d = descriptionSection();
    QVERIFY(!headerHelp(sectionRect(d).center()));
    spin();
    QVERIFY(!QToolTip::isVisible());
}

// Logical indices throughout: moving, hiding and rebuilding the columns keep
// the indicator on the right section.
void LogbookIndicatorsTest::indicatorFollowsColumnWhenMovedHiddenOrReordered()
{
    enableColumns({QStringLiteral("G_IN"), QStringLiteral("G_OUT")});
    QVERIFY(gate().waitEntered());
    QTRY_VERIFY(col("G_OUT").isWorking());
    spin();
    int g = section("G_OUT");
    QCOMPARE(g, 2);

    // Moved to the front
    header()->moveSection(header()->visualIndex(g), 0);
    m_reference->header()->moveSection(m_reference->header()->visualIndex(g), 0);
    spin();
    QCOMPARE(header()->visualIndex(g), 0);
    QVERIFY(!header()->indicatorRect(g).isNull());
    QVERIFY(sectionRect(g).contains(header()->indicatorRect(g)));
    QCOMPARE(sectionRect(g).left(), 0);
    for (int l = 0; l < header()->count(); ++l) {
        if (l != g)
            QCOMPARE(header()->indicatorRect(l), QRect());
    }
    {
        const QImage with = grabHeader();
        const QImage reference = grabReferenceHeader();
        QVERIFY(cut(with, sectionRect(g)) != cut(reference, sectionRect(g)));
        for (int l = 0; l < header()->count(); ++l) {
            if (l != g)
                QCOMPARE(cut(with, sectionRect(l)), cut(reference, sectionRect(l)));
        }
    }

    // Hidden: nothing is painted for it
    header()->hideSection(g);
    m_reference->header()->hideSection(g);
    spin();
    QCOMPARE(header()->indicatorRect(g), QRect());
    QCOMPARE(grabHeader(), grabReferenceHeader());
    header()->showSection(g);
    m_reference->header()->showSection(g);
    spin();
    QVERIFY(!header()->indicatorRect(g).isNull());

    // Rebuilt columns (a reset): new logical indices
    enableColumns({QStringLiteral("G_OUT"), QStringLiteral("G_IN")});
    spin();
    g = section("G_OUT");
    QCOMPARE(g, 1);
    QVERIFY(col("G_OUT").isWorking());
    QVERIFY(!header()->indicatorRect(g).isNull());
    QVERIFY(sectionRect(g).contains(header()->indicatorRect(g)));
    QVERIFY(header()->toolTipForSection(section("G_IN")).isEmpty());
    QCOMPARE(header()->indicatorRect(section("G_IN")), QRect());
    QCOMPARE(header()->toolTipForSection(g), col("G_OUT").toolTip);
}

void LogbookIndicatorsTest::indicatorClearsSortArrowAndNarrowSections()
{
    // G_OUT not the last (stretched) section, so that it can be resized
    enableColumns({QStringLiteral("G_OUT"), QStringLiteral("G_IN")});
    QVERIFY(gate().waitEntered());
    QTRY_VERIFY(col("G_OUT").isWorking());
    const int g = section("G_OUT");
    tree()->sortByColumn(g, Qt::AscendingOrder);
    spin();
    QCOMPARE(header()->sortIndicatorSection(), g);

    const auto arrowRect = [this, g] {
        QStyleOptionHeader opt;
        opt.initFrom(header());
        opt.rect = sectionRect(g);
        opt.state |= QStyle::State_Horizontal;
        opt.orientation = Qt::Horizontal;
        opt.section = g;
        opt.sortIndicator = QStyleOptionHeader::SortUp;
        return header()->style()->subElementRect(QStyle::SE_HeaderArrow, &opt, header());
    };

    QRect r = header()->indicatorRect(g);
    QVERIFY(!r.isNull());
    QVERIFY(sectionRect(g).contains(r));
    QVERIFY(!r.intersects(arrowRect()));

    header()->resizeSection(g, 60);
    spin();
    QCOMPARE(header()->sectionSize(g), 60);
    r = header()->indicatorRect(g);
    QVERIFY(!r.isNull());
    QVERIFY(sectionRect(g).contains(r));
    QVERIFY(!r.intersects(arrowRect()));

    header()->resizeSection(g, 12);
    spin();
    QCOMPARE(header()->indicatorRect(g), QRect());
    QVERIFY(!grabHeader().isNull());        // painted without a glyph, and without a crash
    QVERIFY(!header()->toolTipForSection(g).isEmpty());
}

// No gesture: a click on the badge sorts as a click elsewhere in the section
// does, and starts or cancels nothing. (A finished column with a failure: a
// finished column without one has no glyph.)
void LogbookIndicatorsTest::clickOnIndicatorIsAClickOnTheSection()
{
    QVERIFY(makeBadgedColumn());
    const int ea = section("EA1");
    const QRect r = header()->indicatorRect(ea);
    QVERIFY(!r.isNull());

    const Quiet quiet(*m_queue);
    QSignalSpy cancelSpy(m_queue.get(), &JobQueue::jobCancelRequested);

    QTest::mouseClick(header()->viewport(), Qt::LeftButton, {}, r.center());
    spin();
    QCOMPARE(header()->sortIndicatorSection(), ea);
    const Qt::SortOrder first = header()->sortIndicatorOrder();

    // Again on the glyph: the order flips
    QTest::mouseClick(header()->viewport(), Qt::LeftButton, {}, header()->indicatorRect(ea).center());
    spin();
    QCOMPARE(header()->sortIndicatorSection(), ea);
    QVERIFY(header()->sortIndicatorOrder() != first);

    // ... as it does on the section's left edge
    QTest::mouseClick(header()->viewport(), Qt::LeftButton, {},
                      QPoint(sectionRect(ea).left() + 4, sectionRect(ea).center().y()));
    spin();
    QCOMPARE(header()->sortIndicatorSection(), ea);
    QCOMPARE(header()->sortIndicatorOrder(), first);

    QVERIFY(quiet.holds());
    QCOMPARE(cancelSpy.count(), 0);
    QVERIFY(col("EA1").showsWarning());
}

// ---- The cells ------------------------------------------------------------------------

// Spec 10: "not yet" reads differently from "never", and pending is nowhere
// but in the view.
void LogbookIndicatorsTest::pendingCellsAreDistinctFromUnavailable()
{
    QVERIFY(makeWorkingColumn());
    QCOMPARE(sessionIdsOf(col("G_OUT").running), QStringList({"s1"}));
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

// Spec 13: the pending cell becomes the value when the record is written; the
// progress line reports the fill without a cancel button.
void LogbookIndicatorsTest::pendingCellBecomesValueWhenRecordIsWritten()
{
    QFETCH(bool, stubs);
    if (stubs) {
        PreferencesManager::instance().setValue(PreferenceKeys::LogbookCacheSize, 0);
        QVERIFY(waitForIdle(*m_model));
        for (const char *id : {"s1", "s2", "s3", "s4"})
            QVERIFY2(!m_model->rowAt(m_model->getSessionRow(QString::fromLatin1(id))).isLoaded(), id);
    }

    // The scheduler wired to the view as LogbookDockFeature wires it
    IdleScheduler &scheduler = m_model->scheduler();
    connect(&scheduler, &IdleScheduler::activeTaskChanged, m_logbook, &LogbookView::onActiveTaskChanged);
    connect(&scheduler, &IdleScheduler::progressChanged, m_logbook, &LogbookView::onProgressChanged);
    connect(&scheduler, &IdleScheduler::schedulerIdle, m_logbook, &LogbookView::onSchedulerIdle);
    auto *cancelButton = m_logbook->findChild<QToolButton *>();
    QVERIFY(cancelButton);
    QObject scope;
    int fillActivations = 0;
    bool cancelEverShown = false;
    connect(&scheduler, &IdleScheduler::activeTaskChanged, &scope, [&](int id, bool) {
        if (id != SessionModel::ColumnFillTask)
            return;
        ++fillActivations;
        cancelEverShown = cancelEverShown || cancelButton->isVisibleTo(m_logbook);
    });
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

    // No cancel for requested calculations
    QVERIFY(!cancelEverShown);
    if (stubs)
        QVERIFY(fillActivations > 0);
    else
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
    QVERIFY(!col("G_OUT").isWorking());
    QVERIFY(quiet.holds());
    QVERIFY(unreadable.release());
}

// One repaint of the visible part of one column, and no model signal.
void LogbookIndicatorsTest::columnStateChangeRepaintsOnlyThatColumn()
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

    emit m_demand->columnStateChanged(colId("G_OUT"));
    QTRY_VERIFY(!treeRegion->region.isEmpty());
    QTRY_VERIFY(!headerRegion->region.isEmpty());
    const QRect columnRect(tree()->columnViewportPosition(g), 0, tree()->columnWidth(g),
                           tree()->viewport()->height());
    QVERIFY((treeRegion->region - QRegion(columnRect)).isEmpty());
    QVERIFY((headerRegion->region - QRegion(sectionRect(g))).isEmpty());
    QCOMPARE(dataSpy.count(), 0);
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(layoutSpy.count(), 0);
    QCOMPARE(headerDataSpy.count(), 0);

    // An unknown id is harmless
    emit m_demand->columnStateChanged(QStringLiteral("no such column"));
    spin();
}

// MainWindow deletes the demand layer before the docks: header and cells then
// are the base classes'.
void LogbookIndicatorsTest::survivesDemandDestroyedFirst()
{
    QVERIFY(makeWorkingColumn());
    const int g = section("G_OUT");
    const QPoint inIndicator = header()->indicatorRect(g).center();
    QVERIFY(header()->animation()->isActive());

    m_demand.reset();
    spin();

    QCOMPARE(grabHeader(), grabReferenceHeader());
    QCOMPARE(grabCells(), grabCellsWithBaseDelegate());
    for (int l = 0; l < header()->count(); ++l) {
        QCOMPARE(header()->indicatorRect(l), QRect());
        QVERIFY(header()->toolTipForSection(l).isEmpty());
    }
    QVERIFY(!header()->animation()->isActive());
    for (int r = 0; r < m_model->rowCount(); ++r)
        QVERIFY(!cells()->showsPending(m_model->index(r, g)));

    QVERIFY(hideToolTip());
    QVERIFY(!headerHelp(inIndicator));
    QVERIFY(!cellHelp(cell("s2", "G_OUT")));
    spin();
    QVERIFY(!QToolTip::isVisible());

    tree()->sortByColumn(g, Qt::AscendingOrder);
    spin();
    QCOMPARE(header()->sortIndicatorSection(), g);
}

// A Widgets test writes its own main() (see tst_plot_row_delegate): the same
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
