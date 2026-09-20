#ifndef FLYSIGHT_ENGINE_CALCULATIONENGINE_H
#define FLYSIGHT_ENGINE_CALCULATIONENGINE_H

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVector>

#include "calctypes.h"
#include "calculationregistry.h"
#include "sessionstate.h"

namespace FlySight {

/// Per-session calculation engine: the one cache of derived values, the one
/// dependency graph, and the resolution rule behind every ordinary read.
///
/// Resolving a public name: a stored attribute / source measurement wins;
/// otherwise the registered candidates are tried in registration order and the
/// first whose declared inputs are all available and which produces the output
/// wins. Whether an input is available is decided by persistent state and this
/// same rule applied recursively, never by what happens to be cached.
///
/// Everything a resolution looked at is recorded as a dependency - the stored
/// value's presence *or absence*, every candidate that was rejected, and the
/// winner - so any change that could alter the choice invalidates the cached
/// answer. Unavailable answers are cached like any other.
///
/// Invariant (idempotency): the value returned for a name is a pure function of
/// the session's persistent state, the declared preferences, and the registry.
/// evaluateFresh() is the oracle for it.
///
/// Dependency cycles. A lookup of a node that is already being evaluated is a
/// cycle: every calculation between the two occurrences gets ResultStatus::Cycle
/// and names carry on down their candidate lists. Which nodes are "already
/// being evaluated" depends on where the read started, so two rules keep the
/// answers independent of read order, including for overlapping rings:
///  - only a CONTEXT-FREE result is cached: one whose evaluation never looked up
///    a node that was being evaluated above it. It is then exactly what an
///    evaluation started at that node would produce. A ring closed entirely
///    beneath a node does not disqualify it, and the root of an evaluation has
///    nothing above it, so it is always context-free. Anything else is
///    PROVISIONAL: used by its caller, never cached, and everything it looked
///    at becomes a dependency of the nearest cached ancestor;
///  - a cached answer whose evaluation involved a cycle is served to a nested
///    lookup only if nothing it (transitively) looked at is being evaluated
///    right now; otherwise it is re-evaluated provisionally, because there it
///    would meet the ring again, from a different side.
/// Graphs without cycles never take either path, so they cost what they did.
///
/// Inspection and invalidation never compute. Single-threaded.
class CalculationEngine {
public:
    explicit CalculationEngine(const ISessionState *state,
                               CalculationRegistry *registry = &CalculationRegistry::instance());
    ~CalculationEngine();   ///< withdraws from the registry
    Q_DISABLE_COPY_MOVE(CalculationEngine)

    /// Same logical state at a new address (the owner was moved); keeps caches.
    void rebind(const ISessionState *state);

    // ---- ordinary reads (may compute) --------------------------------------
    // Unavailable reads return an invalid QVariant / empty vector / empty string.
    // Must not be called from inside a compute function.
    QVariant        attribute(const QString &key);
    QVector<double> measurement(const QString &sensor, const QString &name);
    QString         measurementUnit(const QString &sensor, const QString &name);
    bool            isAvailable(const DependencyKey &name);

    // ---- mutation notifications --------------------------------------------
    // Called by the owner of the state AFTER it has mutated the state. Each
    // returns the public names whose cached answer was dropped, always
    // including the changed name itself. They never compute and never touch
    // the state or the preference provider.
    QSet<DependencyKey> attributeChanged(const QString &key);   ///< set or remove
    QSet<DependencyKey> sourceMeasurementChanged(const QString &sensor, const QString &name);
    QSet<DependencyKey> sourceUnitChanged(const QString &sensor, const QString &name);
    QSet<DependencyKey> clear();    ///< drop every cache entry and edge

    /// Broadcast-originated invalidations (registry / preference changes) have
    /// no caller to return a set to; they are delivered here, only when
    /// non-empty. The direct notifications above do NOT call it.
    using InvalidationListener = std::function<void(const QSet<DependencyKey> &)>;
    void setInvalidationListener(InvalidationListener l);

    // ---- explicit evaluation -----------------------------------------------
    struct RequestOutcome {
        bool found = false;
        ResultStatus status = ResultStatus::NotRequested;
        /// Every cached name that was dropped by this request: the names that
        /// had been read while the calculation was "not requested" (dropped
        /// before the evaluation, so that no input is served from them). Empty
        /// when there were none, or when a valid cached result was returned
        /// without evaluating. Nothing that looks at the calculation DURING its
        /// evaluation is ever cached (it saw the calculation on the stack, so
        /// it is provisional), so all outputs appear at once with the result.
        QSet<DependencyKey> invalidated;
    };
    /// Synchronously evaluates one calculation by identity (any policy) and
    /// publishes one result for all of its outputs. A valid cached result is
    /// never recomputed. The second argument selects a family instance.
    RequestOutcome request(const CalculationId &id,
                           const DependencyKey &instanceOutput = DependencyKey::attribute(QString()));

    // ---- inspection: const, never resolves, never computes, never touches state
    enum class CachedState { NotCached, Available, Unavailable };
    CachedState cachedState(const DependencyKey &name) const;
    /// The status of the calculation's cached result. A calculation on a ring
    /// that was entered from above it has no cached result (its verdict was
    /// provisional); for it this reports a DIAGNOSTIC: the status from its most
    /// recent provisional evaluation, typically Cycle. Unlike the values of
    /// names, that diagnostic is not a function of state alone. In tangled
    /// rings a calculation can get different provisional verdicts from
    /// different entry points, so what is reported can depend on read order.
    /// A verdict that differs from the recorded one replaces it and is kept
    /// only on behalf of the cached answer that absorbed the NEW evaluation;
    /// when that answer is invalidated the status reverts to nullopt, even if
    /// an older answer that absorbed the earlier verdict is still cached.
    /// nullopt also when the calculation was never evaluated or when every
    /// answer holding its verdict was invalidated.
    std::optional<ResultStatus> resultStatus(const CalculationId &id,
                                             const DependencyKey &instanceOutput = DependencyKey::attribute(QString())) const;

    // ---- instrumentation (per engine) --------------------------------------
    int  runCount(const CalculationId &registrationId) const;   ///< family: sum over instances
    int  runCountForInstance(const QString &instanceId) const;
    int  totalRunCount() const;
    void resetRunCounts();
    int  cycleCount() const;        ///< detections, including those of provisional re-evaluations
    QList<GraphNode> lastCyclePath() const;
    int  undeclaredReadCount() const;
    std::pair<QString, CalcInput> lastUndeclaredRead() const;   ///< instance id, input
    int  scopeDepth() const;        ///< 0 outside evaluation
    int  edgeCount() const;         ///< total forward edges
    int  cachedNodeCount() const;
    QSet<GraphNode> dependenciesOf(const GraphNode &n) const;

    // ---- oracle ------------------------------------------------------------
    struct Value {
        bool available = false;
        QVariant attribute;
        QVector<double> samples;
        QString unit;
    };
    /// The value `name` has when evaluated from scratch on a throw-away engine
    /// over the same state and registry. This engine's cache, edges, and
    /// counters are untouched.
    Value evaluateFresh(const DependencyKey &name) const;
    /// Availability, attribute (QVariant ==), samples (bit pattern, so
    /// NaN == NaN and -0.0 != 0.0), and unit all equal.
    static bool sameValue(const Value &a, const Value &b);
    /// Reads each name normally and returns those that differ from evaluateFresh().
    QList<DependencyKey> verifyAgainstFresh(const QList<DependencyKey> &names);

private:
    friend class CalculationRegistry;

    enum class Provider { None, Stored, Source, Calculation };

    struct ResolutionEntry {
        Provider provider = Provider::None;
        QString instanceId;         // Provider::Calculation
        bool available = false;
        QVariant attribute;
        QVector<double> samples;    // implicitly shared: a passthrough costs no second buffer
        QString unit;
        bool sawCycle = false;      // see Scope::sawCycle
    };

    struct ResultEntry {
        ResultStatus status = ResultStatus::MissingInput;
        std::shared_ptr<const CalculationResult> bundle;    // non-null only for Ok
        bool requested = false;
        CalculationInstance instance;
        bool sawCycle = false;      // see Scope::sawCycle
    };

    /// One evaluation in progress. Everything read while it is the top of the
    /// stack is recorded in *its* set; there is no shared side-effect list.
    struct Scope {
        GraphNode node;
        /// What this evaluation looked at, plus everything its provisional
        /// (uncached) descendants looked at.
        QSet<GraphNode> looked;
        bool cycle = false;         ///< a Result scope on a detected ring
        /// The shallowest stack index that a cycle detected in this subtree
        /// re-entered. The scope at stack index d is context-free iff
        /// minHit >= d; equal means the ring's top frame is this scope itself.
        int minHit = kNoHit;
        /// A cycle was detected somewhere in this subtree, or a cached answer
        /// carrying this flag was used. Cached answers without it can be
        /// served without looking at the stack.
        bool sawCycle = false;
        /// Statuses of the provisional calculations beneath, for resultStatus().
        QHash<GraphNode, ResultStatus> provisional;
    };
    static constexpr int kNoHit = 0x7fffffff;
    class ScopeGuard;

    enum class Detached { Tag };
    /// Throw-away engine for evaluateFresh(): not enrolled, logs nothing.
    CalculationEngine(const ISessionState *state, CalculationRegistry *registry, Detached);

    // Registry-originated events
    void onRegistryChanged(const RegistryChange &change);
    void onPreferenceChanged(const QString &key);
    void registryDestroyed();

    // Resolution
    ResolutionEntry readTopLevel(const DependencyKey &name);
    ResolutionEntry resolve(const DependencyKey &name);
    bool tryCandidates(const QList<CalculationInstance> &candidates, const DependencyKey &name,
                       ResolutionEntry &entry);
    ResultEntry ensureResult(const CalculationInstance &instance);
    ResultEntry computeResult(const CalculationInstance &instance, bool fromRequest,
                              Scope &closed);
    bool cachedAnswerUsable(const GraphNode &node, bool sawCycle);
    bool isContextFree(const Scope &closed) const;
    void handUp(const Scope &closed);
    RequestOutcome requestInstance(const CalculationInstance &instance, bool *evaluated = nullptr);

    void note(const GraphNode &node);
    int stackIndexOf(const GraphNode &node) const;
    void reportCycle(int stackIndex, const GraphNode &reentered);

    // Publication and invalidation
    void publishEdges(const GraphNode &node, const Scope &closed);
    void setEdges(const GraphNode &node, const QSet<GraphNode> &looked);
    void dropProvisionalOwnedBy(const GraphNode &owner);
    void clearCaches();
    QSet<GraphNode> knownNodes(GraphNode::Kind kind) const;
    void dropForwardEdges(const GraphNode &node);
    QSet<DependencyKey> invalidate(const QList<GraphNode> &seeds);
    QSet<DependencyKey> notifyLeafChanged(const GraphNode &leaf, const DependencyKey &changedName);
    void deliverBroadcast(const QList<GraphNode> &seeds);
    void flushPending();

    static Value toValue(const ResolutionEntry &entry);

    const ISessionState *m_state;
    CalculationRegistry *m_registry;
    bool m_enrolled;
    bool m_quiet;

    QHash<GraphNode, ResolutionEntry> m_resolutions;
    QHash<GraphNode, ResultEntry> m_results;
    QHash<GraphNode, QSet<GraphNode>> m_dependsOn;      // node -> what it looked at
    QHash<GraphNode, QSet<GraphNode>> m_dependents;     // reverse edges

    // Inspection only (resultStatus): the last provisional status of each
    // uncached calculation, kept while a cached node that absorbed that
    // evaluation (an "owner") is still cached.
    struct ProvisionalStatus {
        ResultStatus status = ResultStatus::Cycle;
        QSet<GraphNode> owners;
    };
    QHash<GraphNode, ProvisionalStatus> m_provisional;          // Result node -> status
    QHash<GraphNode, QSet<GraphNode>> m_provisionalOwned;       // owner -> Result nodes

    std::vector<Scope> m_scopes;

    // Notifications that (wrongly) arrived during an evaluation; applied once
    // the evaluation has unwound.
    QList<GraphNode> m_pendingSeeds;
    bool m_pendingClear = false;

    InvalidationListener m_listener;

    QHash<QString, int> m_runsByInstance;
    QHash<CalculationId, int> m_runsByRegistration;
    int m_totalRuns = 0;
    int m_cycleCount = 0;
    QList<GraphNode> m_lastCyclePath;
    int m_undeclaredReadCount = 0;
    std::pair<QString, CalcInput> m_lastUndeclaredRead;
};

} // namespace FlySight

#endif // FLYSIGHT_ENGINE_CALCULATIONENGINE_H
