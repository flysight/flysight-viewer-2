#include "calculationresult.h"

namespace FlySight {

CalculationResult::Output &CalculationResult::slot(const DependencyKey &output)
{
    auto it = m_outputs.find(output);
    if (it == m_outputs.end()) {
        m_order.append(output);
        it = m_outputs.insert(output, Output());
    }
    return it.value();
}

CalculationResult &CalculationResult::setAttribute(const QString &key, const QVariant &value)
{
    Output &out = slot(DependencyKey::attribute(key));
    out = Output();
    if (value.isValid()) {
        out.available = true;
        out.attribute = value;
    }
    return *this;
}

CalculationResult &CalculationResult::setMeasurement(const QString &sensor, const QString &name,
                                                     const QVector<double> &values, const QString &unit)
{
    Output &out = slot(DependencyKey::measurement(sensor, name));
    out = Output();
    if (!values.isEmpty()) {
        out.available = true;
        out.samples = values;
        out.unit = unit;
    }
    return *this;
}

CalculationResult &CalculationResult::setUnavailable(const DependencyKey &output)
{
    slot(output) = Output();
    return *this;
}

CalculationResult &CalculationResult::setReason(const QString &text)
{
    m_reason = text;
    return *this;
}

bool CalculationResult::contains(const DependencyKey &output) const
{
    return m_outputs.contains(output);
}

bool CalculationResult::isAvailable(const DependencyKey &output) const
{
    const auto it = m_outputs.constFind(output);
    return it != m_outputs.constEnd() && it->available;
}

QVariant CalculationResult::attributeValue(const QString &key) const
{
    const auto it = m_outputs.constFind(DependencyKey::attribute(key));
    return it != m_outputs.constEnd() ? it->attribute : QVariant();
}

QVector<double> CalculationResult::measurementValues(const QString &sensor, const QString &name) const
{
    const auto it = m_outputs.constFind(DependencyKey::measurement(sensor, name));
    return it != m_outputs.constEnd() ? it->samples : QVector<double>();
}

QString CalculationResult::measurementUnit(const QString &sensor, const QString &name) const
{
    const auto it = m_outputs.constFind(DependencyKey::measurement(sensor, name));
    return it != m_outputs.constEnd() ? it->unit : QString();
}

} // namespace FlySight
