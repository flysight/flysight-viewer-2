#ifndef FLYSIGHT_ENGINE_CALCULATIONRESULT_H
#define FLYSIGHT_ENGINE_CALCULATIONRESULT_H

#include <QHash>
#include <QList>
#include <QString>
#include <QVariant>
#include <QVector>

#include "calctypes.h"

namespace FlySight {

/// The bundle a calculation returns: a value (or "unavailable") per output.
///
/// Any declared output that is not set is unavailable. An invalid QVariant or an
/// empty sample vector is normalized to "unavailable" when it is set, so a
/// calculation can report a partial result either by not setting an output or
/// by setting it to nothing.
///
/// The engine stores the bundle it accepts as one immutable object, which is
/// what makes multi-output publication atomic.
class CalculationResult {
public:
    CalculationResult() = default;                          ///< everything unavailable
    static CalculationResult unavailable() { return CalculationResult(); }

    CalculationResult &setAttribute(const QString &key, const QVariant &value);
    CalculationResult &setMeasurement(const QString &sensor, const QString &name,
                                      const QVector<double> &values, const QString &unit = QString());
    CalculationResult &setUnavailable(const DependencyKey &output);   ///< explicit form of "not set"

    bool contains(const DependencyKey &output) const;       ///< was set (available or not)
    bool isAvailable(const DependencyKey &output) const;

    QVariant        attributeValue(const QString &key) const;
    QVector<double> measurementValues(const QString &sensor, const QString &name) const;
    QString         measurementUnit(const QString &sensor, const QString &name) const;

    /// Every output that was set, in the order it was first set.
    QList<DependencyKey> setOutputs() const { return m_order; }

    /// Optional human-readable text saying why outputs are unavailable ("IMU
    /// gap of 2.3 s at 14:02:11"). A function of the inputs like every other
    /// part of the bundle, and cached with it; the engine reports it through
    /// CalculationEngine::resultDetail() and blocker inspection, and never
    /// interprets it. Empty means none.
    CalculationResult &setReason(const QString &text);
    QString reason() const { return m_reason; }

private:
    struct Output {
        bool available = false;
        QVariant attribute;
        QVector<double> samples;
        QString unit;
    };

    Output &slot(const DependencyKey &output);

    QHash<DependencyKey, Output> m_outputs;
    QList<DependencyKey> m_order;
    QString m_reason;
};

} // namespace FlySight

#endif // FLYSIGHT_ENGINE_CALCULATIONRESULT_H
