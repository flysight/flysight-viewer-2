#ifndef FLYSIGHT_ENGINE_EVALUATIONCONTEXT_H
#define FLYSIGHT_ENGINE_EVALUATIONCONTEXT_H

#include <QHash>
#include <QList>
#include <QString>
#include <QVariant>
#include <QVector>

#include "calctypes.h"

namespace FlySight {

/// The only window a calculation has onto session state: read access to the
/// inputs it declared, valid for the duration of one compute call.
///
/// Every value served here was resolved by the engine *before* compute was
/// called, so a read never starts a resolution and compute calls never nest.
///
/// Reading something that was not declared is an error, reported without a C++
/// exception: the accessor returns an invalid/empty value, one warning is
/// logged, and the engine discards whatever compute returns (status
/// UndeclaredRead, all outputs unavailable).
///
/// Deliberately absent: the session, the engine, which output was requested,
/// the clock, and random numbers. A calculation is a pure function of its
/// declared inputs.
class EvaluationContext {
public:
    QVariant        attribute(const QString &key) const;
    QVector<double> measurement(const QString &sensor, const QString &name) const;
    /// Unit of a declared Measurement input.
    QString         measurementUnit(const QString &sensor, const QString &name) const;
    QVariant        preference(const QString &key) const;
    QVector<double> sourceMeasurement(const QString &sensor, const QString &name) const;
    QString         sourceUnit(const QString &sensor, const QString &name) const;

    /// Whether `input` is one of the calculation's declared inputs. Silent: it
    /// records nothing, warns nothing, and does not mark the evaluation. It
    /// exists because an empty value can be legitimate (a recorded unit may be
    /// empty), so "empty return" cannot be used to detect an undeclared read.
    bool            isDeclared(const CalcInput &input) const;

private:
    friend class CalculationEngine;

    struct InputValue {
        QVariant value;             // Attribute / Preference
        QVector<double> samples;    // Measurement / SourceMeasurement
        QString unit;               // Measurement / SourceUnit
    };

    EvaluationContext(const QString &instanceId, bool quiet);
    Q_DISABLE_COPY_MOVE(EvaluationContext)

    void provide(const CalcInput &input, const InputValue &value);
    const InputValue *declared(const CalcInput &input) const;

    QString m_instanceId;
    bool m_quiet;
    QHash<CalcInput, InputValue> m_inputs;
    mutable QList<CalcInput> m_undeclaredReads;
};

} // namespace FlySight

#endif // FLYSIGHT_ENGINE_EVALUATIONCONTEXT_H
