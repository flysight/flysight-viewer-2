#ifndef FLYSIGHT_ENGINE_SESSIONSTATE_H
#define FLYSIGHT_ENGINE_SESSIONSTATE_H

#include <QString>
#include <QVariant>
#include <QVector>

namespace FlySight {

/// The narrow window through which the calculation engine sees one session's
/// persistent state: stored attributes and source measurements with their
/// recorded unit text. Nothing derived is visible here.
///
/// Contract:
///  - every method is a pure read of persistent state;
///  - none may call back into the engine, and none may compute anything;
///  - whoever mutates that state must afterwards call the matching
///    CalculationEngine notification (attributeChanged,
///    sourceMeasurementChanged, sourceUnitChanged, or clear).
class ISessionState {
public:
    virtual ~ISessionState() = default;

    /// True when the attribute is stored, even if its stored value is an invalid QVariant.
    virtual bool            hasStoredAttribute(const QString &key) const = 0;
    virtual QVariant        storedAttribute(const QString &key) const = 0;

    /// True when the measurement is present in the source data, even if it has no samples.
    virtual bool            hasSourceMeasurement(const QString &sensor, const QString &name) const = 0;
    virtual QVector<double> sourceMeasurement(const QString &sensor, const QString &name) const = 0;
    /// Recorded unit text of a source measurement; empty text is a value.
    virtual QString         sourceUnit(const QString &sensor, const QString &name) const = 0;
};

/// Pull access to preferences that calculations declare as inputs.
///
/// Change notification is push and not part of this interface: the owner of the
/// real preferences calls CalculationRegistry::notifyPreferenceChanged(key).
class IPreferenceProvider {
public:
    virtual ~IPreferenceProvider() = default;

    /// An invalid QVariant means "no such preference" (the input is unavailable).
    virtual QVariant preferenceValue(const QString &key) const = 0;
};

} // namespace FlySight

#endif // FLYSIGHT_ENGINE_SESSIONSTATE_H
