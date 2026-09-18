#ifndef FLYSIGHTTEST_FAKESESSIONSTATE_H
#define FLYSIGHTTEST_FAKESESSIONSTATE_H

#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVector>

#include "dependencykey.h"
#include "engine/calculationdescriptor.h"
#include "engine/calculationengine.h"
#include "engine/calculationregistry.h"
#include "engine/sessionstate.h"

namespace FlySightTest {

/// Synthetic persistent state for engine tests: stored attributes and source
/// measurements in plain maps, counting every read the engine makes.
class FakeSessionState : public FlySight::ISessionState {
public:
    // Mutators change the maps only; the test (or a convenience overload
    // below) notifies the engine.
    void setAttribute(const QString &key, const QVariant &v);
    void removeAttribute(const QString &key);
    void setMeasurement(const QString &sensor, const QString &name, const QVector<double> &v,
                        const QString &unit = {});
    void removeMeasurement(const QString &sensor, const QString &name);
    void setUnit(const QString &sensor, const QString &name, const QString &unit);

    // Convenience: mutate + notify, returning the engine's invalidated set.
    QSet<FlySight::DependencyKey> setAttribute(FlySight::CalculationEngine &e, const QString &key,
                                               const QVariant &v);
    QSet<FlySight::DependencyKey> removeAttribute(FlySight::CalculationEngine &e, const QString &key);
    QSet<FlySight::DependencyKey> setMeasurement(FlySight::CalculationEngine &e, const QString &sensor,
                                                 const QString &name, const QVector<double> &v,
                                                 const QString &unit = {});
    QSet<FlySight::DependencyKey> removeMeasurement(FlySight::CalculationEngine &e, const QString &sensor,
                                                    const QString &name);
    QSet<FlySight::DependencyKey> setUnit(FlySight::CalculationEngine &e, const QString &sensor,
                                          const QString &name, const QString &unit);

    /// Total ISessionState calls, for "does not compute / does not touch state" checks.
    int readCount() const { return m_reads; }
    void resetReadCount() { m_reads = 0; }

    // ISessionState
    bool            hasStoredAttribute(const QString &key) const override;
    QVariant        storedAttribute(const QString &key) const override;
    bool            hasSourceMeasurement(const QString &sensor, const QString &name) const override;
    QVector<double> sourceMeasurement(const QString &sensor, const QString &name) const override;
    QString         sourceUnit(const QString &sensor, const QString &name) const override;

private:
    using MeasurementKey = QPair<QString, QString>;

    QHash<QString, QVariant> m_attributes;
    QHash<MeasurementKey, QVector<double>> m_measurements;
    QHash<MeasurementKey, QString> m_units;
    mutable int m_reads = 0;
};

/// Synthetic preferences. An invalid QVariant means "no such preference".
class FakePreferenceProvider : public FlySight::IPreferenceProvider {
public:
    void set(const QString &key, const QVariant &v);                                    ///< map only
    void set(FlySight::CalculationRegistry &r, const QString &key, const QVariant &v);  ///< map + notify
    int readCount() const { return m_reads; }
    void resetReadCount() { m_reads = 0; }

    QVariant preferenceValue(const QString &key) const override;

private:
    QHash<QString, QVariant> m_values;
    mutable int m_reads = 0;
};

/// The synthetic calculations shared by the engine test executables. Attribute
/// values are int QVariants. Kept here, next to the fakes, so the three engine
/// test files describe one world instead of three copies of it.
///
/// | Id        | Inputs               | Outputs                                                        |
/// |-----------|----------------------|----------------------------------------------------------------|
/// | sum       | attr A, attr B       | X = A + B                                                      |
/// | fallbackX | attr C               | X = C * 10                                                     |
/// | constX    | -                    | X = -1                                                         |
/// | triple    | attr X, pref p       | Y = X + p, Z = X * 2, W = X * 3 if X >= 0 else unavailable     |
/// | wAlt      | attr Z               | W = Z + 1000                                                   |
/// | meas      | meas S/m, attr Y     | meas S/d[i] = m[i] + Y, unit "u"                               |
/// | neg       | family: "neg:<attr>" | that name = -value of <attr>                                   |
/// | P Q R S   | P: Y2; R: X2         | P: X2 = Y2 + 1, Q: X2 = 100, R: Y2 = X2 + 1, S: Y2 = 200       |
namespace Synthetic {

FlySight::CalculationDescriptor sum();
FlySight::CalculationDescriptor fallbackX();
FlySight::CalculationDescriptor constX();
FlySight::CalculationDescriptor triple(FlySight::EvaluationPolicy policy = FlySight::EvaluationPolicy::OnDemand);
FlySight::CalculationDescriptor wAlt();
FlySight::CalculationDescriptor meas();
FlySight::CalculationFamily     neg();
FlySight::CalculationDescriptor P();
FlySight::CalculationDescriptor Q();
FlySight::CalculationDescriptor R();
FlySight::CalculationDescriptor S();

/// Registers, in this order: sum, fallbackX, constX, triple, wAlt, meas, neg, P, Q, R, S.
void registerSharedWorld(FlySight::CalculationRegistry &registry);

// Shorthand for public names
inline FlySight::DependencyKey attr(const char *key)
{
    return FlySight::DependencyKey::attribute(QString::fromLatin1(key));
}
inline FlySight::DependencyKey measKey(const char *sensor, const char *name)
{
    return FlySight::DependencyKey::measurement(QString::fromLatin1(sensor), QString::fromLatin1(name));
}

} // namespace Synthetic

} // namespace FlySightTest

#endif // FLYSIGHTTEST_FAKESESSIONSTATE_H
