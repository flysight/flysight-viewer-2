// Golden characterization of every built-in calculation, read through
// SessionData on the generated descent fixture.
//
// The literals in goldenValues() (support/builtinfixture.cpp) were derived by
// hand from the algorithms, except for a few rows marked "captured", which
// were recorded from the v2026.04.1 engine before the built-ins were migrated.
// The same table is asserted against the calculation engine on a fake session
// in tst_builtins_engine.

#include <QtTest>

#include "builtinfixture.h"
#include "sessiondata.h"
#include "testenvironment.h"
#include "testmain.h"

using namespace FlySight;
using namespace FlySightTest;

class BuiltinsGoldenTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void sessionDataMatchesGolden_data();
    void sessionDataMatchesGolden();
    void storedEnumerationUnaffected();

private:
    SessionData m_session;
};

void BuiltinsGoldenTest::initTestCase()
{
    TestEnvironment::instance().registerBuiltIns();
    TestEnvironment::instance().resetPreferencesToDefaults();
    m_session = DescentFixture::load();
}

void BuiltinsGoldenTest::init()
{
    TestEnvironment::instance().resetPreferencesToDefaults();
}

void BuiltinsGoldenTest::sessionDataMatchesGolden_data()
{
    QTest::addColumn<int>("row");
    const QList<GoldenValue> golden = goldenValues();
    for (int i = 0; i < golden.size(); ++i)
        QTest::newRow(qPrintable(goldenTag(golden.at(i).name))) << i;
}

void BuiltinsGoldenTest::sessionDataMatchesGolden()
{
    QFETCH(int, row);
    const GoldenValue golden = goldenValues().at(row);

    QVariant attribute;
    QVector<double> samples;
    if (golden.name.type == DependencyKey::Type::Attribute)
        attribute = m_session.getAttribute(golden.name.attributeKey);
    else
        samples = m_session.getMeasurement(golden.name.measurementKey.first,
                                           golden.name.measurementKey.second);

    const QString difference = compareToGolden(golden, attribute, samples);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

// Spec 4: enumeration and presence describe stored data only; a calculation
// output does not appear because it happened to be computed.
void BuiltinsGoldenTest::storedEnumerationUnaffected()
{
    SessionData session = DescentFixture::load();
    for (const GoldenValue &golden : goldenValues()) {
        if (golden.name.type == DependencyKey::Type::Attribute)
            session.getAttribute(golden.name.attributeKey);
        else
            session.getMeasurement(golden.name.measurementKey.first,
                                   golden.name.measurementKey.second);
    }

    QVERIFY(!session.hasMeasurement("GNSS", "z"));
    QVERIFY(!session.hasAttribute("_EXIT_TIME"));
    QVERIFY(!session.hasMeasurement("Simplified", "lat"));
    QVERIFY(!session.sensorKeys().contains(QStringLiteral("Simplified")));
    QVERIFY(session.hasMeasurement("GNSS", "hMSL"));
    QVERIFY(session.hasAttribute("SESSION_ID"));
}

FLYSIGHT_TEST_MAIN(BuiltinsGoldenTest)
#include "tst_builtins_golden.moc"
