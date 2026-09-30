// tst_plot_format: formatValue() and formatXAxisValue() (src/plotutils.h),
// the one way a plot's value is written as text in the legend, the analysis
// tab and the measure tool. A value is formatted by its row's measurement
// type, through the unit converter, as the logbook formats a column; the
// measurement's name never enters. Before, the name did: a lowercased id
// containing "lat" or "lon" was written raw to six decimals, and the
// along-track accelerations (accAlongTrack) were caught by "lon".

#include <QtTest>

#include <cmath>
#include <limits>

#include "plotutils.h"
#include "sessiondata.h"
#include "testenvironment.h"
#include "testmain.h"
#include "units/unitconverter.h"

using namespace FlySight;
using namespace FlySightTest;

class PlotFormatTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup() { TestEnvironment::instance().resetPreferencesToDefaults(); }

    void typedValueIsTheConverterFormat();
    void alongTrackIsAnAcceleration();
    void untypedValueHasOneDecimal();
    void xAxisSecondsHaveThreeDecimals();
    void nanIsDashes();
};

// A typed value is what the unit converter makes of it: converted to the
// display unit and rounded to the type's precision
void PlotFormatTest::typedValueIsTheConverterFormat()
{
    const UnitConverter &units = UnitConverter::instance();
    for (const char *type : {"acceleration", "speed", "distance", "angle", "time"}) {
        const QString t = QString::fromLatin1(type);
        const double value = 4.654622;
        const QString expected = QString::number(units.convert(value, t), 'f', units.getPrecision(t));
        QCOMPARE(formatValue(value, t), expected);
    }
}

// The regression: 4.654622 m/s^2 of acceleration is 0.47 g, not "4.654622";
// the along-track rows format like every other acceleration row
void PlotFormatTest::alongTrackIsAnAcceleration()
{
    const UnitConverter &units = UnitConverter::instance();
    const QString type = QStringLiteral("acceleration");
    const double value = 4.654622;
    const QString expected = QString::number(units.convert(value, type), 'f', units.getPrecision(type));
    QVERIFY(expected != QStringLiteral("4.654622"));
    QCOMPARE(formatValue(value, type), expected);
    // and the same text a logbook column would show
    QCOMPARE(formatValue(value, type), units.formatValue(value, type));
}

// Without a type there is nothing to convert by: one decimal
void PlotFormatTest::untypedValueHasOneDecimal()
{
    QCOMPARE(formatValue(4.654622, QString()), QStringLiteral("4.7"));
}

// The x axis in seconds is a time: three decimals, as the time rows show
void PlotFormatTest::xAxisSecondsHaveThreeDecimals()
{
    QCOMPARE(formatXAxisValue(12.3456789, QString::fromLatin1(SessionKeys::Time), QStringLiteral("exit")),
             QStringLiteral("12.346"));
    QCOMPARE(formatXAxisValue(12.3456789, QString::fromLatin1(SessionKeys::SystemTime), QString()),
             QStringLiteral("12.346"));
}

void PlotFormatTest::nanIsDashes()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    QCOMPARE(formatValue(nan, QStringLiteral("acceleration")), QStringLiteral("--"));
    QCOMPARE(formatValue(nan, QString()), QStringLiteral("--"));
    QCOMPARE(formatXAxisValue(nan, QString::fromLatin1(SessionKeys::Time), QString()), QStringLiteral("--"));
}

FLYSIGHT_TEST_MAIN(PlotFormatTest)
#include "tst_plot_format.moc"
