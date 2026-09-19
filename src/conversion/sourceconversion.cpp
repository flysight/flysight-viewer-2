#include "sourceconversion.h"

#include <optional>

#include <QDebug>
#include <QVector>

#include "../dependencykey.h"
#include "../engine/calculationdescriptor.h"
#include "../units/unitconversion.h"
#include "schematable.h"

using namespace FlySight;

namespace {

// Instance keys must not contain '#' (it separates the family id from the
// instance key), and an instance the registry rejects counts as "family does
// not match". Resolution never falls through to derived candidates once
// conversion families exist, so an unescaped custom column such as "temp#1"
// would become unreadable. '%' is escaped first, and the '/' that joins sensor
// and name is escaped inside each part, so distinct measurements can never
// share an instance key (instances are memoized by key).
QString escapeKeyPart(QString text)
{
    text.replace(QLatin1Char('%'), QStringLiteral("%25"));
    text.replace(QLatin1Char('#'), QStringLiteral("%23"));
    text.replace(QLatin1Char('/'), QStringLiteral("%2F"));
    return text;
}

QString instanceKey(const QString &sensor, const QString &name)
{
    return escapeKeyPart(sensor) + QLatin1Char('/') + escapeKeyPart(name);
}

// The whole conversion: schema step first, unit step second.
//
// `in` shares its buffer with the session's source layer. Only const access is
// allowed on it: a single non-const call would detach and silently double the
// memory of every identity column.
CalculationResult convert(const QString &sensor, const QString &name, const QVector<double> &in,
                          const QString &unitText, double schemaScale)
{
    const ConversionSpec spec = UnitConversion::getConversion(unitText);
    const bool schemaIdentity = (schemaScale == 1.0);
    const bool unitIdentity = (spec.scale == 1.0 && spec.offset == 0.0);

    // Value identity (possibly with a new label): hand the source vector on, so
    // the effective measurement costs no second buffer.
    if (schemaIdentity && unitIdentity)
        return CalculationResult().setMeasurement(sensor, name, in, spec.siUnit);

    const qsizetype n = in.size();
    const double *src = in.constData();
    QVector<double> out(n);
    double *dst = out.data();
    for (qsizetype i = 0; i < n; ++i) {
        double v = src[i];
        if (!schemaIdentity)
            v *= schemaScale;
        // Skipped entirely when it is the identity: v * 1.0 + 0.0 would turn -0.0 into +0.0.
        if (!unitIdentity)
            v = v * spec.scale + spec.offset;
        dst[i] = v;
    }
    return CalculationResult().setMeasurement(sensor, name, out, spec.siUnit);
}

// Candidate 1: measurements some schema version corrects, in a session that
// records SCHEMA_VER.
std::optional<CalculationDescriptor> instantiateSchemaConversion(const DependencyKey &output)
{
    if (output.type != DependencyKey::Type::Measurement)
        return std::nullopt;

    const QString sensor = output.measurementKey.first;
    const QString name = output.measurementKey.second;
    if (!Schema::isSchemaDependent(sensor, name))
        return std::nullopt;

    const QString schemaKey = QString::fromLatin1(Schema::AttributeKey);

    CalculationDescriptor d;
    d.id = instanceKey(sensor, name);
    // This order is the order of the short-circuiting availability pass.
    d.inputs = {
        CalcInput::sourceMeasurement(sensor, name),
        CalcInput::sourceUnit(sensor, name),
        CalcInput::attribute(schemaKey)
    };
    d.outputs = { output };
    d.compute = [sensor, name, schemaKey](const EvaluationContext &ctx) -> CalculationResult {
        const QVariant recorded = ctx.attribute(schemaKey);
        const std::optional<int> version = Schema::parseVersion(recorded);

        double schemaScale = 1.0;
        if (version) {
            schemaScale = Schema::correctionScale(*version, sensor, name);
        } else {
            // Reachable only through a programmatic setAttribute: import and
            // logbook load reject such a file. An explicit but unrecognized
            // declaration is never treated as legacy data, and reporting
            // "unavailable" here would let the default candidate apply the
            // legacy correction - so the values stay as recorded.
            qWarning("%s '%s' is not supported; %s/%s left as recorded",
                     Schema::AttributeKey, qPrintable(recorded.toString()),
                     qPrintable(sensor), qPrintable(name));
        }

        return convert(sensor, name, ctx.sourceMeasurement(sensor, name),
                       ctx.sourceUnit(sensor, name), schemaScale);
    };
    return d;
}

// Candidate 2: every measurement. It does not look at SCHEMA_VER; for a
// schema-dependent measurement it is reached only when candidate 1 could not
// run, i.e. when the attribute is absent.
std::optional<CalculationDescriptor> instantiateDefaultConversion(const DependencyKey &output)
{
    if (output.type != DependencyKey::Type::Measurement)
        return std::nullopt;

    const QString sensor = output.measurementKey.first;
    const QString name = output.measurementKey.second;

    CalculationDescriptor d;
    d.id = instanceKey(sensor, name);
    d.inputs = {
        CalcInput::sourceMeasurement(sensor, name),
        CalcInput::sourceUnit(sensor, name)
    };
    d.outputs = { output };
    d.compute = [sensor, name](const EvaluationContext &ctx) -> CalculationResult {
        // The only place where absence of SCHEMA_VER is interpreted.
        const double schemaScale = Schema::correctionScale(Schema::ImpliedVersion, sensor, name);
        return convert(sensor, name, ctx.sourceMeasurement(sensor, name),
                       ctx.sourceUnit(sensor, name), schemaScale);
    };
    return d;
}

void addSourceConversion(CalculationRegistry &registry, const char *id,
                         std::optional<CalculationDescriptor> (*instantiate)(const DependencyKey &))
{
    CalculationFamily family;
    family.id = QString::fromLatin1(id);
    family.instantiate = instantiate;

    const bool ok = registry.registerSourceConversion(family);
    Q_ASSERT_X(ok, "Calculations::registerSourceConversions", id);
    Q_UNUSED(ok);
}

} // namespace

void Calculations::registerSourceConversions(CalculationRegistry &registry)
{
    // Order matters: candidates are tried in registration order.
    addSourceConversion(registry, "builtin.conversion.schema", instantiateSchemaConversion);
    addSourceConversion(registry, "builtin.conversion.default", instantiateDefaultConversion);
}
