#include "evaluationcontext.h"

#include <QDebug>

namespace FlySight {

EvaluationContext::EvaluationContext(const QString &instanceId, bool quiet)
    : m_instanceId(instanceId)
    , m_quiet(quiet)
{
}

void EvaluationContext::provide(const CalcInput &input, const InputValue &value)
{
    m_inputs.insert(input, value);
}

const EvaluationContext::InputValue *EvaluationContext::declared(const CalcInput &input) const
{
    const auto it = m_inputs.constFind(input);
    if (it != m_inputs.constEnd())
        return &it.value();

    // Undeclared read: remember it for the engine, which discards the result.
    m_undeclaredReads.append(input);
    if (!m_quiet) {
        qWarning().noquote() << "Calculation" << m_instanceId
                             << "read an undeclared input:" << describe(input)
                             << "- its result is discarded";
    }
    return nullptr;
}

QVariant EvaluationContext::attribute(const QString &key) const
{
    const InputValue *v = declared(CalcInput::attribute(key));
    return v ? v->value : QVariant();
}

QVector<double> EvaluationContext::measurement(const QString &sensor, const QString &name) const
{
    const InputValue *v = declared(CalcInput::measurement(sensor, name));
    return v ? v->samples : QVector<double>();
}

QString EvaluationContext::measurementUnit(const QString &sensor, const QString &name) const
{
    const InputValue *v = declared(CalcInput::measurement(sensor, name));
    return v ? v->unit : QString();
}

QVariant EvaluationContext::preference(const QString &key) const
{
    const InputValue *v = declared(CalcInput::preference(key));
    return v ? v->value : QVariant();
}

QVector<double> EvaluationContext::sourceMeasurement(const QString &sensor, const QString &name) const
{
    const InputValue *v = declared(CalcInput::sourceMeasurement(sensor, name));
    return v ? v->samples : QVector<double>();
}

QString EvaluationContext::sourceUnit(const QString &sensor, const QString &name) const
{
    const InputValue *v = declared(CalcInput::sourceUnit(sensor, name));
    return v ? v->unit : QString();
}

bool EvaluationContext::isDeclared(const CalcInput &input) const
{
    return m_inputs.contains(input);
}

CalculationProgress &EvaluationContext::progress() const
{
    return m_progress ? *m_progress : CalculationProgress::none();
}

} // namespace FlySight
