#ifndef FLYSIGHTTEST_BUILTINFIXTURE_H
#define FLYSIGHTTEST_BUILTINFIXTURE_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariant>
#include <QVector>

#include "dependencykey.h"
#include "fakesessionstate.h"
#include "fixturebuilder.h"
#include "sessiondata.h"

namespace FlySightTest {

/// A generated jump on which almost every built-in output can be derived by
/// hand: 296 GNSS rows at 1 Hz (level flight, a 10-second linear acceleration
/// to 50 m/s, 54 s of freefall, a canopy descent at 5 m/s with a 5-second
/// climb at rows 200..204, then 30 s on the ground) plus a three-row sensor
/// file whose time fit is exactly a = 1, b = T0.
namespace DescentFixture {

constexpr double T0 = 1704110400.0;     // 2024-01-01T12:00:00Z

Fs2FileBuilder trackFile(const QByteArray &sessionId = "descent");
Fs2FileBuilder sensorFile(const QByteArray &sessionId = "descent");

/// Imports both files from a fresh temp dir with DataImporter and merges the
/// sensor session into the track session using only public API (setAttribute
/// for keys the target lacks, mergeSourceData for the measurements, so the
/// merged session holds the data as recorded).
FlySight::SessionData load(const QByteArray &sessionId = "descent");

/// The sensor file alone, imported.
FlySight::SessionData loadSensorOnly(const QByteArray &sessionId = "descent");

} // namespace DescentFixture

struct GoldenSample {
    int index;
    double value;
};

/// One expected built-in output on DescentFixture::load() with default
/// preferences (descent pause 30 s, automatic ground reference).
struct GoldenValue {
    FlySight::DependencyKey name;
    bool available;
    QVariant attribute;             ///< attributes: a double, or a QString compared exactly
    QList<GoldenSample> samples;    ///< measurements: samples checked by index
    int sampleCount;                ///< measurements: expected size
    double tolerance;
};

/// Literals only: nothing here is computed by the code under test.
QList<GoldenValue> goldenValues();

/// Text form of a public name, used as the data-row tag.
QString goldenTag(const FlySight::DependencyKey &name);

/// Compares what was read for `golden.name` with the literal. Returns an empty
/// string when they agree, otherwise a description of the first difference.
QString compareToGolden(const GoldenValue &golden, const QVariant &attribute,
                        const QVector<double> &samples);

/// Copies the stored state (stored attributes and the source layer: recorded
/// samples and unit text) of a session into a fake session state.
void copyStoredState(const FlySight::SessionData &from, FakeSessionState &to);

} // namespace FlySightTest

#endif // FLYSIGHTTEST_BUILTINFIXTURE_H
