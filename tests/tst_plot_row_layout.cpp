// The geometry of a plot-list row's control cluster (PlotRowLayout.h): a pure
// function of integers. No widgets, no font, no GUI application.
// Sensor-fusion-jobs spec 9.2 (the view half).

#include <QtTest>

#include "testmain.h"
#include "ui/docks/plotselection/PlotRowLayout.h"

using namespace FlySight;

namespace {

const QRect kItem(20, 100, 200, 20);
const PlotRowMetrics kMetrics{16, 4, 4};

QList<QRect> rectsOf(const PlotRowGeometry &geometry)
{
    return {geometry.warningIcon, geometry.warningCount, geometry.controlLabel,
            geometry.controlIcon, geometry.controlHit};
}

QRect mirrored(const QRect &rect, const QRect &item)
{
    if (rect.isNull())
        return rect;
    // The pixel column x maps to item.left() + item.right() - x
    return QRect(item.left() + item.right() - rect.right(), rect.y(), rect.width(), rect.height());
}

} // namespace

class PlotRowLayoutTest : public QObject {
    Q_OBJECT

private slots:
    void controlOnly();
    void controlAndWarning();
    void warningOnly();
    void nothingShown();
    void emptyLabelOmitsItsSpacing();
    void rightToLeftIsMirrorImage_data();
    void rightToLeftIsMirrorImage();
    void hitRectSpansRowHeightToRightEdge();
};

void PlotRowLayoutTest::controlOnly()
{
    const PlotRowGeometry g = layoutPlotRow(kItem, kMetrics, false, 0, true, 7, Qt::LeftToRight);
    QCOMPARE(g.controlIcon, QRect(200, 102, 16, 16));
    QCOMPARE(g.controlLabel, QRect(189, 100, 7, 20));
    QCOMPARE(g.controlHit, QRect(198, 100, 22, 20));
    QCOMPARE(g.clusterWidth, 31);
    QVERIFY(g.warningIcon.isNull());
    QVERIFY(g.warningCount.isNull());
}

void PlotRowLayoutTest::controlAndWarning()
{
    const PlotRowGeometry g = layoutPlotRow(kItem, kMetrics, true, 7, true, 7, Qt::LeftToRight);
    // The control group does not move when a warning appears
    QCOMPARE(g.controlIcon, QRect(200, 102, 16, 16));
    QCOMPARE(g.controlLabel, QRect(189, 100, 7, 20));
    QCOMPARE(g.controlHit, QRect(198, 100, 22, 20));
    QCOMPARE(g.warningCount, QRect(174, 100, 7, 20));
    QCOMPARE(g.warningIcon, QRect(154, 102, 16, 16));
    QCOMPARE(g.clusterWidth, 66);
}

void PlotRowLayoutTest::warningOnly()
{
    const PlotRowGeometry g = layoutPlotRow(kItem, kMetrics, true, 7, false, 0, Qt::LeftToRight);
    QCOMPARE(g.warningCount.right(), kItem.right() - 4);
    QCOMPARE(g.warningCount, QRect(209, 100, 7, 20));
    QCOMPARE(g.warningIcon, QRect(189, 102, 16, 16));
    QCOMPARE(g.clusterWidth, 31);
    QVERIFY(g.controlHit.isNull());
    QVERIFY(g.controlIcon.isNull());
    QVERIFY(g.controlLabel.isNull());

    // A label width without a control is ignored
    const PlotRowGeometry ignored = layoutPlotRow(kItem, kMetrics, true, 7, false, 9, Qt::LeftToRight);
    QCOMPARE(rectsOf(ignored), rectsOf(g));
    QCOMPARE(ignored.clusterWidth, g.clusterWidth);
}

void PlotRowLayoutTest::nothingShown()
{
    for (const Qt::LayoutDirection direction : {Qt::LeftToRight, Qt::RightToLeft}) {
        const PlotRowGeometry g = layoutPlotRow(kItem, kMetrics, false, 7, false, 7, direction);
        for (const QRect &rect : rectsOf(g))
            QVERIFY(rect.isNull());
        QCOMPARE(g.clusterWidth, 0);
    }
}

void PlotRowLayoutTest::emptyLabelOmitsItsSpacing()
{
    const PlotRowGeometry g = layoutPlotRow(kItem, kMetrics, false, 0, true, 0, Qt::LeftToRight);
    QCOMPARE(g.controlIcon, QRect(200, 102, 16, 16));
    QVERIFY(g.controlLabel.isNull());
    QCOMPARE(g.clusterWidth, 20);       // margin + icon; no spacing for a label that is not there

    // ... so a warning sits two spacings from the icon
    const PlotRowGeometry w = layoutPlotRow(kItem, kMetrics, true, 7, true, 0, Qt::LeftToRight);
    QCOMPARE(w.warningCount, QRect(185, 100, 7, 20));
    QCOMPARE(w.warningIcon, QRect(165, 102, 16, 16));
    QCOMPARE(w.clusterWidth, 55);
}

void PlotRowLayoutTest::rightToLeftIsMirrorImage_data()
{
    QTest::addColumn<QRect>("item");
    QTest::addColumn<bool>("warning");
    QTest::addColumn<int>("countWidth");
    QTest::addColumn<bool>("control");
    QTest::addColumn<int>("labelWidth");

    QTest::newRow("control") << kItem << false << 0 << true << 7;
    QTest::newRow("both") << kItem << true << 7 << true << 7;
    QTest::newRow("warning") << kItem << true << 13 << false << 0;
    QTest::newRow("progress") << QRect(0, 0, 301, 23) << true << 8 << true << 41;
    QTest::newRow("odd origin") << QRect(-7, 3, 97, 17) << true << 6 << true << 6;
    QTest::newRow("exact fit") << QRect(20, 100, 66, 20) << true << 7 << true << 7;
}

void PlotRowLayoutTest::rightToLeftIsMirrorImage()
{
    QFETCH(QRect, item);
    QFETCH(bool, warning);
    QFETCH(int, countWidth);
    QFETCH(bool, control);
    QFETCH(int, labelWidth);

    const PlotRowGeometry ltr = layoutPlotRow(item, kMetrics, warning, countWidth, control, labelWidth, Qt::LeftToRight);
    const PlotRowGeometry rtl = layoutPlotRow(item, kMetrics, warning, countWidth, control, labelWidth, Qt::RightToLeft);

    QCOMPARE(rtl.clusterWidth, ltr.clusterWidth);
    const QList<QRect> left = rectsOf(ltr);
    const QList<QRect> right = rectsOf(rtl);
    for (int i = 0; i < left.size(); ++i) {
        QCOMPARE(right.at(i), mirrored(left.at(i), item));
        QCOMPARE(mirrored(right.at(i), item), left.at(i));
    }

    // Nothing leaves the item horizontally when the cluster fits
    QVERIFY(item.width() >= ltr.clusterWidth);
    for (const QRect &rect : left + right) {
        if (rect.isNull())
            continue;
        QVERIFY2(rect.left() >= item.left() && rect.right() <= item.right(),
                 qPrintable(QStringLiteral("%1..%2").arg(rect.left()).arg(rect.right())));
    }

    // In right-to-left the hit rectangle starts at the item's left edge
    if (control) {
        QCOMPARE(rtl.controlHit.left(), item.left());
        QCOMPARE(ltr.controlHit.right(), item.right());
    }
}

void PlotRowLayoutTest::hitRectSpansRowHeightToRightEdge()
{
    const PlotRowGeometry g = layoutPlotRow(kItem, kMetrics, true, 7, true, 7, Qt::LeftToRight);
    QCOMPARE(g.controlHit.top(), kItem.top());
    QCOMPARE(g.controlHit.bottom(), kItem.bottom());
    QCOMPARE(g.controlHit.right(), kItem.right());
    QCOMPARE(g.controlHit.left(), g.controlIcon.left() - kMetrics.spacing / 2);

    // The icon's column counts, above and below the glyph and out to the edge ...
    QVERIFY(g.controlHit.contains(QPoint(g.controlIcon.center().x(), kItem.top())));
    QVERIFY(g.controlHit.contains(QPoint(g.controlIcon.center().x(), kItem.bottom())));
    QVERIFY(g.controlHit.contains(QPoint(kItem.right(), g.controlIcon.center().y())));
    // ... the label and the warning group do not
    QVERIFY(!g.controlHit.contains(g.controlLabel.center()));
    QVERIFY(!g.controlHit.intersects(g.controlLabel));
    QVERIFY(!g.controlHit.intersects(g.warningCount));
    QVERIFY(!g.controlHit.intersects(g.warningIcon));
    QVERIFY(!g.controlHit.contains(QPoint(kItem.right() + 1, g.controlIcon.center().y())));
}

FLYSIGHT_TEST_MAIN(PlotRowLayoutTest)
#include "tst_plot_row_layout.moc"
