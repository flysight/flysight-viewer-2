// The geometry of a plot-list row's one glyph (PlotRowLayout.h): a pure
// function of integers. No widgets, no font, no GUI application. What is shown
// is DemandState's; where it goes is this function's.
// Sensor-fusion-jobs spec 9.2 (the view half).

#include <QtTest>

#include "testmain.h"
#include "ui/docks/plotselection/PlotRowLayout.h"

using namespace FlySight;

namespace {

const QRect kItem(20, 100, 200, 20);
const PlotRowMetrics kMetrics{16, 4, 4};

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
    void indicatorOnly();
    void nothingShown();
    void rightToLeftIsMirrorImage_data();
    void rightToLeftIsMirrorImage();
};

// The one glyph (the working indicator or the badge), with no label or count
void PlotRowLayoutTest::indicatorOnly()
{
    const PlotRowGeometry g = layoutPlotRow(kItem, kMetrics, true, Qt::LeftToRight);
    QCOMPARE(g.glyph, QRect(200, 102, 16, 16));
    QCOMPARE(g.reservedWidth, 20);      // margin + glyph
}

void PlotRowLayoutTest::nothingShown()
{
    for (const Qt::LayoutDirection direction : {Qt::LeftToRight, Qt::RightToLeft}) {
        const PlotRowGeometry g = layoutPlotRow(kItem, kMetrics, false, direction);
        QVERIFY(g.glyph.isNull());
        QCOMPARE(g.reservedWidth, 0);
    }
}

void PlotRowLayoutTest::rightToLeftIsMirrorImage_data()
{
    QTest::addColumn<QRect>("item");

    QTest::newRow("row") << kItem;
    QTest::newRow("wide") << QRect(0, 0, 301, 23);
    QTest::newRow("odd origin") << QRect(-7, 3, 97, 17);
    QTest::newRow("exact fit") << QRect(20, 100, 20, 20);
}

void PlotRowLayoutTest::rightToLeftIsMirrorImage()
{
    QFETCH(QRect, item);

    const PlotRowGeometry ltr = layoutPlotRow(item, kMetrics, true, Qt::LeftToRight);
    const PlotRowGeometry rtl = layoutPlotRow(item, kMetrics, true, Qt::RightToLeft);

    QCOMPARE(rtl.reservedWidth, ltr.reservedWidth);
    QCOMPARE(rtl.glyph, mirrored(ltr.glyph, item));
    QCOMPARE(mirrored(rtl.glyph, item), ltr.glyph);

    // The glyph does not leave the item horizontally when it fits
    QVERIFY(item.width() >= ltr.reservedWidth);
    for (const QRect &rect : {ltr.glyph, rtl.glyph}) {
        QVERIFY(!rect.isNull());
        QVERIFY2(rect.left() >= item.left() && rect.right() <= item.right(),
                 qPrintable(QStringLiteral("%1..%2").arg(rect.left()).arg(rect.right())));
    }
}

FLYSIGHT_TEST_MAIN(PlotRowLayoutTest)
#include "tst_plot_row_layout.moc"
