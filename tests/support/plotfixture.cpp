#include "plotfixture.h"

#include <QMap>

#include "engine/calculationdescriptor.h"
#include "engine/calculationregistry.h"
#include "engine/calculationresult.h"
#include "engine/evaluationcontext.h"
#include "sessionmodel.h"

using namespace FlySight;

namespace FlySightTest {

namespace {

const char kSensor[] = "Syn";

/// On demand: Syn/<measurement> = {<input> * factor}
CalculationDescriptor bridge(const char *id, const char *input, const char *measurement, int factor = 1)
{
    const QString in = QString::fromLatin1(input);
    const QString sensor = QString::fromLatin1(kSensor);
    const QString name = QString::fromLatin1(measurement);

    CalculationDescriptor d;
    d.id = QString::fromLatin1(id);
    d.inputs = {CalcInput::attribute(in)};
    d.outputs = {DependencyKey::measurement(sensor, name)};
    d.compute = [in, sensor, name, factor](const EvaluationContext &ctx) {
        const double value = ctx.attribute(in).toDouble() * factor;
        return CalculationResult().setMeasurement(sensor, name, {value});
    };
    return d;
}

/// Explicit, pure, no gate: H_OUT = G_OUT + 1. The second link of a chain whose
/// first link (gated) a test can hold.
CalculationDescriptor afterG()
{
    CalculationDescriptor d;
    d.id = QStringLiteral("afterG");
    d.title = QStringLiteral("After G");
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute(QStringLiteral("G_OUT"))};
    d.outputs = {DependencyKey::attribute(QStringLiteral("H_OUT"))};
    d.compute = [](const EvaluationContext &ctx) {
        return CalculationResult().setAttribute(QStringLiteral("H_OUT"),
                                                ctx.attribute(QStringLiteral("G_OUT")).toInt() + 1);
    };
    return d;
}

} // namespace

PlotFixture::PlotFixture()
{
    CalculationRegistry &registry = CalculationRegistry::instance();
    const QStringList before = registry.registeredIds();

    registry.registerCalculation(afterG());
    registry.registerCalculation(bridge("plotG", "G_OUT", "g"));
    registry.registerCalculation(bridge("plotG2", "G_OUT", "g2", 2));
    registry.registerCalculation(bridge("plotDB", "DB", "db"));
    registry.registerCalculation(bridge("plotEA", "EA1", "ea"));
    registry.registerCalculation(bridge("plotT", "T_OUT", "t"));
    registry.registerCalculation(bridge("plotPlain", "P_IN", "plain"));
    registry.registerCalculation(bridge("plotH", "H_OUT", "h"));

    // What was added, not what was asked for (see JobWorld)
    const QStringList after = registry.registeredIds();
    for (const QString &id : after) {
        if (!before.contains(id))
            m_ids.append(id);
    }
}

PlotFixture::~PlotFixture()
{
    CalculationRegistry &registry = CalculationRegistry::instance();
    for (auto it = m_ids.crbegin(); it != m_ids.crend(); ++it)
        registry.unregister(*it);
}

QVector<PlotValue> PlotFixture::plots()
{
    QVector<PlotValue> result;
    for (const char *measurement : {"g", "g2", "db", "ea", "t", "plain", "h"}) {
        PlotValue plot;
        plot.category = QStringLiteral("Synthetic");
        plot.plotName = QString::fromLatin1(measurement);
        plot.sensorID = QString::fromLatin1(kSensor);
        plot.measurementID = QString::fromLatin1(measurement);
        plot.role = PlotRole::Dependent;
        result.append(plot);
    }
    return result;
}

void PlotFixture::show(SessionModel &model, const QStringList &ids, bool visible)
{
    QMap<int, bool> rows;
    for (const QString &id : ids) {
        const int row = model.getSessionRow(id);
        if (row >= 0)
            rows.insert(row, visible);
    }
    model.setRowsVisibility(rows);
}

bool PlotFixture::giveInput(SessionModel &model, const QString &id, const QString &key, double value)
{
    return model.updateAttribute(id, key, value);
}

} // namespace FlySightTest
