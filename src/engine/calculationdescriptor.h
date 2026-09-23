#ifndef FLYSIGHT_ENGINE_CALCULATIONDESCRIPTOR_H
#define FLYSIGHT_ENGINE_CALCULATIONDESCRIPTOR_H

#include <functional>
#include <optional>

#include <QList>

#include "calctypes.h"
#include "calculationresult.h"
#include "evaluationcontext.h"

namespace FlySight {

/// A calculation: a pure function of its declared inputs, returning a bundle.
/// It must not consult anything else (session, clock, random state, undeclared
/// preferences), must not write anywhere, and may throw; the engine turns an
/// exception into an unavailable result.
using ComputeFunction = std::function<CalculationResult(const EvaluationContext &)>;

/// The unit of registration. Registrations are global and hold no per-session
/// state; the same descriptor serves every session.
struct CalculationDescriptor {
    CalculationId        id;
    /// Human-readable name for the interface ("Sensor fusion"). Interface text
    /// only: opaque to the engine, it never affects evaluation, identity, or
    /// the environment fingerprint. Registrars pass translated text; empty
    /// means none (the id is shown instead).
    QString              title;
    /// All required: the calculation runs only when every input is available.
    /// Order is the order of the availability check, which stops at the first
    /// unavailable input. "Works with or without input I" is expressed as two
    /// registered calculations declaring the same output.
    QList<CalcInput>     inputs;
    QList<DependencyKey> outputs;       ///< at least one; attributes and/or measurements
    EvaluationPolicy     policy = EvaluationPolicy::OnDemand;
    ComputeFunction      compute;
    /// Optional: identifies the arithmetic of the calculation's results, for
    /// results stored in the logbook's cache. A stored result is used only while
    /// the version it was stored with equals this one, so change it whenever a
    /// code change can alter what the calculation produces from the same
    /// inputs. Empty means none is declared. Opaque text to the engine: it
    /// never affects evaluation, identity, candidate order, or the environment
    /// fingerprint. A family's `instantiate` may set it on the descriptors it
    /// returns; the registry keeps what it is given.
    QString              resultVersion;
};

/// One registration that stands for a whole set of calculations, instantiated
/// per parameter set on demand (synthesized interpolation, the conversion
/// layer). Each instance has its own result and its own dependencies.
struct CalculationFamily {
    CalculationId    id;
    EvaluationPolicy policy = EvaluationPolicy::OnDemand;

    /// Pure function of the name. Returns nullopt when `name` is not an output
    /// of this family. Otherwise returns the instance: a descriptor whose
    /// `outputs` contains `name` and whose `id` field holds the *instance key*
    /// (canonical text of the parameters, non-empty, no '#'). The registry
    /// rewrites `id` to "<familyId>#<instanceKey>" and applies the family's
    /// policy.
    ///
    /// Rules:
    ///  - deterministic: the same name always yields the same answer;
    ///  - two names that belong to the same instance must return descriptors
    ///    with the same instance key, inputs, and outputs (instances are
    ///    memoized by instance key, so the first one instantiated is kept);
    ///  - the parameters are captured by the `compute` lambda;
    ///  - must not touch any session; may throw, which counts as "no match".
    ///
    /// Example (interpolation): the attribute name "{t}:{s}/{tv}/{m}" yields
    /// instance key = the name, inputs attribute(t), measurement(s, tv),
    /// measurement(s, m), and that one name as output.
    std::function<std::optional<CalculationDescriptor>(const DependencyKey &name)> instantiate;
};

} // namespace FlySight

#endif // FLYSIGHT_ENGINE_CALCULATIONDESCRIPTOR_H
