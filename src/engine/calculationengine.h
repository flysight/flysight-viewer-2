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
        /// before the evaluation, so that no input is served from them), united
        /// with the names that looked at the calculation during the evaluation
        /// (dropped after it, so that all outputs appear at once). Empty when
        /// a valid cached result was returned without evaluating.
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
    std::optional<ResultStatus> resultStatus(const CalculationId &id,
                                             const DependencyKey &instanceOutput = DependencyKey::attribute(QString())) const;

    // ---- instrumentation (per engine) --------------------------------------
    int  runCount(const CalculationId &registrationId) const;   ///< family: sum over instances
    int  runCountForInstance(const QString &instanceId) const;
    int  totalRunCount() const;
    void resetRunCounts();
    int  cycleCount() const;
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
    };

    struct ResultEntry {
        ResultStatus status = ResultStatus::MissingInput;
        std::shared_ptr<const CalculationResult> bundle;    // non-null only for Ok
        bool requested = false;
        CalculationInstance instance;
    };

    /// One evaluation in progress. Everything read while it is the top of the
    /// stack is recorded in *its* set; there is no shared side-effect list.
    struct Scope {
        GraphNode node;
        QSet<GraphNode> looked;
        bool cycle = false;
    };
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
                              QSet<GraphNode> &looked);
    RequestOutcome requestInstance(const CalculationInstance &instance, bool *evaluated = nullptr);

    void note(const GraphNode &node);
    int stackIndexOf(const GraphNode &node) const;
    void reportCycle(int stackIndex, const GraphNode &reentered);

    // Publication and invalidation
    void setEdges(const GraphNode &node, const QSet<GraphNode> &looked);
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
