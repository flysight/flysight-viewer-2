// tst_plot_color: plotColor() (src/plotutils.h), the one colour a plot is
// drawn in. The plot widget, the legend, the measure tool and the plot
// settings page all take a plot's colour from it, so a colour the user chose
// on the settings page is the same colour in each of them; before, the legend
// and the measure tool showed the registry's default whatever was stored.

#include <QColor>
#include <QtTest>

#include "plotregistry.h"
#include "plotutils.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

PlotValue syntheticPlot()
{
    PlotValue pv;
    pv.category = QStringLiteral("Synthetic");
    pv.plotName = QStringLiteral("Value");
    pv.plotUnits = QStringLiteral("m");
    pv.defaultColor = QColor(10, 20, 30);
    pv.sensorID = QStringLiteral("Syn");
    pv.measurementID = QStringLiteral("value");
    pv.measurementType = QStringLiteral("distance");
    return pv;
}

} // namespace

class PlotColorTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup() { TestEnvironment::instance().resetPreferencesToDefaults(); }

    void defaultWithoutAStoredColour();
    void storedColourWins();
    void aStoredValueThatIsNoColourFallsBack();
};

// Nothing stored: the registry's default
void PlotColorTest::defaultWithoutAStoredColour()
{
    const PlotValue pv = syntheticPlot();
    QVERIFY(!PreferencesManager::instance()
                 .getValue(PreferenceKeys::plotColorKey(pv.sensorID, pv.measurementID)).isValid());
    QCOMPARE(plotColor(pv), pv.defaultColor);
}

// A stored colour, as the settings page stores it (a colour name) and as a
// QColor, wins over the default
void PlotColorTest::storedColourWins()
{
    const PlotValue pv = syntheticPlot();
    const QString key = PreferenceKeys::plotColorKey(pv.sensorID, pv.measurementID);

    PreferencesManager::instance().setValue(key, QColor(200, 100, 50).name());
    QCOMPARE(plotColor(pv), QColor(200, 100, 50));

    PreferencesManager::instance().setValue(key, QVariant::fromValue(QColor(1, 2, 3)));
    QCOMPARE(plotColor(pv), QColor(1, 2, 3));

    // Another plot's colour is another key
    PlotValue other = pv;
    other.measurementID = QStringLiteral("other");
    QCOMPARE(plotColor(other), pv.defaultColor);
}

// A stored value that is not a colour (an empty or broken name) is ignored
void PlotColorTest::aStoredValueThatIsNoColourFallsBack()
{
    const PlotValue pv = syntheticPlot();
    const QString key = PreferenceKeys::plotColorKey(pv.sensorID, pv.measurementID);
    for (const QVariant &broken : {QVariant(QString()), QVariant(QStringLiteral("not a colour")), QVariant(42)}) {
        PreferencesManager::instance().setValue(key, broken);
        QCOMPARE(plotColor(pv), pv.defaultColor);
    }
}

FLYSIGHT_TEST_MAIN(PlotColorTest)

#include "tst_plot_color.moc"
