// Stored requested-calculation results: the record format and its storage in
// the logbook.
//
//  - the record file name: the percent-encoding of a calculation id (canonical,
//    dot-free, injective under case folding) and the parse of a file name;
//  - the code stamps (calculation-compatibility marker and environment
//    fingerprint), computed fresh;
//  - the binary codec: a bit-exact round trip (-0, NaN payloads, infinities,
//    subnormals, null / empty / non-ASCII strings, unavailable outputs, output
//    order), a round trip of every attribute type a record accepts, the pinned
//    byte layout, refusal of other format versions, of damaged and of
//    hand-crafted inconsistent payloads (without allocating what a crafted
//    count claims), refusal at encode of every other attribute type (Long and
//    ULong included: their width is not portable), and the size;
//  - LogbookManager's record files on a real temporary logbook: write, read,
//    replace, list, remove, both write failures (a refused encoding, a
//    directory at the record's path) leaving the previous state intact,
//    removal with the session (dotted identity stems included), the stray pass
//    of initialize() in all three index branches, orphan adoption, remap, and a
//    session save that never depends on records.
//
// Expected values are literals, never produced by the code under test. Nothing
// depends on permission bits, on the platform's case sensitivity or on
// directory iteration order (listings are sorted).

#include <QtTest>

#include <cfloat>
#include <climits>
#include <cstring>
#include <functional>
#include <limits>

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QMap>
#include <QPointF>

#include "calculationrecord.h"
#include "calculations/builtincalculations.h"
#include "engine/calculationregistry.h"
#include "engine/storedcalculationresult.h"
#include "fixturebuilder.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "sessiondata.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

const char kExtraId[] = "test.resultrecords.extra";
const char kFitId[] = "builtin.fusion.fit";
const char kFitSuffix[] = ".builtin%2Efusion%2Efit.fvresult";

// A small session that already carries the four attributes loadSession()
// would otherwise backfill (as tst_logbook_index).
SessionData makeSession(const QString &id)
{
    SessionData s;
    s.setAttribute("SESSION_ID", id);
    s.setAttribute("DEVICE_ID", QStringLiteral("test-device"));
    s.setAttribute("_DESCRIPTION", QStringLiteral("first"));
    s.setAttribute("_JUMPER_MASS", 80.0);
    s.setAttribute("_PLANFORM_AREA", 2.0);
    s.setAttribute("_WIND_N", 0.0);
    s.setAttribute("_WIND_E", 0.0);
    s.setSourceMeasurement("IMU", "time", {10.0, 20.0, 30.0}, "s");
    s.setSourceMeasurement("IMU", "wx", {1.0, 2.0, 3.0}, "deg/s");
    return s;
}

double fromBits(quint64 bits)
{
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

quint64 bitsOf(double v)
{
    quint64 bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return bits;
}

// A quiet NaN with the sign bit set and a non-default payload.
const quint64 kSignedPayloadNaN = Q_UINT64_C(0xFFF800000000ABCD);

// 0.0, -0.0, a quiet NaN, a signed NaN with a payload, +inf, -inf, a
// subnormal, DBL_MAX and 0.1.
QVector<double> specialDoubles()
{
    return {0.0, -0.0, std::numeric_limits<double>::quiet_NaN(), fromBits(kSignedPayloadNaN),
            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
            1e-320, DBL_MAX, 0.1};
}

QString stringWithNul()
{
    QString s = QStringLiteral("a");
    s.append(QChar(0));
    s.append(QStringLiteral("b"));
    return s;
}

// Every value the bit-exact round trip covers. `scale` changes the samples of
// the first measurement only, so two snapshots can differ.
StoredCalculationResult sampleSnapshot(const QString &id = QString::fromLatin1(kFitId), double scale = 1.0)
{
    StoredCalculationResult r;
    r.calculationId = id;
    r.resultVersion = QStringLiteral("batch-temperature-bias-v3");
    r.inputFingerprint = QCryptographicHash::hash("sample inputs", QCryptographicHash::Sha256);
    r.leaves = {GraphNode::storedAttribute(QStringLiteral("_JUMPER_MASS")),
                GraphNode::sourceMeasurement(QStringLiteral("IMU"), QStringLiteral("wx")),
                GraphNode::sourceUnit(QStringLiteral("IMU"), QStringLiteral("wx")),
                GraphNode::preference(QStringLiteral("fusion/test"))};

    r.bundle.setMeasurement(QStringLiteral("Fusion"), QStringLiteral("scaled"),
                            {1.5 * scale, -2.25 * scale, 3.0 * scale}, QStringLiteral("m/s"));
    r.bundle.setMeasurement(QStringLiteral("Fusion"), QStringLiteral("special"), specialDoubles(),
                            QStringLiteral("deg"));
    r.bundle.setMeasurement(QStringLiteral("Fusion"), QStringLiteral("emptyUnit"), {1.0}, QStringLiteral(""));
    r.bundle.setMeasurement(QStringLiteral("Fusion"), QStringLiteral("nullUnit"), {2.0});

    r.bundle.setAttribute(QStringLiteral("_S_NONASCII"), QStringLiteral("héllo wörld ✓"));
    r.bundle.setAttribute(QStringLiteral("_S_SURROGATE"), QString::fromUtf8("smile \xF0\x9F\x98\x80"));
    r.bundle.setAttribute(QStringLiteral("_S_CRLF"), QStringLiteral("line1\r\nline2"));
    r.bundle.setAttribute(QStringLiteral("_S_NUL"), stringWithNul());
    r.bundle.setAttribute(QStringLiteral("_S_EMPTY"), QStringLiteral(""));
    r.bundle.setAttribute(QStringLiteral("_S_NULL"), QString());
    r.bundle.setAttribute(QStringLiteral("_D_NEGZERO"), -0.0);
    r.bundle.setAttribute(QStringLiteral("_D_NAN"), fromBits(kSignedPayloadNaN));
    r.bundle.setAttribute(QStringLiteral("_D_TENTH"), 0.1);
    r.bundle.setAttribute(QStringLiteral("_LL"), qlonglong(-9007199254740993LL));
    r.bundle.setAttribute(QStringLiteral("_I"), int(-42));
    r.bundle.setAttribute(QStringLiteral("_B"), true);
    r.bundle.setAttribute(QStringLiteral("_BYTES"), QByteArray("raw\x01", 4));

    const QString reason = QStringLiteral("fit converged after 7 passes – °");
    r.bundle.setReason(reason);
    r.detail = reason;
    return r;
}

CalculationRecord recordFor(const QString &id, double scale = 1.0)
{
    return CalculationRecord::stamped(sampleSnapshot(id, scale));
}

QByteArray encoded(const CalculationRecord &record)
{
    QString error;
    const std::optional<QByteArray> bytes = encodeCalculationRecord(record, &error);
    if (!bytes)
        qWarning("tst_result_records: encoding failed: %s", qPrintable(error));
    return bytes.value_or(QByteArray());
}

QString sameStringDifference(const QString &what, const QString &a, const QString &b)
{
    if (a != b || a.toUtf8() != b.toUtf8())
        return QStringLiteral("%1: '%2' != '%3'").arg(what, a, b);
    if (a.isNull() != b.isNull())
        return QStringLiteral("%1: null %2 != %3").arg(what).arg(a.isNull()).arg(b.isNull());
    return QString();
}

QString variantDifference(const QString &what, const QVariant &a, const QVariant &b)
{
    if (a.typeId() != b.typeId())
        return QStringLiteral("%1: type %2 != %3").arg(what, QString::fromLatin1(a.metaType().name()),
                                                        QString::fromLatin1(b.metaType().name()));
    switch (a.typeId()) {
    case QMetaType::Double:
        if (!sameBits(a.toDouble(), b.toDouble()))
            return QStringLiteral("%1: double bits differ").arg(what);
        return QString();
    case QMetaType::Float: {
        const float fa = a.value<float>();
        const float fb = b.value<float>();
        if (std::memcmp(&fa, &fb, sizeof fa) != 0)
            return QStringLiteral("%1: float bits differ").arg(what);
        return QString();
    }
    case QMetaType::QString:
        return sameStringDifference(what, a.toString(), b.toString());
    default:
        if (a != b)
            return QStringLiteral("%1: values differ").arg(what);
        return QString();
    }
}

// Empty when the two records are equal in every field; otherwise what differs.
QString recordDifference(const CalculationRecord &a, const CalculationRecord &b)
{
    if (a.calculationCompatibility != b.calculationCompatibility)
        return QStringLiteral("compatibility %1 != %2").arg(a.calculationCompatibility).arg(b.calculationCompatibility);
    QString d = sameStringDifference(QStringLiteral("environment"), a.calculationEnvironment, b.calculationEnvironment);
    if (!d.isEmpty())
        return d;

    const StoredCalculationResult &x = a.result;
    const StoredCalculationResult &y = b.result;
    if (!(d = sameStringDifference(QStringLiteral("id"), x.calculationId, y.calculationId)).isEmpty()
        || !(d = sameStringDifference(QStringLiteral("result version"), x.resultVersion, y.resultVersion)).isEmpty()
        || !(d = sameStringDifference(QStringLiteral("detail"), x.detail, y.detail)).isEmpty()
        || !(d = sameStringDifference(QStringLiteral("reason"), x.bundle.reason(), y.bundle.reason())).isEmpty())
        return d;
    if (x.inputFingerprint != y.inputFingerprint)
        return QStringLiteral("fingerprint differs");

    if (x.leaves.size() != y.leaves.size())
        return QStringLiteral("leaf count %1 != %2").arg(x.leaves.size()).arg(y.leaves.size());
    for (qsizetype i = 0; i < x.leaves.size(); ++i) {
        const GraphNode &l = x.leaves[i];
        const GraphNode &r = y.leaves[i];
        if (l.kind != r.kind || l.measurementName != r.measurementName)
            return QStringLiteral("leaf %1: kind differs").arg(i);
        if (!(d = sameStringDifference(QStringLiteral("leaf %1 a").arg(i), l.a, r.a)).isEmpty()
            || !(d = sameStringDifference(QStringLiteral("leaf %1 b").arg(i), l.b, r.b)).isEmpty())
            return d;
    }

    const QList<DependencyKey> outputs = x.bundle.setOutputs();
    if (outputs != y.bundle.setOutputs())
        return QStringLiteral("output order differs");
    for (const DependencyKey &key : outputs) {
        const bool isAttribute = key.type == DependencyKey::Type::Attribute;
        const QString name = isAttribute ? key.attributeKey
                                         : key.measurementKey.first + QLatin1Char('/') + key.measurementKey.second;
        if (!y.bundle.contains(key))
            return QStringLiteral("%1 missing").arg(name);
        if (x.bundle.isAvailable(key) != y.bundle.isAvailable(key))
            return QStringLiteral("%1: availability differs").arg(name);
        if (isAttribute) {
            d = variantDifference(name, x.bundle.attributeValue(key.attributeKey),
                                  y.bundle.attributeValue(key.attributeKey));
        } else {
            const QString &sensor = key.measurementKey.first;
            const QString &m = key.measurementKey.second;
            if (!sameBitsEverywhere(x.bundle.measurementValues(sensor, m), y.bundle.measurementValues(sensor, m)))
                return QStringLiteral("%1: samples differ").arg(name);
            d = sameStringDifference(name + QStringLiteral(" unit"), x.bundle.measurementUnit(sensor, m),
                                     y.bundle.measurementUnit(sensor, m));
        }
        if (!d.isEmpty())
            return d;
    }
    return QString();
}

// The attribute types a record accepts: the list of calculationrecord.h,
// spelled out here, never taken from the code under test.
const QList<int> kRecordableTypes = {
    QMetaType::QString, QMetaType::QByteArray, QMetaType::Bool, QMetaType::Double, QMetaType::Float,
    QMetaType::Int, QMetaType::UInt, QMetaType::LongLong, QMetaType::ULongLong, QMetaType::Short,
    QMetaType::UShort, QMetaType::Char, QMetaType::SChar, QMetaType::UChar};

// A snapshot whose only output is the attribute "_A" = value.
StoredCalculationResult attributeSnapshot(const QVariant &value)
{
    StoredCalculationResult r;
    r.calculationId = QStringLiteral("x.y");
    r.inputFingerprint = QByteArray(32, '\x22');
    r.bundle.setAttribute(QStringLiteral("_A"), value);
    return r;
}

float floatFromBits(quint32 bits)
{
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

// ---- hand-crafted payloads --------------------------------------------------

void pin(QDataStream &s)
{
    s.setVersion(QDataStream::Qt_6_0);
    s.setByteOrder(QDataStream::LittleEndian);
    s.setFloatingPointPrecision(QDataStream::DoublePrecision);
}

// A record whose header is valid (magic, version 1, stamps 7 / "e", id "x.y",
// version "v1", null reason, 32-byte fingerprint), with `body` writing the
// leaves and outputs, and a correct SHA-256 trailer.
QByteArray craft(const std::function<void(QDataStream &)> &body)
{
    QByteArray bytes;
    {
        QDataStream s(&bytes, QIODevice::WriteOnly);
        pin(s);
        s.writeRawData("FVRESULT", 8);
        s << quint32(1) << qint32(7) << QStringLiteral("e") << QStringLiteral("x.y") << QStringLiteral("v1")
          << QString() << QByteArray(32, '\xAB');
        body(s);
    }
    bytes.append(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
    return bytes;
}

QByteArray withChecksum(QByteArray bytes)
{
    bytes.chop(32);
    bytes.append(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
    return bytes;
}

QByteArray patchVersion(QByteArray bytes, quint32 version)
{
    for (int i = 0; i < 4; ++i)
        bytes[8 + i] = char((version >> (8 * i)) & 0xFF);
    return bytes;
}

// A record with one measurement of 1000 samples: its middle byte is a sample byte.
CalculationRecord largeSampleRecord()
{
    StoredCalculationResult r;
    r.calculationId = QStringLiteral("x.y");
    r.inputFingerprint = QByteArray(32, '\x11');
    QVector<double> samples;
    for (int i = 0; i < 1000; ++i)
        samples.append(i * 0.5);
    r.bundle.setMeasurement(QStringLiteral("S"), QStringLiteral("m"), samples, QStringLiteral("u"));
    CalculationRecord record;
    record.calculationCompatibility = 7;
    record.calculationEnvironment = QStringLiteral("e");
    record.result = r;
    return record;
}

// Names and bytes of every file in a directory.
QMap<QString, QByteArray> directoryContents(const QString &dir)
{
    QMap<QString, QByteArray> contents;
    const QStringList names = QDir(dir).entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
    for (const QString &name : names)
        contents.insert(name, readFileBytes(dir + QLatin1Char('/') + name));
    return contents;
}

QStringList sorted(QStringList list)
{
    list.sort();
    return list;
}

} // namespace

class ResultRecordsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // Record format (Tasks 2.1, 2.2)
    void fileIdEncoding_data();
    void fileIdEncoding();
    void fileNameParsing_data();
    void fileNameParsing();
    void fileNamesDifferIgnoringCase();
    void stampsAreCurrent();
    void roundTripIsBitExact();
    void attributeTypesRoundTrip_data();
    void attributeTypesRoundTrip();
    void unavailableAndEmptyOutputs();
    void rejectionShapedRecord();
    void layoutIsPinned();
    void futureVersionIsRefused_data();
    void futureVersionIsRefused();
    void corruptInputIsRefused_data();
    void corruptInputIsRefused();
    void encoderRefusesUnsupportedAttribute();
    void sizeIsOrderOfSamples();

    // Logbook storage (Tasks 2.3, 2.4)
    void writeReadReplace();
    void readStatuses();
    void writeFailureRefusedEncoding();
    void writeFailureDirectoryAtPath();
    void writeForUnknownSession();
    void removeOneAndAll();
    void removeSessionDeletesRecords();
    void removeSessionDottedStems();
    void failedSessionRemovalKeepsRecords();
    void strayRecordsRemovedAtScan_data();
    void strayRecordsRemovedAtScan();
    void orphanAdoptionKeepsRecord();
    void recordsFollowRemap();
    void saveSessionIgnoresRecords();

private:
    // Saves the session, flushes the index and returns its file stem.
    QString saveIndexed(const QString &sessionId);
};

void ResultRecordsTest::initTestCase()
{
    // The stamp helpers need the real registry.
    TestEnvironment::instance().registerBuiltIns();
}

void ResultRecordsTest::init()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();
}

void ResultRecordsTest::cleanup()
{
    CalculationRegistry::instance().unregister(QString::fromLatin1(kExtraId));
}

QString ResultRecordsTest::saveIndexed(const QString &sessionId)
{
    LogbookManager &logbook = LogbookManager::instance();
    if (!logbook.saveSession(makeSession(sessionId)) || !logbook.flushIndex())
        return QString();
    return sessionFileStem(sessionId);
}

// ============================================================================
// Record format
// ============================================================================

void ResultRecordsTest::fileIdEncoding_data()
{
    QTest::addColumn<QString>("id");
    QTest::addColumn<QString>("encoded");
    QTest::addColumn<bool>("canonical");

    QTest::newRow("fusion") << QStringLiteral("builtin.fusion.fit") << QStringLiteral("builtin%2Efusion%2Efit") << true;
    QTest::newRow("family instance") << QString::fromUtf8("x#Key/\xC3\xA9") << QStringLiteral("x%23%4Bey%2F%C3%A9")
                                     << true;
    QTest::newRow("upper case") << QStringLiteral("A") << QStringLiteral("%41") << true;
    QTest::newRow("lower case") << QStringLiteral("a") << QStringLiteral("a") << true;
    QTest::newRow("literal set") << QStringLiteral("az09_-") << QStringLiteral("az09_-") << true;
    QTest::newRow("percent") << QStringLiteral("%") << QStringLiteral("%25") << true;
    QTest::newRow("path and shell") << QStringLiteral("a b\\:*?\"<>|")
                                    << QStringLiteral("a%20b%5C%3A%2A%3F%22%3C%3E%7C") << true;
    QTest::newRow("surrogate pair") << QString::fromUtf8("\xF0\x9F\x98\x80") << QStringLiteral("%F0%9F%98%80")
                                    << true;
    QTest::newRow("embedded nul") << stringWithNul() << QStringLiteral("a%00b") << true;

    // Refused by the decoder
    QTest::newRow("empty") << QString() << QStringLiteral("") << false;
    QTest::newRow("dot") << QString() << QStringLiteral("a.b") << false;
    QTest::newRow("lower-case hex") << QString() << QStringLiteral("%2e") << false;
    QTest::newRow("literal upper case") << QString() << QStringLiteral("A") << false;
    QTest::newRow("not hex") << QString() << QStringLiteral("%G1") << false;
    QTest::newRow("short escape") << QString() << QStringLiteral("%2") << false;
    QTest::newRow("invalid utf-8") << QString() << QStringLiteral("%FF") << false;
    QTest::newRow("truncated utf-8") << QString() << QStringLiteral("%C3") << false;
    QTest::newRow("escaped literal") << QString() << QStringLiteral("%61") << false;
}

void ResultRecordsTest::fileIdEncoding()
{
    QFETCH(QString, id);
    QFETCH(QString, encoded);
    QFETCH(bool, canonical);

    if (!canonical) {
        QVERIFY(!decodeRecordFileId(encoded).has_value());
        return;
    }

    QCOMPARE(encodeRecordFileId(id), encoded);
    QVERIFY(!encoded.contains(QLatin1Char('.')));
    const std::optional<QString> decoded = decodeRecordFileId(encoded);
    QVERIFY(decoded.has_value());
    QCOMPARE(*decoded, id);
}

void ResultRecordsTest::fileNameParsing_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<QString>("stem");
    QTest::addColumn<QString>("id");

    QTest::newRow("dotted stem") << QStringLiteral("a.b.x%2Ey.fvresult") << true << QStringLiteral("a.b")
                                 << QStringLiteral("x.y");
    // The extension is compared case-sensitively on every platform
    QTest::newRow("upper-case extension") << QStringLiteral("s.x.FVRESULT") << false << QString() << QString();
    QTest::newRow("mixed-case extension") << QStringLiteral("s.x.fvResult") << false << QString() << QString();
    QTest::newRow("fusion") << QStringLiteral("3f2c-9a1e.builtin%2Efusion%2Efit.fvresult") << true
                            << QStringLiteral("3f2c-9a1e") << QStringLiteral("builtin.fusion.fit");
    QTest::newRow("no id") << QStringLiteral("x.fvresult") << false << QString() << QString();
    QTest::newRow("empty stem") << QStringLiteral(".x.fvresult") << false << QString() << QString();
    QTest::newRow("csv") << QStringLiteral("s.x.csv") << false << QString() << QString();
    QTest::newRow("save temporary") << QStringLiteral("s.x.fvresult.Ab12Cd") << false << QString() << QString();
    QTest::newRow("empty id") << QStringLiteral("s..fvresult") << false << QString() << QString();
    QTest::newRow("id not canonical") << QStringLiteral("s.A.fvresult") << false << QString() << QString();
}

void ResultRecordsTest::fileNameParsing()
{
    QFETCH(QString, fileName);
    QFETCH(bool, valid);
    QFETCH(QString, stem);
    QFETCH(QString, id);

    const std::optional<std::pair<QString, QString>> parsed = parseRecordFileName(fileName);
    QCOMPARE(parsed.has_value(), valid);
    if (!valid)
        return;
    QCOMPARE(parsed->first, stem);
    QCOMPARE(parsed->second, id);
    QCOMPARE(recordFileName(stem, id), fileName);
}

void ResultRecordsTest::fileNamesDifferIgnoringCase()
{
    QCOMPARE(calculationRecordExtension(), QStringLiteral("fvresult"));
    QCOMPARE(recordFileName(QStringLiteral("3f2c-9a1e"), QString::fromLatin1(kFitId)),
             QStringLiteral("3f2c-9a1e.builtin%2Efusion%2Efit.fvresult"));

    const QString upper = recordFileName(QStringLiteral("s"), QStringLiteral("A"));
    const QString lower = recordFileName(QStringLiteral("s"), QStringLiteral("a"));
    QCOMPARE(upper, QStringLiteral("s.%41.fvresult"));
    QCOMPARE(lower, QStringLiteral("s.a.fvresult"));
    QVERIFY(upper.compare(lower, Qt::CaseInsensitive) != 0);
}

void ResultRecordsTest::stampsAreCurrent()
{
    const CalculationRecord record = CalculationRecord::stamped(sampleSnapshot());
    QVERIFY(record.stampsAreCurrent());
    QCOMPARE(record.calculationCompatibility, 2);
    QCOMPARE(record.calculationEnvironment, calculationEnvironmentFingerprint());
    QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{40}$")).match(record.calculationEnvironment).hasMatch());

    CalculationRecord other = record;
    other.calculationCompatibility = 3;
    QVERIFY(!other.stampsAreCurrent());

    other = record;
    other.calculationEnvironment = QString(40, QLatin1Char('0'));
    QVERIFY(!other.stampsAreCurrent());

    // Computed fresh: a registration after the record was stamped makes it stale.
    CalculationDescriptor extra;
    extra.id = QString::fromLatin1(kExtraId);
    extra.outputs = {DependencyKey::attribute(QStringLiteral("_TEST_RESULTRECORDS_EXTRA"))};
    extra.compute = [](const EvaluationContext &) { return CalculationResult(); };
    QVERIFY(CalculationRegistry::instance().registerCalculation(extra));
    QVERIFY(!record.stampsAreCurrent());
    QVERIFY(CalculationRecord::stamped(sampleSnapshot()).stampsAreCurrent());
}

void ResultRecordsTest::roundTripIsBitExact()
{
    const CalculationRecord record = CalculationRecord::stamped(sampleSnapshot());
    const QByteArray bytes = encoded(record);
    QVERIFY(!bytes.isEmpty());

    CalculationRecord decoded;
    QString error = QStringLiteral("stale");
    QCOMPARE(decodeCalculationRecord(bytes, &decoded, &error), CalculationRecordStatus::Ok);
    QVERIFY(error.isEmpty());
    const QString difference = recordDifference(decoded, record);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // Spelled out, independent of the helper
    const CalculationResult &bundle = decoded.result.bundle;
    const QVector<double> special = bundle.measurementValues(QStringLiteral("Fusion"), QStringLiteral("special"));
    QVERIFY(sameBitsEverywhere(special, specialDoubles()));
    QCOMPARE(special.size(), 9);
    QCOMPARE(bitsOf(special[1]), Q_UINT64_C(0x8000000000000000));
    QCOMPARE(bitsOf(special[3]), kSignedPayloadNaN);
    QCOMPARE(bitsOf(special[6]), bitsOf(1e-320));

    QCOMPARE(bundle.measurementUnit(QStringLiteral("Fusion"), QStringLiteral("special")), QStringLiteral("deg"));
    const QString emptyUnit = bundle.measurementUnit(QStringLiteral("Fusion"), QStringLiteral("emptyUnit"));
    QVERIFY(emptyUnit.isEmpty() && !emptyUnit.isNull());
    QVERIFY(bundle.measurementUnit(QStringLiteral("Fusion"), QStringLiteral("nullUnit")).isNull());

    QCOMPARE(bundle.attributeValue(QStringLiteral("_S_NUL")).toString().size(), 3);
    QCOMPARE(bundle.attributeValue(QStringLiteral("_S_NUL")).toString().toUtf8(), QByteArray("a\0b", 3));
    QCOMPARE(bundle.attributeValue(QStringLiteral("_S_SURROGATE")).toString().toUtf8(),
             QByteArray("smile \xF0\x9F\x98\x80"));
    const QString empty = bundle.attributeValue(QStringLiteral("_S_EMPTY")).toString();
    QVERIFY(empty.isEmpty() && !empty.isNull());
    QVERIFY(bundle.isAvailable(DependencyKey::attribute(QStringLiteral("_S_NULL"))));
    QVERIFY(bundle.attributeValue(QStringLiteral("_S_NULL")).toString().isNull());

    QCOMPARE(bundle.attributeValue(QStringLiteral("_D_NEGZERO")).typeId(), int(QMetaType::Double));
    QCOMPARE(bitsOf(bundle.attributeValue(QStringLiteral("_D_NEGZERO")).toDouble()), Q_UINT64_C(0x8000000000000000));
    QCOMPARE(bitsOf(bundle.attributeValue(QStringLiteral("_D_NAN")).toDouble()), kSignedPayloadNaN);
    QCOMPARE(bundle.attributeValue(QStringLiteral("_LL")).typeId(), int(QMetaType::LongLong));
    QCOMPARE(bundle.attributeValue(QStringLiteral("_LL")).toLongLong(), -9007199254740993LL);
    QCOMPARE(bundle.attributeValue(QStringLiteral("_I")).typeId(), int(QMetaType::Int));
    QCOMPARE(bundle.attributeValue(QStringLiteral("_B")).typeId(), int(QMetaType::Bool));
    QCOMPARE(bundle.attributeValue(QStringLiteral("_BYTES")).toByteArray(), QByteArray("raw\x01", 4));

    // Leaves (all four kinds, in order), stamps, version
    QCOMPARE(decoded.result.leaves.size(), 4);
    QCOMPARE(decoded.result.leaves[1], GraphNode::sourceMeasurement(QStringLiteral("IMU"), QStringLiteral("wx")));
    QCOMPARE(decoded.result.leaves[3], GraphNode::preference(QStringLiteral("fusion/test")));
    QCOMPARE(decoded.calculationCompatibility, CalculationCompatibilityVersion);
    QCOMPARE(decoded.result.resultVersion, QStringLiteral("batch-temperature-bias-v3"));
    QCOMPARE(decoded.result.detail, decoded.result.bundle.reason());

    // The decoded record encodes to the same bytes
    QCOMPARE(encoded(decoded), bytes);
}

void ResultRecordsTest::attributeTypesRoundTrip_data()
{
    QTest::addColumn<QVariant>("value");

    QTest::newRow("QString") << QVariant(QString::fromUtf8("h\xC3\xA9llo"));
    QTest::newRow("QString null") << QVariant(QString());
    QTest::newRow("QByteArray") << QVariant(QByteArray("a\0\xFF", 3));
    QTest::newRow("QByteArray empty") << QVariant(QByteArray(""));
    QTest::newRow("Bool") << QVariant(false);
    QTest::newRow("Double -0") << QVariant(-0.0);
    QTest::newRow("Double NaN payload") << QVariant(fromBits(kSignedPayloadNaN));
    QTest::newRow("Double subnormal") << QVariant(1e-320);
    QTest::newRow("Float") << QVariant::fromValue(1.1f);
    QTest::newRow("Float -0") << QVariant::fromValue(-0.0f);
    QTest::newRow("Float -inf") << QVariant::fromValue(-std::numeric_limits<float>::infinity());
    QTest::newRow("Float NaN payload") << QVariant::fromValue(floatFromBits(0x7FC0ABCDu));
    QTest::newRow("Float subnormal") << QVariant::fromValue(floatFromBits(0x00000001u));
    QTest::newRow("Float max") << QVariant::fromValue(FLT_MAX);
    QTest::newRow("Int") << QVariant::fromValue(int(INT_MIN));
    QTest::newRow("UInt") << QVariant::fromValue(uint(UINT_MAX));
    QTest::newRow("LongLong") << QVariant::fromValue(qlonglong(LLONG_MIN));
    QTest::newRow("ULongLong") << QVariant::fromValue(qulonglong(ULLONG_MAX));
    QTest::newRow("Short") << QVariant::fromValue(short(SHRT_MIN));
    QTest::newRow("UShort") << QVariant::fromValue(ushort(USHRT_MAX));
    QTest::newRow("Char") << QVariant::fromValue(char(0x7F));
    QTest::newRow("SChar") << QVariant::fromValue(static_cast<signed char>(-128));
    QTest::newRow("UChar") << QVariant::fromValue(static_cast<uchar>(255));
}

// Every accepted type comes back as the same type with the same bits (a null
// string as a null string), and the decoded record encodes to the same bytes.
void ResultRecordsTest::attributeTypesRoundTrip()
{
    QFETCH(QVariant, value);
    QVERIFY(kRecordableTypes.contains(value.typeId()));

    const CalculationRecord record = CalculationRecord::stamped(attributeSnapshot(value));
    const QByteArray bytes = encoded(record);
    QVERIFY(!bytes.isEmpty());
    CalculationRecord decoded;
    QString error;
    QCOMPARE(decodeCalculationRecord(bytes, &decoded, &error), CalculationRecordStatus::Ok);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const QVariant back = decoded.result.bundle.attributeValue(QStringLiteral("_A"));
    QCOMPARE(back.typeId(), value.typeId());
    const QString difference = variantDifference(QStringLiteral("_A"), back, value);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    if (value.typeId() == QMetaType::QByteArray) {
        QCOMPARE(back.toByteArray(), value.toByteArray());
        QCOMPARE(back.toByteArray().isNull(), value.toByteArray().isNull());
    }
    QCOMPARE(encoded(decoded), bytes);
}

void ResultRecordsTest::unavailableAndEmptyOutputs()
{
    StoredCalculationResult r;
    r.calculationId = QStringLiteral("test.unavailable");
    r.inputFingerprint = QByteArray(32, '\x01');
    r.bundle.setUnavailable(DependencyKey::attribute(QStringLiteral("_A")));
    r.bundle.setMeasurement(QStringLiteral("S"), QStringLiteral("present"), {1.0}, QStringLiteral("u"));
    r.bundle.setUnavailable(DependencyKey::measurement(QStringLiteral("S"), QStringLiteral("absent")));
    r.bundle.setMeasurement(QStringLiteral("S"), QStringLiteral("normalized"), {});   // unavailable
    r.bundle.setAttribute(QStringLiteral("_INVALID"), QVariant());                     // unavailable
    CalculationRecord record;
    record.calculationCompatibility = 5;
    record.calculationEnvironment = QStringLiteral("env");
    record.result = r;

    CalculationRecord decoded;
    QCOMPARE(decodeCalculationRecord(encoded(record), &decoded), CalculationRecordStatus::Ok);
    const QString difference = recordDifference(decoded, record);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    const CalculationResult &bundle = decoded.result.bundle;
    const QList<DependencyKey> order = {
        DependencyKey::attribute(QStringLiteral("_A")),
        DependencyKey::measurement(QStringLiteral("S"), QStringLiteral("present")),
        DependencyKey::measurement(QStringLiteral("S"), QStringLiteral("absent")),
        DependencyKey::measurement(QStringLiteral("S"), QStringLiteral("normalized")),
        DependencyKey::attribute(QStringLiteral("_INVALID"))};
    QCOMPARE(bundle.setOutputs(), order);
    for (const DependencyKey &key : order) {
        QVERIFY(bundle.contains(key));
        QCOMPARE(bundle.isAvailable(key), key == order[1]);
    }

    // Empty result version and empty (null) reason
    QVERIFY(decoded.result.resultVersion.isEmpty());
    QVERIFY(decoded.result.bundle.reason().isNull());
    QVERIFY(decoded.result.detail.isNull());
    QCOMPARE(decoded.calculationCompatibility, 5);
    QCOMPARE(decoded.calculationEnvironment, QStringLiteral("env"));
}

void ResultRecordsTest::rejectionShapedRecord()
{
    // A rejection / solver failure: Ok with a reason and the diagnostics, no measurements
    StoredCalculationResult r;
    r.calculationId = QString::fromLatin1(kFitId);
    r.resultVersion = QStringLiteral("batch-temperature-bias-v3");
    r.inputFingerprint = QByteArray(32, '\x02');
    r.leaves = {GraphNode::sourceMeasurement(QStringLiteral("IMU"), QStringLiteral("ax"))};
    r.bundle.setAttribute(QStringLiteral("_FUSION_DIAGNOSTICS"),
                          QStringLiteral("{\"outcome\": \"rejected\", \"reason\": \"IMU gap of 2.3 s\"}"));
    r.bundle.setUnavailable(DependencyKey::measurement(QStringLiteral("Fusion"), QStringLiteral("roll")));
    r.bundle.setReason(QStringLiteral("IMU gap of 2.3 s at 14:02:11"));
    r.detail = r.bundle.reason();
    const CalculationRecord rejection = CalculationRecord::stamped(r);

    CalculationRecord decoded;
    QCOMPARE(decodeCalculationRecord(encoded(rejection), &decoded), CalculationRecordStatus::Ok);
    QString difference = recordDifference(decoded, rejection);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QCOMPARE(decoded.result.bundle.reason(), QStringLiteral("IMU gap of 2.3 s at 14:02:11"));
    QCOMPARE(decoded.result.detail, QStringLiteral("IMU gap of 2.3 s at 14:02:11"));

    // An empty bundle: no outputs, empty reason
    StoredCalculationResult empty;
    empty.calculationId = QStringLiteral("test.empty");
    empty.resultVersion = QStringLiteral("");
    empty.inputFingerprint = QByteArray(32, '\0');
    empty.bundle.setReason(QStringLiteral(""));
    empty.detail = QStringLiteral("");
    CalculationRecord emptyRecord = CalculationRecord::stamped(empty);

    CalculationRecord decodedEmpty;
    QCOMPARE(decodeCalculationRecord(encoded(emptyRecord), &decodedEmpty), CalculationRecordStatus::Ok);
    difference = recordDifference(decodedEmpty, emptyRecord);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QVERIFY(decodedEmpty.result.bundle.setOutputs().isEmpty());
    QVERIFY(decodedEmpty.result.leaves.isEmpty());
    QVERIFY(decodedEmpty.result.bundle.reason().isEmpty() && !decodedEmpty.result.bundle.reason().isNull());
}

// The bytes of a small record, written out by hand from the layout table of
// the phase document (format version 1).
void ResultRecordsTest::layoutIsPinned()
{
    StoredCalculationResult r;
    r.calculationId = QStringLiteral("x.y");
    r.resultVersion = QStringLiteral("v1");
    r.inputFingerprint = QByteArray(32, '\xAB');
    r.leaves = {GraphNode::sourceMeasurement(QStringLiteral("S"), QStringLiteral("t"))};
    r.bundle.setMeasurement(QStringLiteral("S"), QStringLiteral("m"), {1.0, -0.0}, QStringLiteral("u"));
    r.bundle.setUnavailable(DependencyKey::attribute(QStringLiteral("a")));
    CalculationRecord record;
    record.calculationCompatibility = 7;
    record.calculationEnvironment = QStringLiteral("e");
    record.result = r;

    const QByteArray expectedPrefix = QByteArray::fromHex(
        "4656524553554C54"                      // 1  magic "FVRESULT"
        "01000000"                              // 2  format version 1
        "07000000"                              // 3  calculation compatibility 7
        "02000000" "6500"                       // 4  environment "e"
        "06000000" "78002E007900"               // 5  calculation id "x.y"
        "04000000" "76003100"                   // 6  result version "v1"
        "FFFFFFFF"                              // 7  reason: null string
        "20000000"                              // 8  fingerprint: 32 bytes of 0xAB
        "ABABABABABABABABABABABABABABABAB"
        "ABABABABABABABABABABABABABABABAB"
        "01000000"                              // 9  one leaf
        "01" "02000000" "5300" "02000000" "7400"    // 9a SourceMeasurement "S" "t"
        "02000000"                              // 10 two outputs
        "02" "02000000" "5300" "02000000" "6D00"    // 10a measurement "S" "m"
        "01"                                        //     available
        "02000000"                                  //     two samples
        "000000000000F03F"                          //     1.0
        "0000000000000080"                          //     -0.0
        "02000000" "7500"                           //     unit "u"
        "01" "02000000" "6100" "FFFFFFFF"           // 10a attribute "a", second null
        "00");                                      //     unavailable

    const QByteArray bytes = encoded(record);
    QCOMPARE(bytes.size(), expectedPrefix.size() + 32);
    QCOMPARE(bytes.first(bytes.size() - 32).toHex(), expectedPrefix.toHex());
    QCOMPARE(bytes.last(32), QCryptographicHash::hash(bytes.first(bytes.size() - 32), QCryptographicHash::Sha256));

    CalculationRecord decoded;
    QCOMPARE(decodeCalculationRecord(bytes, &decoded), CalculationRecordStatus::Ok);
    const QString difference = recordDifference(decoded, record);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
}

void ResultRecordsTest::futureVersionIsRefused_data()
{
    QTest::addColumn<quint32>("version");
    QTest::addColumn<bool>("fixChecksum");

    const QList<quint32> versions = {0, 2, 3, 0xFFFFFFFFu};
    for (quint32 v : versions) {
        QTest::addRow("%u, stale checksum", v) << v << false;
        QTest::addRow("%u, matching checksum", v) << v << true;
    }
}

void ResultRecordsTest::futureVersionIsRefused()
{
    QFETCH(quint32, version);
    QFETCH(bool, fixChecksum);

    QByteArray bytes = patchVersion(encoded(recordFor(QStringLiteral("x"))), version);
    if (fixChecksum)
        bytes = withChecksum(bytes);

    CalculationRecord out;
    out.calculationCompatibility = 12345;
    QString error;
    QCOMPARE(decodeCalculationRecord(bytes, &out, &error), CalculationRecordStatus::UnsupportedVersion);
    QCOMPARE(error, QStringLiteral("format version %1 is not supported").arg(version));
    QCOMPARE(out.calculationCompatibility, 12345);
}

void ResultRecordsTest::corruptInputIsRefused_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("error");

    const int notARecord = int(CalculationRecordStatus::NotARecord);
    const int corrupt = int(CalculationRecordStatus::Corrupt);
    const QByteArray valid = encoded(largeSampleRecord());
    const qsizetype middle = valid.size() / 2;
    QVERIFY(middle > 200 && middle < valid.size() - 200);   // inside the 8000 sample bytes

    QTest::newRow("empty") << QByteArray() << notARecord << QStringLiteral("not a calculation record");
    QTest::newRow("7 bytes") << valid.first(7) << notARecord << QStringLiteral("not a calculation record");
    QByteArray wrongMagic = valid;
    wrongMagic[0] = 'X';
    QTest::newRow("wrong magic") << wrongMagic << notARecord << QStringLiteral("not a calculation record");

    QTest::newRow("truncated at 11") << valid.first(11) << corrupt << QStringLiteral("the record is truncated");
    QTest::newRow("truncated at 40") << valid.first(40) << corrupt << QStringLiteral("the record is truncated");
    QTest::newRow("truncated in the samples") << valid.first(middle) << corrupt << QStringLiteral("checksum mismatch");
    QTest::newRow("one byte short") << valid.first(valid.size() - 1) << corrupt << QStringLiteral("checksum mismatch");
    QByteArray flipped = valid;
    flipped[middle] = char(flipped[middle] ^ 0x10);
    QTest::newRow("flipped sample bit") << flipped << corrupt << QStringLiteral("checksum mismatch");
    QTest::newRow("extra trailing byte") << (valid + QByteArray(1, '\0')) << corrupt
                                         << QStringLiteral("checksum mismatch");

    // Hand-crafted payloads with a correct checksum
    const QString t = QStringLiteral("t");
    QTest::newRow("unknown leaf kind") << craft([&](QDataStream &s) {
        s << quint32(1) << quint8(9) << QStringLiteral("S") << t << quint32(0);
    }) << corrupt << QStringLiteral("unknown leaf kind 9");
    QTest::newRow("leaf count too large") << craft([&](QDataStream &s) {
        s << quint32(0xFFFFFFFFu) << quint8(0) << t << QString();
    }) << corrupt << QStringLiteral("leaf count");
    QTest::newRow("unknown key code") << craft([&](QDataStream &s) {
        s << quint32(0) << quint32(1) << quint8(3) << QStringLiteral("a") << QString() << false;
    }) << corrupt << QStringLiteral("unknown output kind 3");
    QTest::newRow("sample count 0x7FFFFFFF") << craft([&](QDataStream &s) {
        s << quint32(0) << quint32(1) << quint8(2) << QStringLiteral("S") << QStringLiteral("m") << true
          << quint32(0x7FFFFFFF) << 1.0;
    }) << corrupt << QStringLiteral("sample count");
    QTest::newRow("available measurement without samples") << craft([&](QDataStream &s) {
        s << quint32(0) << quint32(1) << quint8(2) << QStringLiteral("S") << QStringLiteral("m") << true
          << quint32(0) << QStringLiteral("u");
    }) << corrupt << QStringLiteral("has no samples");
    QTest::newRow("QVariantList attribute") << craft([&](QDataStream &s) {
        s << quint32(0) << quint32(1) << quint8(1) << QStringLiteral("a") << QString() << true
          << QVariant(QVariantList{1, 2});
    }) << corrupt << QStringLiteral("cannot hold");
    // Long and ULong are refused on read too, whatever the bytes hold
    QTest::newRow("Long attribute") << craft([&](QDataStream &s) {
        s << quint32(0) << quint32(1) << quint8(1) << QStringLiteral("a") << QString() << true
          << QVariant::fromValue(long(5));
    }) << corrupt << QStringLiteral("attribute 'a' has a type a record cannot hold");
    QTest::newRow("ULong attribute") << craft([&](QDataStream &s) {
        s << quint32(0) << quint32(1) << quint8(1) << QStringLiteral("a") << QString() << true
          << QVariant::fromValue(static_cast<unsigned long>(5));
    }) << corrupt << QStringLiteral("attribute 'a' has a type a record cannot hold");
    QTest::newRow("invalid attribute") << craft([&](QDataStream &s) {
        s << quint32(0) << quint32(1) << quint8(1) << QStringLiteral("a") << QString() << true << QVariant();
    }) << corrupt << QStringLiteral("cannot hold");
    QTest::newRow("duplicate output") << craft([&](QDataStream &s) {
        s << quint32(0) << quint32(2) << quint8(1) << QStringLiteral("a") << QString() << false
          << quint8(1) << QStringLiteral("a") << QString() << false;
    }) << corrupt << QStringLiteral("appears twice");
    QTest::newRow("bytes after the last output") << craft([&](QDataStream &s) {
        s << quint32(0) << quint32(1) << quint8(1) << QStringLiteral("a") << QString() << false << quint8(0);
    }) << corrupt << QStringLiteral("unexpected bytes after the last output");
    QTest::newRow("output count too large") << craft([&](QDataStream &s) {
        s << quint32(0) << quint32(1000);
    }) << corrupt << QStringLiteral("output count");
}

void ResultRecordsTest::corruptInputIsRefused()
{
    QFETCH(QByteArray, bytes);
    QFETCH(int, status);
    QFETCH(QString, error);

    CalculationRecord out;
    out.calculationCompatibility = 12345;
    out.calculationEnvironment = QStringLiteral("untouched");
    QString message;
    QCOMPARE(int(decodeCalculationRecord(bytes, &out, &message)), status);
    QVERIFY2(message.contains(error), qPrintable(message));

    QCOMPARE(out.calculationCompatibility, 12345);
    QCOMPARE(out.calculationEnvironment, QStringLiteral("untouched"));
    QVERIFY(out.result.calculationId.isEmpty());
    QVERIFY(out.result.bundle.setOutputs().isEmpty());
}

void ResultRecordsTest::encoderRefusesUnsupportedAttribute()
{
    QString error;

    StoredCalculationResult noId = sampleSnapshot(QString());
    QVERIFY(!encodeCalculationRecord(CalculationRecord::stamped(noId), &error).has_value());
    QCOMPARE(error, QStringLiteral("the record has no calculation id"));

    StoredCalculationResult list = sampleSnapshot();
    list.bundle.setAttribute(QStringLiteral("_X"), QVariantList{1, 2});
    QVERIFY(!encodeCalculationRecord(CalculationRecord::stamped(list), &error).has_value());
    QCOMPARE(error, QStringLiteral("attribute '_X' has a type a record cannot hold (QVariantList)"));

    StoredCalculationResult point = sampleSnapshot();
    point.bundle.setAttribute(QStringLiteral("_P"), QPointF(1.0, 2.0));
    QVERIFY(!encodeCalculationRecord(CalculationRecord::stamped(point), &error).has_value());
    QCOMPARE(error, QStringLiteral("attribute '_P' has a type a record cannot hold (QPointF)"));

    // Not portable: `long` is 32 bits on Windows, 64 on Linux / macOS
    QVERIFY(!encodeCalculationRecord(CalculationRecord::stamped(attributeSnapshot(QVariant::fromValue(long(5)))),
                                     &error).has_value());
    QCOMPARE(error, QStringLiteral("attribute '_A' has a type a record cannot hold (long)"));
    QVERIFY(!encodeCalculationRecord(
                 CalculationRecord::stamped(attributeSnapshot(QVariant::fromValue(static_cast<unsigned long>(5)))),
                 &error).has_value());
    QCOMPARE(error, QStringLiteral("attribute '_A' has a type a record cannot hold (ulong)"));

    // Exactly the accepted types encode: every core type Qt can default-construct
    // (the GUI types need a QGuiApplication, and none of them is accepted)
    int accepted = 0;
    for (int typeId = QMetaType::FirstCoreType; typeId <= QMetaType::LastCoreType; ++typeId) {
        const QMetaType type(typeId);
        if (!type.isValid())
            continue;
        const QVariant value(type, nullptr);
        if (!value.isValid())
            continue;
        const bool encodes = encodeCalculationRecord(CalculationRecord::stamped(attributeSnapshot(value))).has_value();
        QVERIFY2(encodes == kRecordableTypes.contains(typeId), type.name());
        if (encodes)
            ++accepted;
    }
    QCOMPARE(accepted, int(kRecordableTypes.size()));

    // The error pointer is optional, and a success clears it
    QVERIFY(!encodeCalculationRecord(CalculationRecord::stamped(point)).has_value());
    QVERIFY(encodeCalculationRecord(CalculationRecord::stamped(sampleSnapshot()), &error).has_value());
    QVERIFY(error.isEmpty());
}

void ResultRecordsTest::sizeIsOrderOfSamples()
{
    StoredCalculationResult r;
    r.calculationId = QString::fromLatin1(kFitId);
    r.resultVersion = QStringLiteral("batch-temperature-bias-v3");
    r.inputFingerprint = QByteArray(32, '\x03');
    QVector<double> samples(10000);
    for (int i = 0; i < samples.size(); ++i)
        samples[i] = i * 0.01;
    for (int c = 0; c < 17; ++c)
        r.bundle.setMeasurement(QStringLiteral("Fusion"), QStringLiteral("channel%1").arg(c), samples,
                                QStringLiteral("deg"));
    r.bundle.setAttribute(QStringLiteral("_FUSION_DIAGNOSTICS"), QString(20000, QLatin1Char('x')));

    const QByteArray bytes = encoded(CalculationRecord::stamped(r));
    QVERIFY(!bytes.isEmpty());
    QVERIFY2(bytes.size() <= 17 * 8 * 10000 + 20000 * 2 + 4096, qPrintable(QString::number(bytes.size())));
}

// ============================================================================
// Logbook storage
// ============================================================================

void ResultRecordsTest::writeReadReplace()
{
    LogbookManager &logbook = LogbookManager::instance();
    const QString stem = saveIndexed(QStringLiteral("s1"));
    QVERIFY(!stem.isEmpty());

    const CalculationRecord first = recordFor(QString::fromLatin1(kFitId), 1.0);
    QString error = QStringLiteral("stale");
    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), first, &error));
    QVERIFY(error.isEmpty());

    // Exactly the session file and the record; no temporary file
    const QString dir = TestEnvironment::instance().sessionsDir();
    QCOMPARE(QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name),
             sorted({stem + QStringLiteral(".csv"), stem + QString::fromLatin1(kFitSuffix)}));
    QCOMPARE(calculationRecordFiles(), QStringList({stem + QString::fromLatin1(kFitSuffix)}));

    CalculationRecordRead read = logbook.readCalculationRecord(QStringLiteral("s1"), QString::fromLatin1(kFitId));
    QCOMPARE(read.status, CalculationRecordStatus::Ok);
    QVERIFY(read.error.isEmpty());
    QVERIFY(read.record.has_value());
    QString difference = recordDifference(*read.record, first);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    // A second write replaces it
    const CalculationRecord second = recordFor(QString::fromLatin1(kFitId), 3.0);
    QVERIFY(recordDifference(second, first).contains(QStringLiteral("samples differ")));
    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), second));
    read = logbook.readCalculationRecord(QStringLiteral("s1"), QString::fromLatin1(kFitId));
    QCOMPARE(read.status, CalculationRecordStatus::Ok);
    difference = recordDifference(*read.record, second);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));

    QCOMPARE(logbook.calculationRecordIds(QStringLiteral("s1")), QStringList({QString::fromLatin1(kFitId)}));
    QCOMPARE(QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).size(), 2);
}

void ResultRecordsTest::readStatuses()
{
    LogbookManager &logbook = LogbookManager::instance();
    const QString stem = saveIndexed(QStringLiteral("s1"));
    QVERIFY(!stem.isEmpty());
    const QString dir = TestEnvironment::instance().sessionsDir();

    CalculationRecordRead read = logbook.readCalculationRecord(QStringLiteral("nobody"), QStringLiteral("x"));
    QCOMPARE(read.status, CalculationRecordStatus::Missing);
    QCOMPARE(read.error, QStringLiteral("not in the logbook index"));
    QVERIFY(!read.record.has_value());

    read = logbook.readCalculationRecord(QStringLiteral("s1"), QStringLiteral("x"));
    QCOMPARE(read.status, CalculationRecordStatus::Missing);
    QVERIFY(read.error.isEmpty());
    QVERIFY(!read.record.has_value());

    QVERIFY(writeFile(dir + QLatin1Char('/') + stem + QStringLiteral(".garbage.fvresult"), "garbage"));
    read = logbook.readCalculationRecord(QStringLiteral("s1"), QStringLiteral("garbage"));
    QCOMPARE(read.status, CalculationRecordStatus::NotARecord);
    QCOMPARE(read.error, QStringLiteral("not a calculation record"));
    QVERIFY(!read.record.has_value());

    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), recordFor(QStringLiteral("x"))));
    const QByteArray xBytes = readFileBytes(dir + QLatin1Char('/') + stem + QStringLiteral(".x.fvresult"));
    QVERIFY(!xBytes.isEmpty());

    QVERIFY(writeFile(dir + QLatin1Char('/') + stem + QStringLiteral(".z.fvresult"), patchVersion(xBytes, 2)));
    read = logbook.readCalculationRecord(QStringLiteral("s1"), QStringLiteral("z"));
    QCOMPARE(read.status, CalculationRecordStatus::UnsupportedVersion);
    QCOMPARE(read.error, QStringLiteral("format version 2 is not supported"));

    // A valid record of id "x" under the name of id "y"
    QVERIFY(QFile::copy(dir + QLatin1Char('/') + stem + QStringLiteral(".x.fvresult"),
                        dir + QLatin1Char('/') + stem + QStringLiteral(".y.fvresult")));
    read = logbook.readCalculationRecord(QStringLiteral("s1"), QStringLiteral("y"));
    QCOMPARE(read.status, CalculationRecordStatus::Corrupt);
    QCOMPARE(read.error, QStringLiteral("the record belongs to calculation 'x'"));
    QVERIFY(!read.record.has_value());

    // Something that cannot be opened as a file
    QVERIFY(QDir().mkdir(dir + QLatin1Char('/') + stem + QStringLiteral(".d.fvresult")));
    read = logbook.readCalculationRecord(QStringLiteral("s1"), QStringLiteral("d"));
    QCOMPARE(read.status, CalculationRecordStatus::Unreadable);
    QVERIFY(!read.error.isEmpty());

    // A read never deletes anything
    QCOMPARE(calculationRecordFiles(),
             sorted({stem + QStringLiteral(".garbage.fvresult"), stem + QStringLiteral(".x.fvresult"),
                     stem + QStringLiteral(".y.fvresult"), stem + QStringLiteral(".z.fvresult")}));
    QCOMPARE(logbook.calculationRecordIds(QStringLiteral("s1")),
             QStringList({"garbage", "x", "y", "z"}));
}

void ResultRecordsTest::writeFailureRefusedEncoding()
{
    LogbookManager &logbook = LogbookManager::instance();
    const QString stem = saveIndexed(QStringLiteral("s1"));
    QVERIFY(!stem.isEmpty());
    const QString path = TestEnvironment::instance().sessionsDir() + QLatin1Char('/') + stem
        + QString::fromLatin1(kFitSuffix);

    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), recordFor(QString::fromLatin1(kFitId))));
    const QByteArray before = readFileBytes(path);
    QVERIFY(!before.isEmpty());

    StoredCalculationResult refused = sampleSnapshot(QString::fromLatin1(kFitId), 5.0);
    refused.bundle.setAttribute(QStringLiteral("_BAD"), QVariantList{1, 2, 3});
    QString error;
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression(QStringLiteral("calculation record builtin\\.fusion\\.fit of s1 not written: "
                                                           "attribute '_BAD'")));
    QVERIFY(!logbook.writeCalculationRecord(QStringLiteral("s1"), CalculationRecord::stamped(refused), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(error.contains(QStringLiteral("'_BAD'")));

    QCOMPARE(readFileBytes(path), before);
    QCOMPARE(calculationRecordFiles(), QStringList({stem + QString::fromLatin1(kFitSuffix)}));
}

void ResultRecordsTest::writeFailureDirectoryAtPath()
{
    LogbookManager &logbook = LogbookManager::instance();
    const QString stem = saveIndexed(QStringLiteral("s1"));
    QVERIFY(!stem.isEmpty());
    const QString dir = TestEnvironment::instance().sessionsDir();
    const QString path = dir + QLatin1Char('/') + stem + QString::fromLatin1(kFitSuffix);

    QVERIFY(QDir().mkdir(path));
    const auto listing = [&dir]() {
        return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    };
    const QStringList before = listing();
    QCOMPARE(before.size(), 2);

    QString error;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("not written: Couldn't write file")));
    QVERIFY(!logbook.writeCalculationRecord(QStringLiteral("s1"), recordFor(QString::fromLatin1(kFitId)), &error));
    QVERIFY2(error.contains(path), qPrintable(error));
    QVERIFY(error.startsWith(QStringLiteral("Couldn't write file '")));

    QCOMPARE(listing(), before);
    QVERIFY(QFileInfo(path).isDir());
    QVERIFY(calculationRecordFiles().isEmpty());

    QVERIFY(QDir().rmdir(path));
    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), recordFor(QString::fromLatin1(kFitId)), &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(calculationRecordFiles(), QStringList({stem + QString::fromLatin1(kFitSuffix)}));
}

void ResultRecordsTest::writeForUnknownSession()
{
    LogbookManager &logbook = LogbookManager::instance();
    QVERIFY(!saveIndexed(QStringLiteral("s1")).isEmpty());

    QString error;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("of nobody not written: not in the logbook index")));
    QVERIFY(!logbook.writeCalculationRecord(QStringLiteral("nobody"), recordFor(QString::fromLatin1(kFitId)), &error));
    QCOMPARE(error, QStringLiteral("not in the logbook index"));
    QVERIFY(calculationRecordFiles().isEmpty());
    QVERIFY(logbook.calculationRecordIds(QStringLiteral("nobody")).isEmpty());
}

void ResultRecordsTest::removeOneAndAll()
{
    LogbookManager &logbook = LogbookManager::instance();
    const QString stem = saveIndexed(QStringLiteral("s1"));
    QVERIFY(!stem.isEmpty());
    const QString csvPath = sessionFilePath(QStringLiteral("s1"));
    const QByteArray csvBytes = readFileBytes(csvPath);

    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), recordFor(QStringLiteral("a.one"))));
    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), recordFor(QStringLiteral("b.two"))));
    const QString twoPath = TestEnvironment::instance().sessionsDir() + QLatin1Char('/') + stem
        + QStringLiteral(".b%2Etwo.fvresult");
    const QByteArray twoBytes = readFileBytes(twoPath);
    QCOMPARE(logbook.calculationRecordIds(QStringLiteral("s1")), QStringList({"a.one", "b.two"}));

    QVERIFY(logbook.removeCalculationRecord(QStringLiteral("s1"), QStringLiteral("a.one")));
    QCOMPARE(calculationRecordFiles(), QStringList({stem + QStringLiteral(".b%2Etwo.fvresult")}));
    QCOMPARE(readFileBytes(twoPath), twoBytes);
    QCOMPARE(readFileBytes(csvPath), csvBytes);

    // Absent counts as removed
    QVERIFY(logbook.removeCalculationRecord(QStringLiteral("s1"), QStringLiteral("a.one")));

    QVERIFY(logbook.removeCalculationRecords(QStringLiteral("s1")));
    QVERIFY(calculationRecordFiles().isEmpty());
    QVERIFY(logbook.calculationRecordIds(QStringLiteral("s1")).isEmpty());
    QCOMPARE(readFileBytes(csvPath), csvBytes);
    QVERIFY(logbook.removeCalculationRecords(QStringLiteral("s1")));

    QVERIFY(!logbook.removeCalculationRecord(QStringLiteral("nobody"), QStringLiteral("a.one")));
    QVERIFY(!logbook.removeCalculationRecords(QStringLiteral("nobody")));
}

void ResultRecordsTest::removeSessionDeletesRecords()
{
    LogbookManager &logbook = LogbookManager::instance();
    const QString stem1 = saveIndexed(QStringLiteral("s1"));
    const QString stem2 = saveIndexed(QStringLiteral("s2"));
    QVERIFY(!stem1.isEmpty() && !stem2.isEmpty());
    const QString dir = TestEnvironment::instance().sessionsDir();

    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), recordFor(QString::fromLatin1(kFitId))));
    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), recordFor(QStringLiteral("x"))));
    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s2"), recordFor(QString::fromLatin1(kFitId))));
    QCOMPARE(calculationRecordFiles().size(), 3);

    const QString csv2 = dir + QLatin1Char('/') + stem2 + QStringLiteral(".csv");
    const QString record2 = dir + QLatin1Char('/') + stem2 + QString::fromLatin1(kFitSuffix);
    const QByteArray csv2Bytes = readFileBytes(csv2);
    const QByteArray record2Bytes = readFileBytes(record2);

    QVERIFY(logbook.removeSession(QStringLiteral("s1")));

    QCOMPARE(sessionCsvFiles(), QStringList({stem2 + QStringLiteral(".csv")}));
    QCOMPARE(calculationRecordFiles(), QStringList({stem2 + QString::fromLatin1(kFitSuffix)}));
    QCOMPARE(readFileBytes(csv2), csv2Bytes);
    QCOMPARE(readFileBytes(record2), record2Bytes);
    QVERIFY(logbook.calculationRecordIds(QStringLiteral("s1")).isEmpty());
}

void ResultRecordsTest::removeSessionDottedStems()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();

    // Two session files named by hand, a.csv and a.b.csv, and no index: the
    // fallback scan makes them the identity entries "a" and "a.b".
    QVERIFY(!saveIndexed(QStringLiteral("s1")).isEmpty());
    const QByteArray csv = readFileBytes(sessionFilePath(QStringLiteral("s1")));
    QVERIFY(QFile::remove(sessionFilePath(QStringLiteral("s1"))));
    QVERIFY(QFile::remove(env.indexPath()));
    const QString dir = env.sessionsDir();
    QVERIFY(writeFile(dir + QStringLiteral("/a.csv"), csv));
    QVERIFY(writeFile(dir + QStringLiteral("/a.b.csv"), csv));

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.hasDeferredScan());
    QCOMPARE(sorted(logbook.scannedUuids()), QStringList({"a", "a.b"}));

    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("a"), recordFor(QStringLiteral("x"))));
    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("a.b"), recordFor(QStringLiteral("x"))));
    QCOMPARE(calculationRecordFiles(), QStringList({"a.b.x.fvresult", "a.x.fvresult"}));
    QCOMPARE(logbook.calculationRecordIds(QStringLiteral("a")), QStringList({"x"}));
    QCOMPARE(logbook.calculationRecordIds(QStringLiteral("a.b")), QStringList({"x"}));

    QVERIFY(logbook.removeSession(QStringLiteral("a")));
    QCOMPARE(calculationRecordFiles(), QStringList({"a.b.x.fvresult"}));
    QCOMPARE(sessionCsvFiles(), QStringList({"a.b.csv"}));
    QCOMPARE(logbook.readCalculationRecord(QStringLiteral("a.b"), QStringLiteral("x")).status,
             CalculationRecordStatus::Ok);
}

void ResultRecordsTest::failedSessionRemovalKeepsRecords()
{
    LogbookManager &logbook = LogbookManager::instance();
    const QString stem = saveIndexed(QStringLiteral("s1"));
    QVERIFY(!stem.isEmpty());
    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), recordFor(QString::fromLatin1(kFitId))));
    const QString recordPath = TestEnvironment::instance().sessionsDir() + QLatin1Char('/') + stem
        + QString::fromLatin1(kFitSuffix);
    const QByteArray recordBytes = readFileBytes(recordPath);

    // The csv is already gone, so removing it fails
    QVERIFY(QFile::remove(sessionFilePath(QStringLiteral("s1"))));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("LogbookManager: failed to remove .*\\.csv")));
    QVERIFY(!logbook.removeSession(QStringLiteral("s1")));

    QCOMPARE(calculationRecordFiles(), QStringList({stem + QString::fromLatin1(kFitSuffix)}));
    QCOMPARE(readFileBytes(recordPath), recordBytes);
    QCOMPARE(logbook.calculationRecordIds(QStringLiteral("s1")), QStringList({QString::fromLatin1(kFitId)}));
}

void ResultRecordsTest::strayRecordsRemovedAtScan_data()
{
    QTest::addColumn<QString>("branch");

    QTest::newRow("extended index") << QStringLiteral("extended");
    QTest::newRow("legacy flat index") << QStringLiteral("legacy");
    QTest::newRow("no index") << QStringLiteral("none");
}

void ResultRecordsTest::strayRecordsRemovedAtScan()
{
    QFETCH(QString, branch);

    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();
    const QString stem1 = saveIndexed(QStringLiteral("s1"));
    const QString stem2 = saveIndexed(QStringLiteral("s2"));
    QVERIFY(!stem1.isEmpty() && !stem2.isEmpty());
    const QString dir = env.sessionsDir();

    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), recordFor(QString::fromLatin1(kFitId))));
    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s2"), recordFor(QStringLiteral("exp.a"))));
    const QByteArray recordBytes = readFileBytes(dir + QLatin1Char('/') + stem1 + QString::fromLatin1(kFitSuffix));

    const QString stray = QStringLiteral("0badc0de-0000-4000-8000-000000000000") + QString::fromLatin1(kFitSuffix);
    const QString junk = QStringLiteral("junk.fvresult");
    QVERIFY(writeFile(dir + QLatin1Char('/') + stray, recordBytes));
    QVERIFY(writeFile(dir + QLatin1Char('/') + junk, "junk"));
    QVERIFY(writeFile(dir + QStringLiteral("/notes.txt"), "notes"));
    // Hand-renamed: another spelling of the extension is not a record, of a
    // session or of none, so it is neither listed nor removed as a stray
    QVERIFY(writeFile(dir + QLatin1Char('/') + stem1 + QStringLiteral(".x.FVRESULT"), recordBytes));
    QVERIFY(writeFile(dir + QStringLiteral("/0badc0de-0000-4000-8000-000000000001.x.FVRESULT"), recordBytes));
    QVERIFY(writeFile(dir + QStringLiteral("/x.csv.AbC123"), "a leftover session-save temporary"));
    QVERIFY(writeFile(dir + QLatin1Char('/') + stem1 + QString::fromLatin1(kFitSuffix) + QStringLiteral(".Q1w2E3"),
                      "a leftover record temporary"));

    QMap<QString, QByteArray> expected = directoryContents(dir);
    QCOMPARE(expected.size(), 11);
    QVERIFY(expected.remove(stray) == 1);
    QVERIFY(expected.remove(junk) == 1);

    if (branch == QLatin1String("legacy")) {
        QJsonObject entry1;
        entry1[QStringLiteral("uuid")] = stem1;
        QJsonObject entry2;
        entry2[QStringLiteral("uuid")] = stem2;
        QJsonObject flat;
        flat[QStringLiteral("s1")] = entry1;
        flat[QStringLiteral("s2")] = entry2;
        QVERIFY(writeIndex(flat));
    } else if (branch == QLatin1String("none")) {
        QVERIFY(QFile::remove(env.indexPath()));
    }

    env.reopenLogbook();
    logbook.initialize();

    // Exactly the stray record and junk.fvresult are gone; every other file keeps its bytes
    const QMap<QString, QByteArray> after = directoryContents(dir);
    QCOMPARE(after.keys(), expected.keys());
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it)
        QVERIFY2(after.value(it.key()) == it.value(), qPrintable(it.key()));

    // No record ever shows up as a session
    QCOMPARE(sessionCsvFiles(), sorted({stem1 + QStringLiteral(".csv"), stem2 + QStringLiteral(".csv")}));
    if (branch == QLatin1String("none")) {
        QVERIFY(logbook.hasDeferredScan());
        QCOMPARE(sorted(logbook.scannedUuids()), sorted({stem1, stem2}));
        QCOMPARE(logbook.cachedColumnValues({descriptionColumn()}).keys(), sorted({stem1, stem2}));
    } else {
        QCOMPARE(logbook.hasIndexData(), branch == QLatin1String("extended"));
        QCOMPARE(logbook.cachedColumnValues({descriptionColumn()}).keys(), QStringList({"s1", "s2"}));
        QCOMPARE(logbook.calculationRecordIds(QStringLiteral("s1")), QStringList({QString::fromLatin1(kFitId)}));
        QCOMPARE(logbook.calculationRecordIds(QStringLiteral("s2")), QStringList({"exp.a"}));
    }
}

void ResultRecordsTest::orphanAdoptionKeepsRecord()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();

    QVERIFY(!saveIndexed(QStringLiteral("a")).isEmpty());
    const QStringList indexedFiles = sessionCsvFiles();

    // "c": the csv is committed, the index never flushed; it has one record
    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("c"))));
    QStringList orphanFiles = sessionCsvFiles();
    for (const QString &file : indexedFiles)
        orphanFiles.removeOne(file);
    QCOMPARE(orphanFiles.size(), 1);
    const QString stem = QFileInfo(orphanFiles.first()).completeBaseName();
    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("c"), recordFor(QStringLiteral("x"))));
    const QString recordName = stem + QStringLiteral(".x.fvresult");
    const QByteArray recordBytes = readFileBytes(env.sessionsDir() + QLatin1Char('/') + recordName);

    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.hasIndexData());

    // Adopted, record kept, and no identity entry for the record file
    QCOMPARE(logbook.cachedColumnValues({descriptionColumn()}).keys(), sorted({QStringLiteral("a"), stem}));
    QVERIFY(logbook.isIdentityEntry(stem));
    QCOMPARE(calculationRecordFiles(), QStringList({recordName}));
    QCOMPARE(readFileBytes(env.sessionsDir() + QLatin1Char('/') + recordName), recordBytes);
    QCOMPARE(logbook.calculationRecordIds(stem), QStringList({"x"}));
    QCOMPARE(logbook.readCalculationRecord(stem, QStringLiteral("x")).status, CalculationRecordStatus::Ok);
}

void ResultRecordsTest::recordsFollowRemap()
{
    TestEnvironment &env = TestEnvironment::instance();
    LogbookManager &logbook = LogbookManager::instance();

    // An identity entry: the session file without an index
    const QString stem = saveIndexed(QStringLiteral("s1"));
    QVERIFY(!stem.isEmpty());
    QVERIFY(QFile::remove(env.indexPath()));
    env.reopenLogbook();
    logbook.initialize();
    QVERIFY(logbook.isIdentityEntry(stem));

    const CalculationRecord record = recordFor(QString::fromLatin1(kFitId));
    QVERIFY(logbook.writeCalculationRecord(stem, record));
    QCOMPARE(calculationRecordFiles(), QStringList({stem + QString::fromLatin1(kFitSuffix)}));

    // What the model does when it first loads the stub
    QVERIFY(logbook.remapSessionId(stem, QStringLiteral("s1")));
    QCOMPARE(logbook.calculationRecordIds(QStringLiteral("s1")), QStringList({QString::fromLatin1(kFitId)}));
    const CalculationRecordRead read = logbook.readCalculationRecord(QStringLiteral("s1"), QString::fromLatin1(kFitId));
    QCOMPARE(read.status, CalculationRecordStatus::Ok);
    const QString difference = recordDifference(*read.record, record);
    QVERIFY2(difference.isEmpty(), qPrintable(difference));
    QCOMPARE(logbook.readCalculationRecord(stem, QString::fromLatin1(kFitId)).status, CalculationRecordStatus::Missing);
    QVERIFY(logbook.calculationRecordIds(stem).isEmpty());

    // Not a stray at the next start
    QVERIFY(logbook.flushIndex());
    env.reopenLogbook();
    logbook.initialize();
    QCOMPARE(calculationRecordFiles(), QStringList({stem + QString::fromLatin1(kFitSuffix)}));
    QCOMPARE(logbook.calculationRecordIds(QStringLiteral("s1")), QStringList({QString::fromLatin1(kFitId)}));
}

void ResultRecordsTest::saveSessionIgnoresRecords()
{
    LogbookManager &logbook = LogbookManager::instance();
    const QString stem = saveIndexed(QStringLiteral("s1"));
    QVERIFY(!stem.isEmpty());
    const QString csvPath = sessionFilePath(QStringLiteral("s1"));
    const QByteArray csvBefore = readFileBytes(csvPath);
    QVERIFY(!csvBefore.isEmpty());

    QVERIFY(logbook.writeCalculationRecord(QStringLiteral("s1"), recordFor(QString::fromLatin1(kFitId))));
    const QString recordPath = TestEnvironment::instance().sessionsDir() + QLatin1Char('/') + stem
        + QString::fromLatin1(kFitSuffix);
    const QByteArray recordBytes = readFileBytes(recordPath);

    QVERIFY(logbook.saveSession(makeSession(QStringLiteral("s1"))));
    QCOMPARE(sessionFilePath(QStringLiteral("s1")), csvPath);
    QCOMPARE(readFileBytes(csvPath), csvBefore);
    QCOMPARE(readFileBytes(recordPath), recordBytes);
    QCOMPARE(sessionCsvFiles(), QStringList({stem + QStringLiteral(".csv")}));
}

FLYSIGHT_TEST_MAIN(ResultRecordsTest)
#include "tst_result_records.moc"
