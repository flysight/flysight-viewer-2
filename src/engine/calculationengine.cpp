#include "calculationengine.h"

#include <algorithm>
#include <cstring>
#include <exception>

#include <QDebug>
#include <QStringList>

namespace FlySight {

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
        if (!m_engine->m_scopes.empty()) {
            // What a subtree met, its parent's subtree met. (A scope that is
            // unwound by an exception reports nothing: nothing above it is
            // published either.)
            Scope &parent = m_engine->m_scopes.back();
            parent.minHit = std::min(parent.minHit, scope.minHit);
            parent.sawCycle = parent.sawCycle || scope.sawCycle;
        }
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
    clearCaches();
}

void CalculationEngine::clearCaches()
{
    m_resolutions.clear();
    m_results.clear();
    m_dependsOn.clear();
    m_dependents.clear();
    m_provisional.clear();
    m_provisionalOwned.clear();
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
    // Every calculation on the ring - between the two occurrences of the
    // re-entered node - is unavailable. Resolution scopes are not marked: they
    // carry on down their candidate list.
    //
    // Which calculations are on the stack when a ring closes depends on where
    // the evaluation started, so by itself this verdict is NOT independent of
    // read order (with overlapping rings not even the names' values are). What
    // makes the answers independent is that the verdict never reaches the cache
    // from a context it depends on: the detecting scope remembers how far up
    // the stack the ring reached (minHit), that propagates to every scope the
    // result flows through, and a scope below the ring's top frame is
    // provisional - see resolve() and ensureResult(). The top frame itself is
    // not tainted by its own ring: evaluated on its own it closes the same ring
    // the same way.
    Scope &detecting = m_scopes.back();
    detecting.minHit = std::min(detecting.minHit, stackIndex);
    detecting.sawCycle = true;

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

bool CalculationEngine::isContextFree(const Scope &closed) const
{
    // Called after the scope was popped, so m_scopes.size() is the stack index
    // it had. Context-free: no cycle detected beneath it re-entered a frame
    // ABOVE it. Its evaluation then never consulted the part of the stack it
    // does not own, so it went exactly as an evaluation started at this node
    // goes, and the result is a function of state alone. The root (index 0)
    // always qualifies.
    return closed.minHit >= int(m_scopes.size());
}

void CalculationEngine::handUp(const Scope &closed)
{
    // A provisional result is not cached and records no edges of its own; the
    // scope that consumed it inherits everything it looked at. By induction
    // the nearest cached ancestor depends on all of it, so a change to any of
    // it still invalidates every cached answer it helped to shape. (There is
    // a parent: a scope with nothing above it is never provisional.)
    Scope &parent = m_scopes.back();
    parent.looked.unite(closed.looked);
    for (auto it = closed.provisional.constBegin(); it != closed.provisional.constEnd(); ++it)
        parent.provisional.insert(it.key(), it.value());
}

bool CalculationEngine::cachedAnswerUsable(const GraphNode &node, bool sawCycle)
{
    // A cached answer is the node's value when evaluated on its own. Here it
    // stands in for an evaluation beneath the current stack, which goes the
    // same way unless it looks up a node that is on the stack - then it would
    // close a ring that the stand-alone evaluation did not see (or saw from
    // the other side). So the answer is usable iff nothing it transitively
    // looked at is being evaluated now.
    //
    // Without the flag there is nothing to check, and a graph without cycles
    // never gets further than this line. Everything a cycle-free evaluation
    // visited was cached with it and is invalidated with it, and a node that is
    // both cached and on the stack was turned away here, so it carries the
    // flag - which a cycle-free evaluation cannot have visited.
    if (!sawCycle || m_scopes.empty())
        return true;

    QSet<GraphNode> onStack;
    for (const Scope &scope : m_scopes)
        onStack.insert(scope.node);

    // The edges are complete for this purpose: a cached node's edges include
    // what its provisional descendants looked at. They can over-approximate;
    // turning an answer away costs a re-evaluation, never correctness.
    QSet<GraphNode> visited;
    QList<GraphNode> queue = {node};
    while (!queue.isEmpty()) {
        const GraphNode n = queue.takeLast();
        if (visited.contains(n))
            continue;
        visited.insert(n);
        if (onStack.contains(n))
            return false;
        const auto edges = m_dependsOn.constFind(n);
        if (edges == m_dependsOn.constEnd())
            continue;
        for (const GraphNode &target : edges.value()) {
            if (target.kind == GraphNode::Kind::Resolution || target.kind == GraphNode::Kind::Result)
                queue.append(target);
        }
    }
    m_scopes.back().sawCycle = true;    // whoever uses it inherits the check
    return true;
}

// Cost. Work is repeated only inside cyclic regions: a provisional result is
// recomputed by each evaluation that reaches it, and a calculation on a ring
// may run more than once per read. Every evaluation in progress is a simple
// path through the strongly connected region it is in, so the repetition is
// bounded by the number of such paths - exponential in the size of a densely
// tangled region in the worst case, but a cycle is a registration error that
// is reported on every detection, regions are a handful of calculations, and
// everything acyclic hanging off a region is context-free and cached once.

CalculationEngine::ResolutionEntry CalculationEngine::resolve(const DependencyKey &name)
{
    const GraphNode R = GraphNode::resolution(name);
    note(R);

    const auto cached = m_resolutions.constFind(R);
    if (cached != m_resolutions.constEnd() && cachedAnswerUsable(R, cached->sawCycle))
        return cached.value();
    // A cached answer that was turned away stays cached, and the evaluation
    // below never replaces it. Usually that evaluation meets the stack and is
    // provisional. It need not: the walk in cachedAnswerUsable() can
    // over-approximate, and then the evaluation is context-free and merely
    // reproduces the cached answer. What it looked at is dropped in that case,
    // which is sound because the edges of the retained entry already cover it.
    const bool turnedAway = cached != m_resolutions.constEnd();

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
                // The effective value is the conversion layer's output. A name
                // with recorded source data never falls through to derived
                // candidates, even when its conversion is unavailable.
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

    // Publish last: the scope is gone, then entry and edges go in together -
    // if the answer is context-free. A provisional one goes to the caller only.
    const Scope scope = guard.finish();
    entry.sawCycle = scope.sawCycle;
    if (!isContextFree(scope)) {
        handUp(scope);
    } else if (!turnedAway) {
        m_resolutions.insert(R, entry);
        publishEdges(R, scope);
    }
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
    if (cached != m_results.constEnd() && cachedAnswerUsable(C, cached->sawCycle))
        return cached.value();
    // Turned away (see resolve()). Whether an explicit calculation has been
    // requested is not derivable from state, so it carries over.
    const bool turnedAway = cached != m_results.constEnd();
    const bool requested = turnedAway && cached->requested;

    const int onStack = stackIndexOf(C);
    if (onStack >= 0) {
        reportCycle(onStack, C);
        ResultEntry unavailable;
        unavailable.status = ResultStatus::Cycle;
        return unavailable;
    }

    // The same rule as for names, and it settles when a Cycle verdict may be
    // cached: only when it is context-free, i.e. when this calculation is the
    // top frame of every ring that was closed beneath it (as it always is for
    // request(), whose scope is the root). A calculation that was
    // marked because a ring passed THROUGH it to a frame above is provisional,
    // like everything else computed under that ring.
    Scope scope;
    const ResultEntry entry = computeResult(instance, requested, scope);
    if (!isContextFree(scope)) {
        scope.provisional.insert(C, entry.status);
        handUp(scope);
    } else if (!turnedAway) {
        m_results.insert(C, entry);
        publishEdges(C, scope);
    }
    return entry;
}

CalculationEngine::ResultEntry CalculationEngine::computeResult(const CalculationInstance &instance,
                                                                bool fromRequest,
                                                                Scope &closed)
{
    const CalculationDescriptor &d = *instance.descriptor;

    ResultEntry entry;
    entry.instance = instance;
    entry.requested = fromRequest;

    ScopeGuard guard(this, GraphNode::result(instance.instanceId));

    if (d.policy == EvaluationPolicy::Explicit && !fromRequest) {
        // Reads never start an explicit calculation. No input was looked at.
        entry.status = ResultStatus::NotRequested;
        closed = guard.finish();
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
    closed = guard.finish();
    entry.sawCycle = closed.sawCycle;
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

void CalculationEngine::publishEdges(const GraphNode &node, const Scope &closed)
{
    // A ring closed beneath the node makes it look at itself; an edge to
    // itself says nothing.
    // (Checked first: QSet::remove detaches - a deep copy - even when the key
    // is absent, and without a ring it always is.)
    if (closed.looked.contains(node)) {
        QSet<GraphNode> looked = closed.looked;
        looked.remove(node);
        setEdges(node, looked);
    } else {
        setEdges(node, closed.looked);
    }

    // Inspection only: the provisional calculations this answer absorbed. A
    // calculation that has a cached result of its own reports that instead.
    dropProvisionalOwnedBy(node);
    if (node.kind == GraphNode::Kind::Result) {
        const auto stale = m_provisional.constFind(node);
        if (stale != m_provisional.constEnd()) {
            const QSet<GraphNode> owners = stale->owners;
            m_provisional.erase(stale);
            for (const GraphNode &owner : owners) {
                const auto owned = m_provisionalOwned.find(owner);
                if (owned != m_provisionalOwned.end()) {
                    owned->remove(node);
                    if (owned->isEmpty())
                        m_provisionalOwned.erase(owned);
                }
            }
        }
    }
    for (auto it = closed.provisional.constBegin(); it != closed.provisional.constEnd(); ++it) {
        if (m_results.contains(it.key()))
            continue;
        ProvisionalStatus &p = m_provisional[it.key()];
        if (p.status != it.value()) {
            // The most recent verdict replaces a different earlier one.
            for (const GraphNode &owner : std::as_const(p.owners)) {
                const auto owned = m_provisionalOwned.find(owner);
                if (owned != m_provisionalOwned.end()) {
                    owned->remove(it.key());
                    if (owned->isEmpty())
                        m_provisionalOwned.erase(owned);
                }
            }
            p.owners.clear();
            p.status = it.value();
        }
        p.owners.insert(node);
        m_provisionalOwned[node].insert(it.key());
    }
}

void CalculationEngine::dropProvisionalOwnedBy(const GraphNode &owner)
{
    const auto owned = m_provisionalOwned.constFind(owner);
    if (owned == m_provisionalOwned.constEnd())
        return;
    const QSet<GraphNode> nodes = owned.value();
    m_provisionalOwned.erase(owned);
    for (const GraphNode &n : nodes) {
        const auto p = m_provisional.find(n);
        if (p == m_provisional.end())
            continue;
        p->owners.remove(owner);
        if (p->owners.isEmpty())
            m_provisional.erase(p);
    }
}

QSet<GraphNode> CalculationEngine::knownNodes(GraphNode::Kind kind) const
{
    // Cached nodes, plus nodes that are not cached themselves but that a cached
    // answer depends on: a provisional resolution or result lives on only as an
    // edge of the answer that absorbed it.
    QSet<GraphNode> nodes;
    if (kind == GraphNode::Kind::Resolution) {
        for (auto it = m_resolutions.constBegin(); it != m_resolutions.constEnd(); ++it)
            nodes.insert(it.key());
    } else if (kind == GraphNode::Kind::Result) {
        for (auto it = m_results.constBegin(); it != m_results.constEnd(); ++it)
            nodes.insert(it.key());
    }
    for (auto it = m_dependents.constBegin(); it != m_dependents.constEnd(); ++it) {
        if (it.key().kind == kind)
            nodes.insert(it.key());
    }
    return nodes;
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
        dropProvisionalOwnedBy(n);

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
    clearCaches();
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
        // No cache entry may outlive its registration, and neither may an
        // answer that absorbed a provisional result of it. An instance id is
        // the registration id, or "<registration id>#<instance key>".
        const QString familyPrefix = change.registrationId + QLatin1Char('#');
        const QSet<GraphNode> results = knownNodes(GraphNode::Kind::Result);
        for (const GraphNode &n : results) {
            if (n.a == change.registrationId || n.a.startsWith(familyPrefix))
                seeds.append(n);
        }
    }

    switch (change.kind) {
    case RegistryChange::Kind::Calculation:
        // Added: a cached fallback or a cached "none" must re-resolve so it can
        // pick up the new candidate. Removed: likewise for whatever it provided.
        for (const DependencyKey &out : change.outputs)
            seeds.append(GraphNode::resolution(out));
        break;

    case RegistryChange::Kind::Family: {
        const QSet<GraphNode> resolutions = knownNodes(GraphNode::Kind::Resolution);
        for (const GraphNode &n : resolutions) {
            bool accepts = false;
            try {
                accepts = change.instantiate && change.instantiate(n.publicName()).has_value();
            } catch (...) {
                accepts = false;    // a throwing family never matches
            }
            if (accepts)
                seeds.append(n);
        }
        break;
    }

    case RegistryChange::Kind::SourceConversion: {
        // Whether *any* conversion is registered decides between passthrough
        // and the conversion layer for every measurement with source data, so
        // every cached measurement name re-resolves, not only the names this
        // family accepts.
        const QSet<GraphNode> resolutions = knownNodes(GraphNode::Kind::Resolution);
        for (const GraphNode &n : resolutions) {
            if (n.measurementName)
                seeds.append(n);
        }
        break;
    }
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

    // Forget the cached "not requested" answer BEFORE evaluating. While it is
    // in the cache a nested lookup of this calculation (an input that
    // transitively reads one of its outputs) would be served from the cache
    // ahead of the stack check in ensureResult(): no cycle would be reported,
    // and the input would resolve as though the calculation were still not
    // requested. Everything that cached an answer derived from "not requested"
    // goes with it, so no input of this evaluation is served from that stale
    // state either. A "not requested" entry looked at nothing, so it has no
    // forward edges of its own; dropForwardEdges is for symmetry.
    if (cached != m_results.constEnd()) {
        m_results.remove(C);
        dropForwardEdges(C);
    }
    outcome.invalidated = invalidate(m_dependents.value(C).values());

    Scope scope;
    const ResultEntry entry = computeResult(instance, /*fromRequest=*/true, scope);
    if (evaluated)
        *evaluated = true;

    // Whatever looked at this calculation DURING the evaluation saw it on the
    // stack (a cycle), so it was provisional and nothing was cached for it:
    // every output appears at once with the publication below. This scope is
    // the root of the evaluation, so its own result - Cycle included - is
    // context-free by construction and is published.
    Q_ASSERT(isContextFree(scope));
    m_results.insert(C, entry);
    publishEdges(C, scope);
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
    const GraphNode C = GraphNode::result(instanceId);
    const auto it = m_results.constFind(C);
    if (it != m_results.constEnd())
        return it->status;
    // No cached result: the last provisional verdict, if one is still current.
    const auto provisional = m_provisional.constFind(C);
    if (provisional != m_provisional.constEnd())
        return provisional->status;
    return std::nullopt;
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
