#include "calculationengine.h"

#include <algorithm>
#include <cstring>
#include <exception>

#include <QDebug>
#include <QStringList>

namespace FlySight {

namespace {

bool isEmptyName(const DependencyKey &name)
{
    // See CalculationRegistry: a "no name" key has all strings empty.
    return name.attributeKey.isEmpty()
        && name.measurementKey.first.isEmpty() && name.measurementKey.second.isEmpty();
}

} // namespace

// =============================================================================
// Scope stack
// =============================================================================

/// Pushes an evaluation scope and guarantees it is popped again, whatever is
/// thrown. finish() pops early and hands back what the scope recorded, so that
/// publication can be the last step, after the scope is gone.
class CalculationEngine::ScopeGuard {
public:
    ScopeGuard(CalculationEngine *engine, const GraphNode &node)
        : m_engine(engine)
    {
        Scope scope;
        scope.node = node;
        m_engine->m_scopes.push_back(std::move(scope));
        if (m_engine->m_scopes.size() == 1 && m_engine->m_registry) {
            // Lets the registry refuse to change while anything is evaluating.
            m_counted = m_engine->m_registry;
            m_counted->evaluationStarted();
        }
    }

    ~ScopeGuard()
    {
        if (!m_finished)
            pop();
    }

    Scope finish()
    {
        Scope scope = std::move(m_engine->m_scopes.back());
        pop();
        return scope;
    }

private:
    Q_DISABLE_COPY_MOVE(ScopeGuard)

    void pop()
    {
        m_engine->m_scopes.pop_back();
        if (m_counted)
            m_counted->evaluationFinished();
        m_finished = true;
    }

    CalculationEngine *m_engine;
    CalculationRegistry *m_counted = nullptr;
    bool m_finished = false;
};

// =============================================================================
// Construction
// =============================================================================

CalculationEngine::CalculationEngine(const ISessionState *state, CalculationRegistry *registry)
    : m_state(state)
    , m_registry(registry)
    , m_enrolled(false)
    , m_quiet(false)
{
    Q_ASSERT(m_state);
    Q_ASSERT(m_registry);
    if (m_registry) {
        m_registry->enrol(this);
        m_enrolled = true;
    }
}

CalculationEngine::CalculationEngine(const ISessionState *state, CalculationRegistry *registry, Detached)
    : m_state(state)
    , m_registry(registry)
    , m_enrolled(false)
    , m_quiet(true)
{
}

CalculationEngine::~CalculationEngine()
{
    if (m_enrolled && m_registry)
        m_registry->withdraw(this);
}

void CalculationEngine::rebind(const ISessionState *state)
{
    Q_ASSERT(state);
    m_state = state;
}

void CalculationEngine::registryDestroyed()
{
    m_registry = nullptr;
    m_enrolled = false;
    m_resolutions.clear();
    m_results.clear();
    m_dependsOn.clear();
    m_dependents.clear();
}

// =============================================================================
// Ordinary reads
// =============================================================================

CalculationEngine::ResolutionEntry CalculationEngine::readTopLevel(const DependencyKey &name)
{
    if (!m_scopes.empty()) {
        // A compute function (or the state object) called back into the engine.
        // Calculations read through their EvaluationContext only.
        Q_ASSERT_X(false, "CalculationEngine", "engine read from inside an evaluation");
        if (!m_quiet)
            qWarning().noquote() << "CalculationEngine: read of" << describe(name)
                                 << "from inside an evaluation - reported unavailable";
        return ResolutionEntry();
    }
    flushPending();
    const ResolutionEntry entry = resolve(name);
    flushPending();
    return entry;
}

QVariant CalculationEngine::attribute(const QString &key)
{
    const ResolutionEntry entry = readTopLevel(DependencyKey::attribute(key));
    return entry.available ? entry.attribute : QVariant();
}

QVector<double> CalculationEngine::measurement(const QString &sensor, const QString &name)
{
    const ResolutionEntry entry = readTopLevel(DependencyKey::measurement(sensor, name));
    return entry.available ? entry.samples : QVector<double>();
}

QString CalculationEngine::measurementUnit(const QString &sensor, const QString &name)
{
    const ResolutionEntry entry = readTopLevel(DependencyKey::measurement(sensor, name));
    return entry.available ? entry.unit : QString();
}

bool CalculationEngine::isAvailable(const DependencyKey &name)
{
    return readTopLevel(name).available;
}

// =============================================================================
// Resolution
// =============================================================================

void CalculationEngine::note(const GraphNode &node)
{
    if (!m_scopes.empty())
        m_scopes.back().looked.insert(node);
}

int CalculationEngine::stackIndexOf(const GraphNode &node) const
{
    for (size_t i = 0; i < m_scopes.size(); ++i) {
        if (m_scopes[i].node == node)
            return int(i);
    }
    return -1;
}

void CalculationEngine::reportCycle(int stackIndex, const GraphNode &reentered)
{
    // Every calculation on the ring is unavailable, whichever node the ring was
    // entered through; that is what keeps the answer independent of read order.
    // Resolution scopes are not marked: they carry on down their candidate list.
    QList<GraphNode> path;
    for (size_t i = size_t(stackIndex); i < m_scopes.size(); ++i) {
        if (m_scopes[i].node.kind == GraphNode::Kind::Result)
            m_scopes[i].cycle = true;
        path.append(m_scopes[i].node);
    }
    path.append(reentered);

    ++m_cycleCount;
    m_lastCyclePath = path;

    if (!m_quiet) {
        QStringList text;
        for (const GraphNode &n : path)
            text.append(describe(n));
        qWarning().noquote() << "CalculationEngine: dependency cycle (registration error):"
                             << text.join(QStringLiteral(" -> "));
    }
}

CalculationEngine::ResolutionEntry CalculationEngine::resolve(const DependencyKey &name)
{
    const GraphNode R = GraphNode::resolution(name);
    note(R);

    const auto cached = m_resolutions.constFind(R);
    if (cached != m_resolutions.constEnd())
        return cached.value();

    const int onStack = stackIndexOf(R);
    if (onStack >= 0) {
        reportCycle(onStack, R);
        return ResolutionEntry();   // unavailable; nothing is cached for R here
    }

    ScopeGuard guard(this, R);
    ResolutionEntry entry;

    if (name.type == DependencyKey::Type::Attribute) {
        // Record before branching: the *absence* of a stored value is a dependency too.
        note(GraphNode::storedAttribute(name.attributeKey));
        if (m_state && m_state->hasStoredAttribute(name.attributeKey)) {
            // A stored attribute always wins, even when its value is invalid.
            entry.provider = Provider::Stored;
            entry.attribute = m_state->storedAttribute(name.attributeKey);
            entry.available = entry.attribute.isValid();
        } else if (m_registry) {
            tryCandidates(m_registry->candidatesFor(name), name, entry);
        }
    } else {
        const QString &sensor = name.measurementKey.first;
        const QString &meas = name.measurementKey.second;
        note(GraphNode::sourceMeasurement(sensor, meas));
        if (m_state && m_state->hasSourceMeasurement(sensor, meas)) {
            if (m_registry && m_registry->hasSourceConversions()) {
                // The effective value is the conversion layer's output. It never
                // falls through to derived candidates (spec 7.3 rule 1).
                tryCandidates(m_registry->sourceConversionsFor(sensor, meas), name, entry);
            } else {
                note(GraphNode::sourceUnit(sensor, meas));
                entry.provider = Provider::Source;
                entry.samples = m_state->sourceMeasurement(sensor, meas);
                entry.unit = m_state->sourceUnit(sensor, meas);
                entry.available = !entry.samples.isEmpty();
            }
        } else if (m_registry) {
            tryCandidates(m_registry->candidatesFor(name), name, entry);
        }
    }

    if (!entry.available) {
        // Normalize, so an unavailable entry never carries a value.
        const Provider provider = entry.provider;
        entry = ResolutionEntry();
        entry.provider = provider == Provider::Calculation ? Provider::None : provider;
    }

    // Publish last: the scope is gone, then entry and edges go in together.
    const Scope scope = guard.finish();
    m_resolutions.insert(R, entry);
    setEdges(R, scope.looked);
    return entry;
}

bool CalculationEngine::tryCandidates(const QList<CalculationInstance> &candidates,
                                      const DependencyKey &name, ResolutionEntry &entry)
{
    for (const CalculationInstance &candidate : candidates) {
        // ensureResult notes Result(candidate) in the open Resolution scope, so
        // candidates that are rejected here are dependencies of the name too.
        const ResultEntry result = ensureResult(candidate);
        if (result.status != ResultStatus::Ok || !result.bundle || !result.bundle->isAvailable(name))
            continue;   // did not run, or ran and reported this output unavailable

        entry.provider = Provider::Calculation;
        entry.instanceId = candidate.instanceId;
        entry.available = true;
        if (name.type == DependencyKey::Type::Attribute) {
            entry.attribute = result.bundle->attributeValue(name.attributeKey);
        } else {
            entry.samples = result.bundle->measurementValues(name.measurementKey.first,
                                                             name.measurementKey.second);
            entry.unit = result.bundle->measurementUnit(name.measurementKey.first,
                                                        name.measurementKey.second);
        }
        return true;
    }
    return false;
}

CalculationEngine::ResultEntry CalculationEngine::ensureResult(const CalculationInstance &instance)
{
    const GraphNode C = GraphNode::result(instance.instanceId);
    note(C);

    const auto cached = m_results.constFind(C);
    if (cached != m_results.constEnd())
        return cached.value();

    const int onStack = stackIndexOf(C);
    if (onStack >= 0) {
        reportCycle(onStack, C);
        ResultEntry unavailable;
        unavailable.status = ResultStatus::Cycle;
        return unavailable;
    }

    QSet<GraphNode> looked;
    const ResultEntry entry = computeResult(instance, /*fromRequest=*/false, looked);
    m_results.insert(C, entry);
    setEdges(C, looked);
    return entry;
}

CalculationEngine::ResultEntry CalculationEngine::computeResult(const CalculationInstance &instance,
                                                                bool fromRequest,
                                                                QSet<GraphNode> &looked)
{
    const CalculationDescriptor &d = *instance.descriptor;

    ResultEntry entry;
    entry.instance = instance;
    entry.requested = fromRequest;

    ScopeGuard guard(this, GraphNode::result(instance.instanceId));

    if (d.policy == EvaluationPolicy::Explicit && !fromRequest) {
        // Reads never start an explicit calculation. No input was looked at.
        entry.status = ResultStatus::NotRequested;
        looked = guard.finish().looked;
        return entry;
    }

    // ---- availability pass: declared order, stop at the first unavailable input.
    // Stopping early is sound: while that input stays unavailable the answer
    // cannot change, and the input has been recorded.
    EvaluationContext ctx(instance.instanceId, m_quiet);
    ResultStatus status = ResultStatus::Ok;

    for (const CalcInput &input : d.inputs) {
        EvaluationContext::InputValue value;
        bool available = false;

        switch (input.kind) {
        case CalcInput::Kind::Attribute: {
            const ResolutionEntry r = resolve(DependencyKey::attribute(input.key));   // may nest
            available = r.available;
            value.value = r.attribute;
            break;
        }
        case CalcInput::Kind::Measurement: {
            const ResolutionEntry r = resolve(DependencyKey::measurement(input.sensor, input.name));
            available = r.available;
            value.samples = r.samples;
            value.unit = r.unit;
            break;
        }
        case CalcInput::Kind::Preference: {
            note(GraphNode::preference(input.key));
            const IPreferenceProvider *provider = m_registry ? m_registry->preferenceProvider() : nullptr;
            if (provider)
                value.value = provider->preferenceValue(input.key);
            available = value.value.isValid();
            break;
        }
        case CalcInput::Kind::SourceMeasurement: {
            note(GraphNode::sourceMeasurement(input.sensor, input.name));
            if (m_state && m_state->hasSourceMeasurement(input.sensor, input.name))
                value.samples = m_state->sourceMeasurement(input.sensor, input.name);
            available = !value.samples.isEmpty();
            break;
        }
        case CalcInput::Kind::SourceUnit: {
            // The unit exists exactly when the measurement does, so both leaves matter.
            note(GraphNode::sourceMeasurement(input.sensor, input.name));
            note(GraphNode::sourceUnit(input.sensor, input.name));
            if (m_state && m_state->hasSourceMeasurement(input.sensor, input.name)) {
                value.unit = m_state->sourceUnit(input.sensor, input.name);
                available = true;   // empty unit text is a value
            }
            break;
        }
        }

        if (m_scopes.back().cycle) {
            status = ResultStatus::Cycle;
            break;
        }
        if (!available) {
            status = ResultStatus::MissingInput;
            break;
        }
        ctx.provide(input, value);
    }

    // ---- run
    if (status == ResultStatus::Ok) {
        // Counted before the call, so a throwing calculation counts as a run.
        ++m_totalRuns;
        ++m_runsByInstance[instance.instanceId];
        ++m_runsByRegistration[instance.registrationId];

        CalculationResult bundle;
        try {
            bundle = d.compute(ctx);
        } catch (const std::exception &ex) {
            status = ResultStatus::Failed;
            if (!m_quiet)
                qWarning().noquote() << "Calculation" << instance.instanceId << "failed:" << ex.what();
        } catch (...) {
            status = ResultStatus::Failed;
            if (!m_quiet)
                qWarning().noquote() << "Calculation" << instance.instanceId
                                     << "failed with a non-standard exception";
        }

        if (!ctx.m_undeclaredReads.isEmpty()) {
            m_undeclaredReadCount += int(ctx.m_undeclaredReads.size());
            m_lastUndeclaredRead = std::make_pair(instance.instanceId, ctx.m_undeclaredReads.last());
            if (status == ResultStatus::Ok)
                status = ResultStatus::UndeclaredRead;
        }

        if (status == ResultStatus::Ok) {
            // A key of the wrong type (setMeasurement on an attribute output or
            // vice versa) is simply not among the declared outputs.
            for (const DependencyKey &out : bundle.setOutputs()) {
                if (!d.outputs.contains(out)) {
                    status = ResultStatus::InvalidOutput;
                    if (!m_quiet)
                        qWarning().noquote() << "Calculation" << instance.instanceId
                                             << "set an output it did not declare:" << describe(out)
                                             << "- its result is discarded";
                    break;
                }
            }
        }

        // Only a clean run publishes a bundle: never a partial result of a failure.
        if (status == ResultStatus::Ok)
            entry.bundle = std::make_shared<const CalculationResult>(std::move(bundle));
    }

    entry.status = status;
    looked = guard.finish().looked;
    return entry;
}

// =============================================================================
// Edges, publication, invalidation
// =============================================================================

void CalculationEngine::dropForwardEdges(const GraphNode &node)
{
    const auto it = m_dependsOn.constFind(node);
    if (it == m_dependsOn.constEnd())
        return;
    const QSet<GraphNode> old = it.value();
    m_dependsOn.erase(it);
    for (const GraphNode &target : old) {
        const auto rev = m_dependents.find(target);
        if (rev == m_dependents.end())
            continue;
        rev->remove(node);
        if (rev->isEmpty())
            m_dependents.erase(rev);
    }
}

void CalculationEngine::setEdges(const GraphNode &node, const QSet<GraphNode> &looked)
{
    dropForwardEdges(node);     // detach the old reverse edges first
    if (looked.isEmpty())
        return;
    m_dependsOn.insert(node, looked);
    for (const GraphNode &target : looked)
        m_dependents[target].insert(node);
}

QSet<DependencyKey> CalculationEngine::invalidate(const QList<GraphNode> &seeds)
{
    // Breadth-first over reverse edges. Touches only the cache and the edge
    // maps: never resolve / ensureResult / compute, never the state, never the
    // preference provider. Invalidation never triggers computation.
    QSet<DependencyKey> names;
    QSet<GraphNode> visited;
    QList<GraphNode> queue = seeds;

    while (!queue.isEmpty()) {
        const GraphNode n = queue.takeFirst();
        if (visited.contains(n))
            continue;
        visited.insert(n);

        if (n.kind == GraphNode::Kind::Resolution) {
            // Only names that had actually been resolved are reported.
            if (m_resolutions.remove(n))
                names.insert(n.publicName());
        } else if (n.kind == GraphNode::Kind::Result) {
            m_results.remove(n);
        }

        // Propagation does not depend on whether n itself was cached.
        const QSet<GraphNode> dependents = m_dependents.value(n);
        dropForwardEdges(n);
        for (const GraphNode &dependent : dependents) {
            if (!visited.contains(dependent))
                queue.append(dependent);
        }
    }
    return names;
}

QSet<DependencyKey> CalculationEngine::notifyLeafChanged(const GraphNode &leaf,
                                                         const DependencyKey &changedName)
{
    // The changed name is always reported, even if nothing was cached; the
    // model relies on that to refresh the edited cell.
    QSet<DependencyKey> names;
    names.insert(changedName);

    if (!m_scopes.empty()) {
        // Never invalidate in the middle of an evaluation.
        Q_ASSERT_X(false, "CalculationEngine", "state change notified during an evaluation");
        m_pendingSeeds.append(leaf);
        return names;
    }
    names.unite(invalidate({leaf}));
    return names;
}

QSet<DependencyKey> CalculationEngine::attributeChanged(const QString &key)
{
    return notifyLeafChanged(GraphNode::storedAttribute(key), DependencyKey::attribute(key));
}

QSet<DependencyKey> CalculationEngine::sourceMeasurementChanged(const QString &sensor, const QString &name)
{
    return notifyLeafChanged(GraphNode::sourceMeasurement(sensor, name),
                             DependencyKey::measurement(sensor, name));
}

QSet<DependencyKey> CalculationEngine::sourceUnitChanged(const QString &sensor, const QString &name)
{
    return notifyLeafChanged(GraphNode::sourceUnit(sensor, name),
                             DependencyKey::measurement(sensor, name));
}

QSet<DependencyKey> CalculationEngine::clear()
{
    if (!m_scopes.empty()) {
        Q_ASSERT_X(false, "CalculationEngine", "clear() during an evaluation");
        m_pendingClear = true;
        return {};
    }
    QSet<DependencyKey> names;
    for (auto it = m_resolutions.constBegin(); it != m_resolutions.constEnd(); ++it)
        names.insert(it.key().publicName());
    m_resolutions.clear();
    m_results.clear();
    m_dependsOn.clear();
    m_dependents.clear();
    return names;
}

void CalculationEngine::setInvalidationListener(InvalidationListener l)
{
    m_listener = std::move(l);
}

void CalculationEngine::deliverBroadcast(const QList<GraphNode> &seeds)
{
    if (seeds.isEmpty())
        return;
    if (!m_scopes.empty()) {
        Q_ASSERT_X(false, "CalculationEngine", "broadcast invalidation during an evaluation");
        m_pendingSeeds.append(seeds);
        return;
    }
    const QSet<DependencyKey> names = invalidate(seeds);
    if (!names.isEmpty() && m_listener)
        m_listener(names);
}

void CalculationEngine::flushPending()
{
    if (!m_scopes.empty() || (!m_pendingClear && m_pendingSeeds.isEmpty()))
        return;

    QSet<DependencyKey> names;
    if (m_pendingClear) {
        m_pendingClear = false;
        names = clear();
    }
    const QList<GraphNode> seeds = m_pendingSeeds;
    m_pendingSeeds.clear();
    names.unite(invalidate(seeds));
    if (!names.isEmpty() && m_listener)
        m_listener(names);
}

void CalculationEngine::onPreferenceChanged(const QString &key)
{
    // A preference no calculation declared has no dependents: nothing is
    // invalidated and the listener is not called.
    deliverBroadcast({GraphNode::preference(key)});
}

void CalculationEngine::onRegistryChanged(const RegistryChange &change)
{
    QList<GraphNode> seeds;

    if (!change.added) {
        // No cache entry may outlive its registration.
        for (auto it = m_results.constBegin(); it != m_results.constEnd(); ++it) {
            if (it->instance.registrationId == change.registrationId)
                seeds.append(it.key());
        }
    }

    switch (change.kind) {
    case RegistryChange::Kind::Calculation:
        // Added: a cached fallback or a cached "none" must re-resolve so it can
        // pick up the new candidate. Removed: likewise for whatever it provided.
        for (const DependencyKey &out : change.outputs)
            seeds.append(GraphNode::resolution(out));
        break;

    case RegistryChange::Kind::Family:
        for (auto it = m_resolutions.constBegin(); it != m_resolutions.constEnd(); ++it) {
            bool accepts = false;
            try {
                accepts = change.instantiate && change.instantiate(it.key().publicName()).has_value();
            } catch (...) {
                accepts = false;    // a throwing family never matches
            }
            if (accepts)
                seeds.append(it.key());
        }
        break;

    case RegistryChange::Kind::SourceConversion:
        // Whether *any* conversion is registered decides between passthrough
        // and the conversion layer for every measurement with source data, so
        // every cached measurement name re-resolves, not only the names this
        // family accepts.
        for (auto it = m_resolutions.constBegin(); it != m_resolutions.constEnd(); ++it) {
            if (it.key().measurementName)
                seeds.append(it.key());
        }
        break;
    }

    deliverBroadcast(seeds);
}

// =============================================================================
// Explicit evaluation
// =============================================================================

CalculationEngine::RequestOutcome CalculationEngine::request(const CalculationId &id,
                                                             const DependencyKey &instanceOutput)
{
    RequestOutcome outcome;
    if (!m_scopes.empty()) {
        Q_ASSERT_X(false, "CalculationEngine", "request() from inside an evaluation");
        return outcome;
    }
    flushPending();
    if (!m_registry)
        return outcome;

    const std::optional<CalculationInstance> instance = m_registry->instance(id, instanceOutput);
    if (!instance)
        return outcome;     // unknown id / name not of this family: nothing changes

    outcome = requestInstance(*instance);
    flushPending();
    return outcome;
}

CalculationEngine::RequestOutcome CalculationEngine::requestInstance(const CalculationInstance &instance,
                                                                     bool *evaluated)
{
    RequestOutcome outcome;
    outcome.found = true;
    if (evaluated)
        *evaluated = false;

    const GraphNode C = GraphNode::result(instance.instanceId);
    const auto cached = m_results.constFind(C);
    if (cached != m_results.constEnd() && cached->status != ResultStatus::NotRequested) {
        outcome.status = cached->status;    // a valid result is never recomputed
        return outcome;
    }

    QSet<GraphNode> looked;
    const ResultEntry entry = computeResult(instance, /*fromRequest=*/true, looked);
    if (evaluated)
        *evaluated = true;

    // Names that were read while the calculation was "not requested" cached that
    // answer; drop them (not the result node) so all outputs appear at once.
    outcome.invalidated = invalidate(m_dependents.value(C).values());

    m_results.insert(C, entry);
    setEdges(C, looked);
    outcome.status = entry.status;
    return outcome;
}

// =============================================================================
// Inspection and instrumentation
// =============================================================================

CalculationEngine::CachedState CalculationEngine::cachedState(const DependencyKey &name) const
{
    const auto it = m_resolutions.constFind(GraphNode::resolution(name));
    if (it == m_resolutions.constEnd())
        return CachedState::NotCached;
    return it->available ? CachedState::Available : CachedState::Unavailable;
}

std::optional<ResultStatus> CalculationEngine::resultStatus(const CalculationId &id,
                                                            const DependencyKey &instanceOutput) const
{
    QString instanceId = id;
    if (!isEmptyName(instanceOutput)) {
        // Instantiating is a pure function of the name; it touches no session.
        const std::optional<CalculationInstance> instance =
            m_registry ? m_registry->instance(id, instanceOutput) : std::nullopt;
        if (!instance)
            return std::nullopt;
        instanceId = instance->instanceId;
    }
    const auto it = m_results.constFind(GraphNode::result(instanceId));
    if (it == m_results.constEnd())
        return std::nullopt;
    return it->status;
}

int CalculationEngine::runCount(const CalculationId &registrationId) const
{
    return m_runsByRegistration.value(registrationId, 0);
}

int CalculationEngine::runCountForInstance(const QString &instanceId) const
{
    return m_runsByInstance.value(instanceId, 0);
}

int CalculationEngine::totalRunCount() const { return m_totalRuns; }

void CalculationEngine::resetRunCounts()
{
    m_runsByInstance.clear();
    m_runsByRegistration.clear();
    m_totalRuns = 0;
}

int CalculationEngine::cycleCount() const { return m_cycleCount; }
QList<GraphNode> CalculationEngine::lastCyclePath() const { return m_lastCyclePath; }
int CalculationEngine::undeclaredReadCount() const { return m_undeclaredReadCount; }
std::pair<QString, CalcInput> CalculationEngine::lastUndeclaredRead() const { return m_lastUndeclaredRead; }
int CalculationEngine::scopeDepth() const { return int(m_scopes.size()); }

int CalculationEngine::edgeCount() const
{
    int count = 0;
    for (auto it = m_dependsOn.constBegin(); it != m_dependsOn.constEnd(); ++it)
        count += int(it->size());
    return count;
}

int CalculationEngine::cachedNodeCount() const
{
    return int(m_resolutions.size() + m_results.size());
}

QSet<GraphNode> CalculationEngine::dependenciesOf(const GraphNode &n) const
{
    return m_dependsOn.value(n);
}

// =============================================================================
// Oracle
// =============================================================================

CalculationEngine::Value CalculationEngine::toValue(const ResolutionEntry &entry)
{
    Value v;
    v.available = entry.available;
    if (entry.available) {
        v.attribute = entry.attribute;
        v.samples = entry.samples;
        v.unit = entry.unit;
    }
    return v;
}

CalculationEngine::Value CalculationEngine::evaluateFresh(const DependencyKey &name) const
{
    Q_ASSERT_X(m_scopes.empty(), "CalculationEngine", "evaluateFresh() from inside an evaluation");

    // Same state, same registry, empty cache; not enrolled, so the registry's
    // engine list is untouched and no broadcast can reach it.
    CalculationEngine fresh(m_state, m_registry, Detached::Tag);

    // Which explicit calculations have been requested is the one piece of cache
    // state that cannot be derived from persistent state, so it is replayed.
    // Sorted for a deterministic order; repeated until a pass evaluates nothing,
    // because requesting one calculation can drop another that read its output
    // while it was still "not requested".
    QList<CalculationInstance> requested;
    for (auto it = m_results.constBegin(); it != m_results.constEnd(); ++it) {
        if (it->requested)
            requested.append(it->instance);
    }
    std::sort(requested.begin(), requested.end(),
              [](const CalculationInstance &a, const CalculationInstance &b) {
                  return a.instanceId < b.instanceId;
              });
    for (qsizetype pass = 0; pass <= requested.size(); ++pass) {
        bool any = false;
        for (const CalculationInstance &instance : requested) {
            bool evaluated = false;
            fresh.requestInstance(instance, &evaluated);
            any = any || evaluated;
        }
        if (!any)
            break;
    }

    return toValue(fresh.readTopLevel(name));
}

bool CalculationEngine::sameValue(const Value &a, const Value &b)
{
    if (a.available != b.available)
        return false;
    if (a.attribute != b.attribute)
        return false;
    if (a.unit != b.unit)
        return false;
    if (a.samples.size() != b.samples.size())
        return false;
    // Bit pattern, not ==: NaN equals NaN, and -0.0 differs from 0.0.
    return a.samples.isEmpty()
        || std::memcmp(a.samples.constData(), b.samples.constData(),
                       size_t(a.samples.size()) * sizeof(double)) == 0;
}

QList<DependencyKey> CalculationEngine::verifyAgainstFresh(const QList<DependencyKey> &names)
{
    QList<DependencyKey> mismatches;
    for (const DependencyKey &name : names) {
        const Value cached = toValue(readTopLevel(name));
        if (!sameValue(cached, evaluateFresh(name)))
            mismatches.append(name);
    }
    return mismatches;
}

} // namespace FlySight
