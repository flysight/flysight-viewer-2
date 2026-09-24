// Session-level idempotency oracle: after any sequence of reads,
// edits, merges, preference changes, and registry changes on REAL sessions,
// the value returned for every name equals the value obtained by clearing all
// caches and evaluating from scratch.
//
//   acceptance 10 - randomized reads, edits and merges equal a fresh evaluation;
//   acceptance 5  - effective values are identical before and after a save /
//                   reload, under random histories (part B);
//   acceptance 11 - user overrides of one output of a multi-output calculation
//                   never create a cycle (cycleCount() stays 0);
//   acceptance 13 - registry changes reach every session (two sessions run
//                   interleaved on the global registry);
//   acceptance 15 - declared preferences invalidate, snapshotted ones do not;
//   acceptance 16 - derived wTotal and interpolated gyro values follow source
//                   and schema changes (they are catalogue names).
//
// Part A (sessionSequences) works on SessionData alone; part B
// (modelSequences) on the real SessionModel / LogbookManager through the
// application's import path, with restarts and a persisted-state oracle.
//
// Randomized but reproducible: std::mt19937 with a literal seed, reduced with
// `rng() % n` only, every choice an index into a fixed list. No clock and no
// hash-container iteration takes part in a decision. The only computed
// expectations are oracle comparisons and live-versus-reloaded equality; both
// parts also contain literal checkpoints.
//
// Reproduce a failure with FLYSIGHT_ORACLE_SEEDS=<seed> (see tests/README.md).

#include <cstdio>
#include <memory>
#include <random>

#include <QtTest>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include "altitudemarkerfeature.h"
#include "builtinfixture.h"
#include "calculations/builtincalculations.h"
#include "dataimporter.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "fixturebuilder.h"
#include "logbookcolumn.h"
#include "logbookmanager.h"
#include "logbookprobe.h"
#include "oraclecatalogue.h"
#include "parsedfile.h"
#include "preferences/preferencekeys.h"
#include "preferences/preferencesmanager.h"
#include "sessiondata.h"
#include "sessionimport.h"
#include "sessionmerge.h"
#include "sessionmodel.h"
#include "testenvironment.h"
#include "testmain.h"
#include "testutil.h"

using namespace FlySight;
using namespace FlySightTest;

namespace {

constexpr double T0 = DescentFixture::T0;
constexpr int kSampleNames = 8;         // names verified after every mutation
constexpr int kCheckpointEvery = 25;    // full-catalogue verification

using Outcome = MergeResult::Outcome;

// An in-memory SCHEMA_VER of "3" is part of the operation mix; the conversion
// layer warns about it on every read.
void quietHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg && message.contains(QLatin1String("is not supported")))
        return;
    fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, message)));
}

struct AltitudeRegistration {
    const char *key;
    double thresholdMetres;
};
const AltitudeRegistration kAltitudes[] = {
    {"_ALTITUDE_1000_M", 1000.0},
    {"_ALTITUDE_2000_M", 2000.0},
    {"_ALTITUDE_300_FT", 91.44},
};

struct ColumnRef {
    const char *sensor;
    const char *name;
};
const ColumnRef kReplaceColumns[] = {
    {"IMU", "wx"}, {"IMU", "az"}, {"TIME", "tow"}, {"GNSS", "hMSL"}, {"GNSS", "velD"}, {"BARO", "pressure"}};

struct UnitEdit {
    const char *sensor;
    const char *name;
    QStringList units;
};
const QList<UnitEdit> kUnitEdits = {
    {"IMU", "ax", {"g", "m/s^2", "furlongs"}},
    {"IMU", "az", {"g", "m/s^2", "furlongs"}},
    {"IMU", "temperature", {"deg C", "degC"}},
    {"MAG", "x", {"gauss", "T"}},
};

struct PreferenceEdit {
    QString key;
    QList<double> values;
    bool declared;      // a declared input of a calculation (else: snapshotted at import)
};
QList<PreferenceEdit> preferenceEdits()
{
    return {
        {PreferenceKeys::ImportDescentPauseSeconds, {30.0, 5.0, 1.0}, true},
        {PreferenceKeys::AeroMass, {1.0, 90.0}, false},
    };
}

QString valueText(const QVariant &value)
{
    if (!value.isValid())
        return QStringLiteral("remove");
    return value.userType() == QMetaType::Double ? QString::number(value.toDouble(), 'g', 17)
                                                 : value.toString();
}

// The attribute each logbook column of part B shows, in column order (D, X, G, A).
QStringList columnAttributeKeys()
{
    return {QStringLiteral("_DESCRIPTION"), QStringLiteral("_EXIT_TIME"),
            SessionData::interpolationKey(QStringLiteral("_M"), QStringLiteral("IMU"),
                                          QString::fromLatin1(SessionKeys::Time), QStringLiteral("wx")),
            QStringLiteral("_ALTITUDE_1000_M")};
}

// The value index.json (as read from disk) holds for (session, column);
// Undefined when it holds none.
QJsonValue indexValue(const QJsonObject &root, const QString &sessionId, int column)
{
    const QJsonObject columns = root[QStringLiteral("columns")].toObject();
    for (auto it = columns.constBegin(); it != columns.constEnd(); ++it) {
        const QJsonObject def = it.value().toObject();
        const QString type = def[QStringLiteral("type")].toString();
        const QString attributeKey = def[QStringLiteral("attributeKey")].toString();
        bool match = false;
        switch (column) {
        case 0: match = type == QLatin1String("SessionAttribute") && attributeKey == QLatin1String("_DESCRIPTION"); break;
        case 1: match = type == QLatin1String("SessionAttribute") && attributeKey == QLatin1String("_EXIT_TIME"); break;
        case 2: match = type == QLatin1String("MeasurementAtMarker"); break;
        case 3: match = type == QLatin1String("SessionAttribute") && attributeKey == QLatin1String("_ALTITUDE_1000_M"); break;
        }
        if (match) {
            return root[QStringLiteral("sessions")].toObject()[sessionId].toObject()
                       [QStringLiteral("values")].toObject().value(it.key());
        }
    }
    return QJsonValue(QJsonValue::Undefined);
}

// A cached column value against the value the saved file gives. Text and
// number forms of the same number are the same value (a reloaded attribute is
// text, a live one may be a double).
bool sameColumnValue(const QVariant &cached, const QVariant &expected)
{
    if (!cached.isValid() || !expected.isValid())
        return cached.isValid() == expected.isValid();
    bool okCached = false, okExpected = false;
    const double c = cached.toDouble(&okCached);
    const double e = expected.toDouble(&okExpected);
    return okCached && okExpected ? c == e : cached.toString() == expected.toString();
}

// What a plot and the analysis docks keep on screen: dependents of the gyro,
// the accelerometer, the ground elevation, the exit and the manoeuvre marker.
// Part B's subscriber holds these besides the randomly read name.
QList<DependencyKey> subscriberNames()
{
    return {DependencyKey::measurement(QStringLiteral("IMU"), QStringLiteral("wTotal")),
            DependencyKey::measurement(QStringLiteral("IMU"), QStringLiteral("aTotal")),
            DependencyKey::measurement(QStringLiteral("MAG"), QStringLiteral("total")),
            DependencyKey::measurement(QStringLiteral("GNSS"), QStringLiteral("z")),
            DependencyKey::measurement(QStringLiteral("BARO"), QStringLiteral("_time")),
            DependencyKey::attribute(QStringLiteral("_EXIT_TIME")),
            DependencyKey::attribute(QStringLiteral("_DURATION")),
            DependencyKey::attribute(QStringLiteral("_WSP_TIME_RESULT")),
            DependencyKey::attribute(QStringLiteral("_M:IMU/_time/wx")),
            DependencyKey::attribute(QStringLiteral("_ALTITUDE_1000_M"))};
}

// Shared by both parts: the seeded generator, the operation log, and the one
// place a check fails.
class RunBase {
public:
    RunBase(unsigned seed, int operations)
        : m_seed(seed), m_operations(operations), m_rng(seed), m_catalogue(oracleCatalogue())
    {
    }

    const QString &failure() const { return m_failure; }
    const OracleOpLog &log() const { return m_log; }

protected:
    int pick(int n) { return int(m_rng() % unsigned(n)); }

    bool fail(const QString &what)
    {
        if (m_failure.isEmpty())
            m_failure = QStringLiteral("seed=%1 step=%2 %3").arg(m_seed).arg(m_step).arg(what);
        return false;
    }

    // Reads each name the ordinary way and compares it with a fresh evaluation.
    bool verify(const SessionData &session, const QString &label, const QList<DependencyKey> &names)
    {
        CalculationEngine &engine = session.calculationEngine();
        for (const DependencyKey &name : names) {
            const CalculationEngine::Value cached = oracleRead(session, name);
            const CalculationEngine::Value fresh = engine.evaluateFresh(name);
            if (!CalculationEngine::sameValue(cached, fresh)) {
                return fail(QStringLiteral("session=%1 name=%2 cached=%3 fresh=%4")
                                .arg(label, describe(name), oracleDescribe(cached), oracleDescribe(fresh)));
            }
        }
        if (engine.scopeDepth() != 0)
            return fail(QStringLiteral("session=%1 scopeDepth=%2").arg(label).arg(engine.scopeDepth()));
        if (engine.undeclaredReadCount() != 0)
            return fail(QStringLiteral("session=%1 undeclared read of %2 by %3")
                            .arg(label, describe(engine.lastUndeclaredRead().second),
                                 engine.lastUndeclaredRead().first));
        // Acceptance 11: user overrides never create a cycle
        if (engine.cycleCount() != 0)
            return fail(QStringLiteral("session=%1 cycleCount=%2").arg(label).arg(engine.cycleCount()));
        return true;
    }

    QList<DependencyKey> randomNames(int count)
    {
        QList<DependencyKey> names;
        for (int i = 0; i < count; ++i)
            names.append(m_catalogue.at(pick(int(m_catalogue.size()))));
        return names;
    }

    unsigned m_seed;
    int m_operations;
    int m_step = 0;
    std::mt19937 m_rng;
    QList<DependencyKey> m_catalogue;
    OracleOpLog m_log;
    QString m_failure;
};

// ─────────────────────────────── part A: SessionData only

class SessionRun : public RunBase {
public:
    SessionRun(unsigned seed, int operations) : RunBase(seed, operations) {}
    ~SessionRun() { restoreGlobals(); }

    bool execute();

private:
    struct Subject {
        QString label;
        SessionData data;
        int deliveries = 0;                     // invalidation-listener calls
        QList<SessionData> fragments;           // parsed once, fragment order
        QStringList fragmentNames;
        QSet<QString> knownAttributes;          // keys that were ever stored
    };

    bool prefix(Subject &s, bool sensorFirst);
    bool operation();
    bool afterMutation(Subject &s, int runsBefore);
    bool enumerationIsStoredOnly(const Subject &s);
    bool endState(Subject &s);
    bool merge(Subject &s, int fragment, bool mustSucceed);
    int totalRuns() const
    {
        return m_subjects[0]->data.calculationEngine().totalRunCount()
             + m_subjects[1]->data.calculationEngine().totalRunCount();
    }
    void restoreGlobals();

    std::unique_ptr<Subject> m_subjects[2];
    bool m_altitudeRegistered[3] = {false, false, false};
    QSet<QPair<QString, QString>> m_fragmentColumns;    // every column any fragment records
};

void SessionRun::restoreGlobals()
{
    for (int i = 0; i < 3; ++i) {
        if (m_altitudeRegistered[i]) {
            CalculationRegistry::instance().unregister(
                AltitudeMarkerManager::calculationId(QString::fromLatin1(kAltitudes[i].key)),
                CalculationRegistry::Removal::Change);
            m_altitudeRegistered[i] = false;
        }
    }
    TestEnvironment::instance().resetPreferencesToDefaults();
}

bool SessionRun::merge(Subject &s, int fragment, bool mustSucceed)
{
    const SourceData sourceBefore = s.data.sourceData();
    const QStringList keysBefore = s.data.attributeKeys();

    const MergePlan plan = SessionMerge::plan(s.data, s.fragments.at(fragment));
    if (plan.error.contains(QLatin1String("rows but")))
        return fail(QStringLiteral("ragged merge of %1: %2").arg(s.fragmentNames.at(fragment), plan.error));
    if (!plan.ok()) {
        m_log.append(m_step, s.label, QStringLiteral("merge %1 rejected").arg(s.fragmentNames.at(fragment)));
        if (mustSucceed)
            return fail(QStringLiteral("merge of %1 rejected: %2").arg(s.fragmentNames.at(fragment), plan.error));
        // A rejected plan is never applied and nothing changed
        if (!(s.data.sourceData() == sourceBefore) || s.data.attributeKeys() != keysBefore)
            return fail(QStringLiteral("rejected merge of %1 changed the session").arg(s.fragmentNames.at(fragment)));
        return true;
    }

    m_log.append(m_step, s.label, QStringLiteral("merge %1 %2").arg(s.fragmentNames.at(fragment),
                                                                plan.isEmpty() ? "unchanged" : "applied"));
    for (const auto &entry : plan.attributesToSet)
        s.knownAttributes.insert(entry.first);
    SessionMerge::apply(s.data, plan);
    return true;
}

bool SessionRun::prefix(Subject &s, bool sensorFirst)
{
    const QByteArray id = s.label.toLatin1();
    const QList<OracleFragment> fragments = oracleFragments(id);
    const QStringList paths = writeOracleFragments(
        fragments, TestEnvironment::instance().newTempDir(QStringLiteral("oracle-") + s.label));

    for (int i = 0; i < fragments.size(); ++i) {
        DataImporter importer;
        ParsedFile parsed;
        if (!importer.parseFile(paths.at(i), parsed))
            return fail(QStringLiteral("fragment %1: %2").arg(fragments.at(i).name, importer.getLastError()));
        s.fragments.append(parsed.data);
        s.fragmentNames.append(fragments.at(i).name);

        const SourceData source = parsed.data.sourceData();
        for (auto sensorIt = source.cbegin(); sensorIt != source.cend(); ++sensorIt) {
            for (auto it = sensorIt->cbegin(); it != sensorIt->cend(); ++it)
                m_fragmentColumns.insert({sensorIt.key(), it.key()});
        }
    }

    // Fragment 0 is TRACK, fragment 1 is SENSOR
    DataImporter importer;
    if (!importer.importFile(paths.at(sensorFirst ? 1 : 0), s.data))
        return fail(QStringLiteral("prefix import: %1").arg(importer.getLastError()));
    m_log.append(0, s.label, sensorFirst ? QStringLiteral("import SENSOR") : QStringLiteral("import TRACK"));
    for (const QString &key : s.data.attributeKeys())
        s.knownAttributes.insert(key);

    int *deliveries = &s.deliveries;
    s.data.calculationEngine().setInvalidationListener(
        [deliveries](const QSet<DependencyKey> &) { ++*deliveries; });

    if (sensorFirst) {
        // Literal checkpoint: the sensor file alone
        if (s.data.getAttribute("_START_TIME").toDouble() != T0 + 10.0)
            return fail(QStringLiteral("checkpoint _START_TIME (sensor only)"));
        if (s.data.getAttribute("_DURATION").toDouble() != 20.0)
            return fail(QStringLiteral("checkpoint _DURATION (sensor only)"));
        if (!isNear(s.data.getMeasurement("IMU", "wTotal").value(0), 5.7344))
            return fail(QStringLiteral("checkpoint IMU/wTotal (sensor only)"));
        if (!s.data.getMeasurement("GNSS", "z").isEmpty())
            return fail(QStringLiteral("checkpoint GNSS/z (sensor only)"));
    }

    if (!merge(s, sensorFirst ? 0 : 1, true))
        return false;

    // Literal checkpoint: both files
    if (s.data.getAttribute("_START_TIME").toDouble() != T0)
        return fail(QStringLiteral("checkpoint _START_TIME"));
    if (s.data.getAttribute("_DURATION").toDouble() != 295.0)
        return fail(QStringLiteral("checkpoint _DURATION"));
    if (s.data.getAttribute("_EXIT_TIME").toDouble() != T0 + 9.0)
        return fail(QStringLiteral("checkpoint _EXIT_TIME"));
    if (s.data.getAttribute("_GROUND_ELEV").toDouble() != 100.0)
        return fail(QStringLiteral("checkpoint _GROUND_ELEV"));
    return true;
}

// Calculation outputs never appear in enumeration because they
// happened to be computed.
bool SessionRun::enumerationIsStoredOnly(const Subject &s)
{
    for (const QString &key : s.data.attributeKeys()) {
        if (!s.knownAttributes.contains(key))
            return fail(QStringLiteral("session=%1 attributeKeys() lists '%2', which was never stored").arg(s.label, key));
    }
    for (const QString &sensor : s.data.sensorKeys()) {
        for (const QString &name : s.data.measurementKeys(sensor)) {
            if (!m_fragmentColumns.contains({sensor, name}))
                return fail(QStringLiteral("session=%1 measurementKeys() lists %2/%3, which no file records")
                                .arg(s.label, sensor, name));
        }
    }
    return true;
}

bool SessionRun::afterMutation(Subject &s, int runsBefore)
{
    // Invalidation never computes
    if (totalRuns() != runsBefore)
        return fail(QStringLiteral("the mutation itself ran %1 calculation(s)").arg(totalRuns() - runsBefore));
    if (!enumerationIsStoredOnly(*m_subjects[0]) || !enumerationIsStoredOnly(*m_subjects[1]))
        return false;
    return verify(s.data, s.label, randomNames(kSampleNames));
}

bool SessionRun::operation()
{
    Subject &s = *m_subjects[pick(2)];
    const int kind = pick(100);
    const int runsBefore = totalRuns();

    if (kind < 40) {                                        // 40 %: read one name
        const DependencyKey name = m_catalogue.at(pick(int(m_catalogue.size())));
        m_log.append(m_step, s.label, QStringLiteral("read %1").arg(describe(name)));
        return verify(s.data, s.label, {name});
    }
    if (kind < 45) {                                        // 5 %: read burst
        m_log.append(m_step, s.label, QStringLiteral("readBurst"));
        return verify(s.data, s.label, randomNames(10));
    }
    if (kind < 60) {                                        // 15 %: attribute set / remove
        const QList<OracleAttributeEdit> edits = oracleAttributeEdits();
        const OracleAttributeEdit &edit = edits.at(pick(int(edits.size())));
        const QVariant value = edit.values.at(pick(int(edit.values.size())));
        m_log.append(m_step, s.label, QStringLiteral("setAttribute %1 %2").arg(edit.key, valueText(value)));
        if (value.isValid()) {
            s.data.setAttribute(edit.key, value);
            s.knownAttributes.insert(edit.key);
        } else {
            s.data.removeAttribute(edit.key);
        }
        return afterMutation(s, runsBefore);
    }
    if (kind < 68) {                                        // 8 %: merge a fragment
        if (!merge(s, pick(int(s.fragments.size())), false))
            return false;
        return afterMutation(s, runsBefore);
    }
    if (kind < 76) {                                        // 8 %: replace source samples
        const ColumnRef &column = kReplaceColumns[pick(int(std::size(kReplaceColumns)))];
        if (!s.data.hasSourceMeasurement(column.sensor, column.name)) {
            m_log.append(m_step, s.label, QStringLiteral("read %1/%2 (no source)").arg(column.sensor, column.name));
            return verify(s.data, s.label, {DependencyKey::measurement(column.sensor, column.name)});
        }
        QVector<double> samples = s.data.sourceMeasurement(column.sensor, column.name);
        QString deltas;
        for (double &sample : samples) {
            const int delta = pick(5) - 2;
            sample += delta;
            if (deltas.size() < 24)
                deltas += QString::number(delta) + QLatin1Char(' ');
        }
        m_log.append(m_step, s.label, QStringLiteral("setSourceMeasurement %1/%2 %3").arg(column.sensor, column.name, deltas));
        s.data.setSourceMeasurement(column.sensor, column.name, samples, s.data.sourceUnit(column.sensor, column.name));
        return afterMutation(s, runsBefore);
    }
    if (kind < 81) {                                        // 5 %: recorded unit text
        const UnitEdit &edit = kUnitEdits.at(pick(int(kUnitEdits.size())));
        const QString unit = edit.units.at(pick(int(edit.units.size())));
        if (!s.data.hasSourceMeasurement(edit.sensor, edit.name)) {
            m_log.append(m_step, s.label, QStringLiteral("read %1/%2 (no source)").arg(edit.sensor, edit.name));
            return verify(s.data, s.label, {DependencyKey::measurement(edit.sensor, edit.name)});
        }
        m_log.append(m_step, s.label, QStringLiteral("setUnit %1/%2 %3").arg(edit.sensor, edit.name, unit));
        s.data.setUnit(edit.sensor, edit.name, unit);
        return afterMutation(s, runsBefore);
    }
    if (kind < 86) {                                        // 5 %: SCHEMA_VER ("3": in-memory unsupported)
        const char *values[] = {"2", "1", nullptr, "3"};
        const char *value = values[pick(4)];
        m_log.append(m_step, s.label, QStringLiteral("SCHEMA_VER %1").arg(value ? value : "remove"));
        if (value) {
            s.data.setAttribute("SCHEMA_VER", QString::fromLatin1(value));
            s.knownAttributes.insert(QStringLiteral("SCHEMA_VER"));
        } else {
            s.data.removeAttribute("SCHEMA_VER");
        }
        return afterMutation(s, runsBefore);
    }
    if (kind < 91) {                                        // 5 %: preference (acceptance 15)
        const QList<PreferenceEdit> edits = preferenceEdits();
        const PreferenceEdit &edit = edits.at(pick(int(edits.size())));
        const double value = edit.values.at(pick(int(edit.values.size())));
        m_log.append(m_step, s.label, QStringLiteral("preference %1 %2").arg(edit.key).arg(value));
        const int deliveriesBefore = m_subjects[0]->deliveries + m_subjects[1]->deliveries;
        PreferencesManager::instance().setValue(edit.key, value);
        if (!edit.declared && m_subjects[0]->deliveries + m_subjects[1]->deliveries != deliveriesBefore)
            return fail(QStringLiteral("snapshotted preference %1 invalidated a session").arg(edit.key));
        return afterMutation(s, runsBefore);
    }
    if (kind < 96) {                                        // 5 %: registry change (acceptance 13)
        const int index = pick(3);
        const QString key = QString::fromLatin1(kAltitudes[index].key);
        CalculationRegistry &registry = CalculationRegistry::instance();
        if (m_altitudeRegistered[index]) {
            m_log.append(m_step, s.label, QStringLiteral("unregister %1").arg(key));
            if (!registry.unregister(AltitudeMarkerManager::calculationId(key), CalculationRegistry::Removal::Change))
                return fail(QStringLiteral("unregister %1 failed").arg(key));
        } else {
            m_log.append(m_step, s.label, QStringLiteral("register %1").arg(key));
            if (!registry.registerCalculation(AltitudeMarkerManager::makeDescriptor(key, kAltitudes[index].thresholdMetres)))
                return fail(QStringLiteral("register %1 failed").arg(key));
        }
        m_altitudeRegistered[index] = !m_altitudeRegistered[index];
        if (totalRuns() != runsBefore)
            return fail(QStringLiteral("the registry change itself ran a calculation"));
        // The other session is affected just the same
        Subject &other = *m_subjects[&s == m_subjects[0].get() ? 1 : 0];
        if (!verify(other.data, other.label, {DependencyKey::attribute(key)}))
            return false;
        return afterMutation(s, totalRuns());
    }

    // 4 %: object motion - rebind and cold copies
    if (pick(2) == 0) {
        m_log.append(m_step, s.label, QStringLiteral("move"));
        SessionData tmp = std::move(s.data);
        s.data = std::move(tmp);
    } else {
        m_log.append(m_step, s.label, QStringLiteral("copyAssign"));
        const SessionData copy(s.data);
        s.data = copy;
    }
    return afterMutation(s, runsBefore);
}

// Independent of the oracle: with the persistent state restored, every value
// comes back to its literal.
bool SessionRun::endState(Subject &s)
{
    for (const OracleAttributeEdit &edit : oracleAttributeEdits()) {
        if (edit.key == QLatin1String("_WIND_N"))
            s.data.setAttribute(edit.key, 0.0);
        else if (edit.key == QLatin1String("_JUMPER_MASS"))
            s.data.setAttribute(edit.key, 1.0);
        else
            s.data.removeAttribute(edit.key);
    }
    s.data.removeAttribute("SCHEMA_VER");
    m_log.append(m_step, s.label, QStringLiteral("restore"));
    if (!merge(s, 0, true) || !merge(s, 1, true))
        return false;

    for (const GoldenValue &golden : goldenValues()) {
        const bool measurement = golden.name.type == DependencyKey::Type::Measurement;
        // Recorded data the original fixture lacks cannot be removed again
        // (IMU/wTotal after IMU_WTOTAL): stored beats derived, so skip it.
        if (measurement && s.data.hasSourceMeasurement(golden.name.measurementKey.first, golden.name.measurementKey.second))
            continue;
        const QString difference = compareToGolden(
            golden,
            measurement ? QVariant() : s.data.getAttribute(golden.name.attributeKey),
            measurement ? s.data.getMeasurement(golden.name.measurementKey.first, golden.name.measurementKey.second)
                        : QVector<double>());
        if (!difference.isEmpty())
            return fail(QStringLiteral("session=%1 end state: %2").arg(s.label, difference));
    }
    return true;
}

bool SessionRun::execute()
{
    for (int i = 0; i < 2; ++i) {
        m_subjects[i] = std::make_unique<Subject>();
        m_subjects[i]->label = i == 0 ? QStringLiteral("o1") : QStringLiteral("o2");
        if (!prefix(*m_subjects[i], m_seed % 2 == 1))
            return false;
    }

    for (m_step = 1; m_step <= m_operations; ++m_step) {
        if (!operation())
            return false;
        if (m_step % kCheckpointEvery == 0 || m_step == m_operations) {
            for (const auto &subject : m_subjects) {
                if (!verify(subject->data, subject->label, m_catalogue))
                    return false;
            }
        }
    }

    restoreGlobals();
    return endState(*m_subjects[0]) && endState(*m_subjects[1]);
}

// ─────────────────────────────── part B: SessionModel, LogbookManager, import path

class ModelRun : public RunBase {
public:
    ModelRun(unsigned seed, int operations) : RunBase(seed, operations) {}
    ~ModelRun() { tearDown(); }

    bool execute();

private:
    bool start();
    bool restart(bool crash);
    void connectMirror();
    bool verifyMirror(const SessionData &session, const QString &id);
    bool operation();
    bool verifyLoadedRows();
    bool importFragment(const QString &id, int fragment);
    bool persistedState();
    void tearDown();

    const QStringList m_ids = {QStringLiteral("o1"), QStringLiteral("o2")};
    QMap<QString, QStringList> m_paths;         // session id -> fragment paths
    QStringList m_fragmentNames;
    std::unique_ptr<SessionModel> m_model;
    std::unique_ptr<AltitudeMarkerManager> m_altitudes;

    // A subscriber, as the plot and the docks are: it keeps what it read until
    // SessionModel::dependencyChanged names it (every change of a merge reaches
    // consumers through the model's one notification path). Invalidations that
    // originate in the registry or a preference are delivered on the next
    // event-loop pass, so the mirror is only checked when none is outstanding.
    QHash<QString, QHash<DependencyKey, CalculationEngine::Value>> m_mirror;
    bool m_broadcastOutstanding = false;
};

void ModelRun::connectMirror()
{
    m_mirror.clear();
    m_broadcastOutstanding = false;
    QObject::connect(m_model.get(), &SessionModel::dependencyChanged, m_model.get(),
                     [this](const QString &sessionId, const DependencyKey &key) {
                         m_mirror[sessionId].remove(key);
                     });
}

bool ModelRun::verifyMirror(const SessionData &session, const QString &id)
{
    if (m_broadcastOutstanding)
        return true;
    // Catalogue order, not hash order: the first failure reported is reproducible.
    const QHash<DependencyKey, CalculationEngine::Value> mirror = m_mirror.value(id);
    for (const DependencyKey &name : std::as_const(m_catalogue)) {
        const auto it = mirror.constFind(name);
        if (it == mirror.constEnd())
            continue;
        const CalculationEngine::Value fresh = session.calculationEngine().evaluateFresh(name);
        if (!CalculationEngine::sameValue(it.value(), fresh)) {
            return fail(QStringLiteral("session=%1 name=%2 a subscriber still holds %3 but the value is %4: "
                                       "no dependencyChanged was emitted")
                            .arg(id, describe(name), oracleDescribe(it.value()), oracleDescribe(fresh)));
        }
    }
    return true;
}

void ModelRun::tearDown()
{
    m_altitudes.reset();
    writeAltitudes({});
    m_model.reset();
    TestEnvironment::instance().resetPreferencesToDefaults();
}

bool ModelRun::start()
{
    TestEnvironment &env = TestEnvironment::instance();
    env.useFreshLogbook();
    env.resetPreferencesToDefaults();
    LogbookManager::instance().initialize();

    m_model = std::make_unique<SessionModel>();
    connectMirror();
    m_altitudes = std::make_unique<AltitudeMarkerManager>();
    writeAltitudes({});
    PreferencesManager::instance().setValue(PreferenceKeys::AltitudeMarkersUnits, QStringLiteral("Metric"));
    m_altitudes->refresh();

    const QString root = env.newTempDir(QStringLiteral("oracle-model"));
    for (const QString &id : m_ids) {
        const QList<OracleFragment> fragments = oracleFragments(id.toLatin1());
        m_paths[id] = writeOracleFragments(fragments, root + QLatin1Char('/') + id);
        if (m_fragmentNames.isEmpty()) {
            for (const OracleFragment &fragment : fragments)
                m_fragmentNames.append(fragment.name);
        }
    }

    // Fixed prefix, through the application's import path. Fragment 0 is
    // TRACK, fragment 1 is SENSOR; o2 gets them in reverse order.
    for (int i = 0; i < m_ids.size(); ++i) {
        const QStringList &paths = m_paths[m_ids.at(i)];
        const QStringList order = i == 0 ? QStringList{paths.at(0), paths.at(1)}
                                         : QStringList{paths.at(1), paths.at(0)};
        const SessionImport::BatchResult batch = SessionImport::importFiles(*m_model, order);
        m_log.append(0, m_ids.at(i), i == 0 ? QStringLiteral("import TRACK SENSOR") : QStringLiteral("import SENSOR TRACK"));
        if (batch.files.size() != 2 || batch.files.at(0).outcome != Outcome::Created
            || batch.files.at(1).outcome != Outcome::Merged)
            return fail(QStringLiteral("prefix import of %1: %2").arg(m_ids.at(i), SessionImport::failureMessage(batch.failures(), QString())));
    }
    if (m_model->rowCount() != 2)
        return fail(QStringLiteral("prefix: %1 rows").arg(m_model->rowCount()));

    // Literal checkpoint
    for (const QString &id : m_ids) {
        const SessionData &s = m_model->sessionRef(m_model->getSessionRow(id));
        if (s.getAttribute("_EXIT_TIME").toDouble() != T0 + 9.0
            || s.getAttribute("_DURATION").toDouble() != 295.0
            || !isNear(s.getMeasurement("IMU", "wTotal").value(0), 5.7344)
            || s.getAttribute("_DESCRIPTION").toString() != QLatin1String("24-01-01/12-00-00"))
            return fail(QStringLiteral("prefix checkpoint of %1").arg(id));
    }

    // Both sessions exist on disk from here on (a simulated crash loses edits,
    // never a session).
    if (!waitForIdle(*m_model, 20000))
        return fail(QStringLiteral("prefix: not idle"));
    return true;
}

// An application restart: every row is a stub again; the next read or merge
// loads it. `crash` = the process dies instead of shutting down: index.json is
// as the last column task left it, dirty sessions are NOT saved, their edits
// are lost. Even then the cached columns on disk must not disagree
// with the session files on disk.
bool ModelRun::restart(bool crash)
{
    QJsonObject crashIndex;
    if (crash) {
        LogbookManager::instance().flushIndex();    // what a ColumnTask completion does
        crashIndex = readIndex();
    } else if (!waitForIdle(*m_model, 20000)) {
        return fail(QStringLiteral("restart: not idle"));
    }
    m_altitudes.reset();
    m_model.reset();

    LogbookManager &logbook = LogbookManager::instance();
    TestEnvironment::instance().reopenLogbook();
    logbook.initialize();

    m_model = std::make_unique<SessionModel>();
    m_model->populateFromIndex(logbook.cachedColumnValues(LogbookColumnStore::instance().enabledColumns()),
                               logbook.lastAccessedMap());
    m_model->startColumnWorker();
    connectMirror();
    m_altitudes = std::make_unique<AltitudeMarkerManager>();
    m_altitudes->refresh();

    // The index the crash left behind is valid, per column, for the
    // environment it records for that column. Where that is the column's
    // current one, none of its values may disagree with the session file
    // next to it.
    if (crash && crashIndex[QStringLiteral("calculationCompatibility")].toInt() == 2) {
        const QStringList keys = columnAttributeKeys();
        const QVector<LogbookColumn> columns = LogbookColumnStore::instance().enabledColumns();
        if (columns.size() != keys.size())
            return fail(QStringLiteral("crash: %1 columns").arg(columns.size()));
        for (const QString &id : m_ids) {
            const std::optional<SessionData> saved = logbook.loadSession(id);
            if (!saved)
                return fail(QStringLiteral("crash: session %1 does not load").arg(id));
            for (int column = 0; column < keys.size(); ++column) {
                if (indexColumnEnvironment(crashIndex, columns.at(column))
                    != logbookColumnEnvironment(columns.at(column), CalculationRegistry::instance()))
                    continue;       // valid for another environment: not kept at a start in this one
                const QJsonValue value = indexValue(crashIndex, id, column);
                if (!value.isDouble() && !value.isString())
                    continue;       // nothing cached: always consistent
                const QVariant expected = saved->getAttribute(keys.at(column));
                if (!sameColumnValue(value.toVariant(), expected)) {
                    return fail(QStringLiteral("crash: session=%1 index.json holds '%2' for %3, the session file gives '%4'")
                                    .arg(id, value.toVariant().toString(), keys.at(column), expected.toString()));
                }
            }
        }
    }

    if (m_model->rowCount() != 2)
        return fail(QStringLiteral("restart: %1 rows").arg(m_model->rowCount()));
    for (const QString &id : m_ids) {
        if (m_model->getSessionRow(id) < 0)
            return fail(QStringLiteral("restart: session %1 is gone").arg(id));
    }
    return true;
}

// Never forces a load just to verify: that would touch the LRU and change the
// sequence under test.
bool ModelRun::verifyLoadedRows()
{
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const SessionRow &sr = std::as_const(*m_model).rowAt(row);
        if (!sr.isLoaded() || sr.loadFailed)
            continue;
        if (!verifyMirror(*sr.session, sr.sessionId) || !verify(*sr.session, sr.sessionId, randomNames(kSampleNames)))
            return false;
    }
    return true;
}

bool ModelRun::importFragment(const QString &id, int fragment)
{
    // Settle first, so that the file on disk is the session's current state.
    if (!waitForIdle(*m_model, 20000))
        return fail(QStringLiteral("import: not idle"));
    m_broadcastOutstanding = false;
    const QString csvPath = sessionFilePath(id);
    if (csvPath.isEmpty())
        return fail(QStringLiteral("import: session %1 has no file").arg(id));
    const QByteArray before = readFileBytes(csvPath);

    const SessionImport::BatchResult batch = SessionImport::importFiles(*m_model, {m_paths[id].at(fragment)});
    if (batch.files.size() != 1)
        return fail(QStringLiteral("import: %1 results").arg(batch.files.size()));
    const MergeResult &result = batch.files.first();

    const char *text = "?";
    switch (result.outcome) {
    case Outcome::Created:   text = "Created"; break;
    case Outcome::Merged:    text = "Merged"; break;
    case Outcome::Unchanged: text = "Unchanged"; break;
    case Outcome::Failed:    text = "Failed"; break;
    }
    m_log.append(m_step, id, QStringLiteral("import %1 %2").arg(m_fragmentNames.at(fragment), QLatin1String(text)));

    if (result.outcome == Outcome::Created)
        return fail(QStringLiteral("import of %1 created a second session").arg(m_fragmentNames.at(fragment)));
    if (result.error.contains(QLatin1String("rows but")))
        return fail(QStringLiteral("ragged merge: %1").arg(result.error));
    if (m_model->rowCount() != 2)
        return fail(QStringLiteral("import: %1 rows").arg(m_model->rowCount()));

    if (result.outcome == Outcome::Failed || result.outcome == Outcome::Unchanged) {
        if (!waitForIdle(*m_model, 20000))
            return fail(QStringLiteral("import: not idle afterwards"));
        if (readFileBytes(csvPath) != before)
            return fail(QStringLiteral("a %1 import rewrote the session file").arg(QLatin1String(text)));
    }
    return true;
}

bool ModelRun::operation()
{
    const QString id = m_ids.at(pick(2));
    const int kind = pick(100);

    if (kind < 35) {                                        // 35 %: read through the model
        const DependencyKey name = m_catalogue.at(pick(int(m_catalogue.size())));
        m_log.append(m_step, id, QStringLiteral("read %1").arg(describe(name)));
        const SessionData &session = m_model->sessionRef(m_model->getSessionRow(id));
        QList<DependencyKey> held = subscriberNames();
        held.prepend(name);
        if (!verify(session, id, held))
            return false;
        for (const DependencyKey &heldName : std::as_const(held))
            m_mirror[id].insert(heldName, oracleRead(session, heldName));
    } else if (kind < 60) {                                 // 25 %: attribute edit
        const QList<OracleAttributeEdit> edits = oracleAttributeEdits();
        const OracleAttributeEdit &edit = edits.at(pick(int(edits.size())));
        const QVariant value = edit.values.at(pick(int(edit.values.size())));
        m_log.append(m_step, id, QStringLiteral("updateAttribute %1 %2").arg(edit.key, valueText(value)));
        if (value.isValid())
            m_model->updateAttribute(id, edit.key, value);
        else
            m_model->removeAttribute(id, edit.key);
    } else if (kind < 72) {                                 // 12 %: import one fragment file
        if (!importFragment(id, pick(int(m_fragmentNames.size()))))
            return false;
    } else if (kind < 77) {                                 // 5 %: preference
        const QList<PreferenceEdit> edits = preferenceEdits();
        const PreferenceEdit &edit = edits.at(pick(int(edits.size())));
        const double value = edit.values.at(pick(int(edit.values.size())));
        m_log.append(m_step, id, QStringLiteral("preference %1 %2").arg(edit.key).arg(value));
        PreferencesManager::instance().setValue(edit.key, value);
        m_broadcastOutstanding = true;
    } else if (kind < 82) {                                 // 5 %: altitude list
        const QList<QList<int>> lists = {{}, {1000}, {1000, 2000}, {2000}};
        const int index = pick(int(lists.size()));
        m_log.append(m_step, id, QStringLiteral("altitudes #%1").arg(index));
        writeAltitudes(lists.at(index));
        m_broadcastOutstanding = true;
    } else if (kind < 85) {                                 // 3 %
        m_log.append(m_step, id, QStringLiteral("flushPendingInvalidations"));
        m_model->flushPendingInvalidations();
        m_broadcastOutstanding = false;
    } else if (kind < 90) {                                 // 5 %
        m_log.append(m_step, id, QStringLiteral("waitForIdle"));
        if (!waitForIdle(*m_model, 20000))
            return fail(QStringLiteral("not idle"));
        m_broadcastOutstanding = false;
    } else {                                                // 10 %: restart (3 in 10: a crash)
        const bool crash = kind >= 97;
        m_log.append(m_step, id, crash ? QStringLiteral("crash") : QStringLiteral("restart"));
        if (!restart(crash))
            return false;
    }

    return verifyLoadedRows();
}

// Acceptance 5 under random histories, and the column-cache rule: what is on disk is what is
// in memory, effective values survive the reload, and the cached columns never
// disagree with the saved file.
bool ModelRun::persistedState()
{
    m_model->flushPendingInvalidations();
    if (!waitForIdle(*m_model, 20000))
        return fail(QStringLiteral("end: not idle"));

    LogbookManager &logbook = LogbookManager::instance();
    const QVector<LogbookColumn> columns = LogbookColumnStore::instance().enabledColumns();
    const QMap<QString, QMap<int, QVariant>> cachedValues = logbook.cachedColumnValues(columns);

    for (const QString &id : m_ids) {
        const SessionData &live = m_model->sessionRef(m_model->getSessionRow(id));
        const std::optional<SessionData> fresh = logbook.loadSession(id);
        if (!fresh)
            return fail(QStringLiteral("end: session %1 does not load").arg(id));

        if (!(fresh->sourceData() == live.sourceData()))
            return fail(QStringLiteral("end: session %1: the saved source data differs from memory").arg(id));

        for (const DependencyKey &name : m_catalogue) {
            const CalculationEngine::Value a = oracleRead(live, name);
            const CalculationEngine::Value b = oracleRead(*fresh, name);
            if (!CalculationEngine::sameValue(a, b)) {
                return fail(QStringLiteral("end: session=%1 name=%2 live=%3 reloaded=%4")
                                .arg(id, describe(name), oracleDescribe(a), oracleDescribe(b)));
            }
        }

        const QStringList columnKeys = columnAttributeKeys();
        for (int column = 0; column < columnKeys.size(); ++column) {
            const QVariant cached = cachedValues.value(id).value(column);
            const QVariant expected = fresh->getAttribute(columnKeys.at(column));
            if (!sameColumnValue(cached, expected)) {
                return fail(QStringLiteral("end: session=%1 cached column %2 is '%3', the saved file gives '%4'")
                                .arg(id, columnKeys.at(column), cached.toString(), expected.toString()));
            }
        }
    }

    if (!logbook.flushIndex() && logbook.indexNeedsFlush())
        return fail(QStringLiteral("end: index.json could not be written"));
    if (readIndex()[QStringLiteral("calculationCompatibility")].toInt() != 2)
        return fail(QStringLiteral("end: index.json has no calculationCompatibility 2"));
    return true;
}

bool ModelRun::execute()
{
    if (!start())
        return false;
    for (m_step = 1; m_step <= m_operations; ++m_step) {
        if (!operation())
            return false;
    }
    return persistedState();
}

} // namespace

class SessionOracleTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void sessionSequences_data();
    void sessionSequences();
    void modelSequences_data();
    void modelSequences();

private:
    template <typename Run>
    void runSeed(int defaultOperations);

    QStringList m_registryBefore;
    QtMessageHandler m_previousHandler = nullptr;
};

void SessionOracleTest::initTestCase()
{
    // As the application does: registrations are complete before initialize().
    TestEnvironment::instance().registerBuiltIns();
    PreferencesManager::instance().registerPreference(PreferenceKeys::LogbookColumnsVersion, 0);

    LogbookColumn description;
    description.type = ColumnType::SessionAttribute;
    description.attributeKey = QStringLiteral("_DESCRIPTION");
    LogbookColumn exitTime;
    exitTime.type = ColumnType::SessionAttribute;
    exitTime.attributeKey = QStringLiteral("_EXIT_TIME");
    LogbookColumn gyro;
    gyro.type = ColumnType::MeasurementAtMarker;
    gyro.sensorID = QStringLiteral("IMU");
    gyro.measurementID = QStringLiteral("wx");
    gyro.measurementType = QStringLiteral("rotation");
    gyro.markerAttributeKey = QStringLiteral("_M");
    LogbookColumn altitude;
    altitude.type = ColumnType::SessionAttribute;
    altitude.attributeKey = QStringLiteral("_ALTITUDE_1000_M");
    LogbookColumnStore::instance().setColumns({description, exitTime, gyro, altitude});
}

void SessionOracleTest::init()
{
    TestEnvironment::instance().resetPreferencesToDefaults();
    m_registryBefore = CalculationRegistry::instance().registeredIds();
    m_previousHandler = qInstallMessageHandler(quietHandler);
}

// The global registry and the preferences are left as they were found.
void SessionOracleTest::cleanup()
{
    qInstallMessageHandler(m_previousHandler);
    TestEnvironment::instance().resetPreferencesToDefaults();
    QCOMPARE(CalculationRegistry::instance().registeredIds(), m_registryBefore);
    QCOMPARE(CalculationRegistry::instance().enrolledEngineCount(), 0);
}

template <typename Run>
void SessionOracleTest::runSeed(int defaultOperations)
{
    QFETCH(unsigned, seed);

    QString failure;
    {
        Run run(seed, oracleOperationCount(defaultOperations));
        if (!run.execute()) {
            failure = run.failure();
            qInstallMessageHandler(m_previousHandler);
            run.log().dump();       // the whole history first, then the verdict
        }

        // Opt-in: append every operation log to a file, e.g. to diff two runs
        // of one seed. Never set by CTest.
        const QString logPath = qEnvironmentVariable("FLYSIGHT_ORACLE_LOG");
        if (!logPath.isEmpty()) {
            QFile file(logPath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
                file.write(QStringLiteral("== %1 seed %2\n").arg(QLatin1String(QTest::currentTestFunction())).arg(seed).toUtf8());
                file.write(run.log().lines().join(QLatin1Char('\n')).toUtf8() + '\n');
            }
        }
    }
    if (!failure.isEmpty())
        QFAIL(qPrintable(failure + QStringLiteral(" (reproduce with FLYSIGHT_ORACLE_SEEDS=%1)").arg(seed)));
}

void SessionOracleTest::sessionSequences_data()
{
    QTest::addColumn<unsigned>("seed");
    for (unsigned seed : oracleSeeds(1, 20))
        QTest::newRow(qPrintable(QStringLiteral("seed %1").arg(seed))) << seed;
}

// Acceptance 10 (and 11, 13, 15, 16)
void SessionOracleTest::sessionSequences()
{
    runSeed<SessionRun>(300);
}

void SessionOracleTest::modelSequences_data()
{
    QTest::addColumn<unsigned>("seed");
    for (unsigned seed : oracleSeeds(1, 6))
        QTest::newRow(qPrintable(QStringLiteral("seed %1").arg(seed))) << seed;
}

// Acceptance 10 and 5 (and the column-cache rule of acceptance 18)
void SessionOracleTest::modelSequences()
{
    runSeed<ModelRun>(120);
}

FLYSIGHT_TEST_MAIN(SessionOracleTest)
#include "tst_session_oracle.moc"
