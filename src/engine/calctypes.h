#ifndef FLYSIGHT_ENGINE_CALCTYPES_H
#define FLYSIGHT_ENGINE_CALCTYPES_H

#include <QHash>
#include <QString>
#include <QStringList>

#include "../dependencykey.h"

// Vocabulary of the calculation engine.
//
//  - DependencyKey (unchanged, from dependencykey.h) is the *public name* of an
//    attribute or a measurement: what ordinary consumers read, what a
//    calculation declares as an output, and what invalidation reports.
//  - CalcInput is a *declared input* of a calculation. Besides public names it
//    can express a preference and, for the conversion layer only, the source
//    layer of a measurement.
//  - GraphNode is an identity in the per-session dependency graph.
//
// CalcInput and GraphNode never reach the UI or Python in this form.

namespace FlySight {

/// Stable identity of a registered calculation, e.g. "builtin.analysisRange".
/// Must not contain '#', which separates a family id from its instance key.
using CalculationId = QString;

enum class EvaluationPolicy {
    OnDemand,   ///< runs when one of its outputs is first read
    Explicit    ///< runs only through CalculationEngine::request()
};

/// One declared input of a calculation. All declared inputs are required.
struct CalcInput {
    enum class Kind { Attribute, Measurement, Preference, SourceMeasurement, SourceUnit };

    Kind kind = Kind::Attribute;
    QString key;            ///< Attribute / Preference key
    QString sensor, name;   ///< Measurement / Source* identity

    static CalcInput attribute(const QString &key)
    {
        CalcInput in;
        in.kind = Kind::Attribute;
        in.key = key;
        return in;
    }
    static CalcInput measurement(const QString &sensor, const QString &name)
    {
        CalcInput in;
        in.kind = Kind::Measurement;
        in.sensor = sensor;
        in.name = name;
        return in;
    }
    static CalcInput preference(const QString &key)
    {
        CalcInput in;
        in.kind = Kind::Preference;
        in.key = key;
        return in;
    }
    static CalcInput sourceMeasurement(const QString &sensor, const QString &name)
    {
        CalcInput in;
        in.kind = Kind::SourceMeasurement;
        in.sensor = sensor;
        in.name = name;
        return in;
    }
    static CalcInput sourceUnit(const QString &sensor, const QString &name)
    {
        CalcInput in;
        in.kind = Kind::SourceUnit;
        in.sensor = sensor;
        in.name = name;
        return in;
    }

    bool isSourceKind() const
    {
        return kind == Kind::SourceMeasurement || kind == Kind::SourceUnit;
    }
};

inline bool operator==(const CalcInput &lhs, const CalcInput &rhs)
{
    return lhs.kind == rhs.kind && lhs.key == rhs.key
        && lhs.sensor == rhs.sensor && lhs.name == rhs.name;
}
inline bool operator!=(const CalcInput &lhs, const CalcInput &rhs) { return !(lhs == rhs); }

inline size_t qHash(const CalcInput &in, size_t seed = 0)
{
    return qHashMulti(seed, static_cast<int>(in.kind), in.key, in.sensor, in.name);
}

/// An identity in the per-session dependency graph.
///
/// Leaves (persistent state): StoredAttribute, SourceMeasurement, SourceUnit,
/// Preference. Cached nodes: Resolution (the candidate choice behind a public
/// name; for a measurement this is the effective measurement) and Result (one
/// calculation instance's whole result bundle).
struct GraphNode {
    enum class Kind { StoredAttribute, SourceMeasurement, SourceUnit, Preference, Resolution, Result };

    Kind kind = Kind::StoredAttribute;
    QString a, b;                   ///< key | sensor,name | instance id
    bool measurementName = false;   ///< Resolution only: the public name is a measurement

    static GraphNode storedAttribute(const QString &key)
    {
        GraphNode n;
        n.kind = Kind::StoredAttribute;
        n.a = key;
        return n;
    }
    static GraphNode sourceMeasurement(const QString &sensor, const QString &name)
    {
        GraphNode n;
        n.kind = Kind::SourceMeasurement;
        n.a = sensor;
        n.b = name;
        return n;
    }
    static GraphNode sourceUnit(const QString &sensor, const QString &name)
    {
        GraphNode n;
        n.kind = Kind::SourceUnit;
        n.a = sensor;
        n.b = name;
        return n;
    }
    static GraphNode preference(const QString &key)
    {
        GraphNode n;
        n.kind = Kind::Preference;
        n.a = key;
        return n;
    }
    static GraphNode resolution(const DependencyKey &name)
    {
        GraphNode n;
        n.kind = Kind::Resolution;
        if (name.type == DependencyKey::Type::Measurement) {
            n.measurementName = true;
            n.a = name.measurementKey.first;
            n.b = name.measurementKey.second;
        } else {
            n.a = name.attributeKey;
        }
        return n;
    }
    static GraphNode result(const QString &instanceId)
    {
        GraphNode n;
        n.kind = Kind::Result;
        n.a = instanceId;
        return n;
    }

    /// Resolution nodes only: the public name this node resolves.
    DependencyKey publicName() const
    {
        return measurementName ? DependencyKey::measurement(a, b) : DependencyKey::attribute(a);
    }
};

inline bool operator==(const GraphNode &lhs, const GraphNode &rhs)
{
    return lhs.kind == rhs.kind && lhs.measurementName == rhs.measurementName
        && lhs.a == rhs.a && lhs.b == rhs.b;
}
inline bool operator!=(const GraphNode &lhs, const GraphNode &rhs) { return !(lhs == rhs); }

inline size_t qHash(const GraphNode &n, size_t seed = 0)
{
    return qHashMulti(seed, static_cast<int>(n.kind), n.measurementName, n.a, n.b);
}

/// Outcome of evaluating one calculation instance. Everything except Ok means
/// "all outputs unavailable"; every status is cached with its dependencies.
enum class ResultStatus {
    Ok,             ///< ran; individual outputs may still be unavailable (partial result)
    MissingInput,   ///< a declared input is unavailable; did not run
    NotRequested,   ///< Explicit policy and nothing has requested it; did not run
    Cycle,          ///< on a dependency cycle; did not run
    Failed,         ///< compute threw
    UndeclaredRead, ///< compute read something it had not declared
    InvalidOutput   ///< compute set an output it had not declared (or of the wrong type)
};

/// True for the "no name" key that selects a plain calculation rather than a
/// family instance. Such a key is built with DependencyKey::attribute(QString()),
/// so all of its strings are empty; never look at `type` alone, which a
/// default-constructed key leaves unset.
inline bool isEmptyName(const DependencyKey &name)
{
    return name.attributeKey.isEmpty()
        && name.measurementKey.first.isEmpty() && name.measurementKey.second.isEmpty();
}

// ---- text forms, for warnings only ----------------------------------------

inline QString describe(const DependencyKey &name)
{
    if (name.type == DependencyKey::Type::Measurement)
        return name.measurementKey.first + QLatin1Char('/') + name.measurementKey.second;
    return name.attributeKey;
}

inline QString describe(const CalcInput &in)
{
    switch (in.kind) {
    case CalcInput::Kind::Attribute:         return QStringLiteral("attribute(%1)").arg(in.key);
    case CalcInput::Kind::Measurement:       return QStringLiteral("measurement(%1/%2)").arg(in.sensor, in.name);
    case CalcInput::Kind::Preference:        return QStringLiteral("preference(%1)").arg(in.key);
    case CalcInput::Kind::SourceMeasurement: return QStringLiteral("sourceMeasurement(%1/%2)").arg(in.sensor, in.name);
    case CalcInput::Kind::SourceUnit:        return QStringLiteral("sourceUnit(%1/%2)").arg(in.sensor, in.name);
    }
    return QString();
}

inline QString describe(const GraphNode &n)
{
    switch (n.kind) {
    case GraphNode::Kind::StoredAttribute:   return QStringLiteral("storedAttribute(%1)").arg(n.a);
    case GraphNode::Kind::SourceMeasurement: return QStringLiteral("sourceMeasurement(%1/%2)").arg(n.a, n.b);
    case GraphNode::Kind::SourceUnit:        return QStringLiteral("sourceUnit(%1/%2)").arg(n.a, n.b);
    case GraphNode::Kind::Preference:        return QStringLiteral("preference(%1)").arg(n.a);
    case GraphNode::Kind::Resolution:
        return n.measurementName ? QStringLiteral("resolution(%1/%2)").arg(n.a, n.b)
                                 : QStringLiteral("resolution(%1)").arg(n.a);
    case GraphNode::Kind::Result:            return QStringLiteral("result(%1)").arg(n.a);
    }
    return QString();
}

} // namespace FlySight

#endif // FLYSIGHT_ENGINE_CALCTYPES_H
