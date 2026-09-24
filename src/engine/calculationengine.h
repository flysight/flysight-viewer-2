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

#include "blockerreport.h"
#include "calctypes.h"
#include "calculationregistry.h"
#include "preparedcalculation.h"
#include "sessionstate.h"
#include "storedcalculationresult.h"

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
/// Inspection and invalidation never compute.
///
/// Stored results. The installed Ok result of a plain explicit calculation can
/// be exported as a StoredCalculationResult (exportResult) and restored into an
/// engine over the same inputs (restoreResult). Export is an inspection that
/// reads the state and the preferences for the input fingerprint. Restore is a
/// publication without a run: it gathers the inputs as prepare() does, which
/// repeats every lookup the result made, and, if every name resolved as the
/// snapshot says and the snapshot is otherwise still valid, installs it
/// through the install step of a publish, so readers, blockers,
/// resultStatus(), dependenciesOf() and later invalidation cannot tell it
/// from a fresh one. Both are main-thread only. Run counters count runs, and
/// a restore is not one. An explicit-result listener hears of every requested
/// install and of every drop by an input change or by a registry change made
/// while the application runs, so that its owner can keep stored results in
/// step.
///
/// Single-threaded: the engine, its registry, and its session are used from the
/// main thread only, and the engine creates no thread and holds no lock. The one
/// thing that may run elsewhere is PreparedCalculation::compute(), which sees
/// the inputs captured by prepare() and nothing of the engine; the rule is
/// spelled out in preparedcalculation.h.
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

    /// What happened to an explicit calculation's cached result.
    struct ExplicitResultEvent {
        enum class Kind {
            Installed,  ///< request(), prepare() (NothingToRun / Blocked) or publish() cached a requested result
            Dropped     ///< an invalidation that started at an input, or at a registry change made
                        ///< while the application runs, dropped a requested result
        };
        Kind kind = Kind::Installed;
        QString instanceId;                                 ///< == the calculation id for a plain calculation
        ResultStatus status = ResultStatus::NotRequested;   ///< the status installed / dropped
    };
    using ExplicitResultListener = std::function<void(const ExplicitResultEvent &)>;
    /// Called once per event, in the order the events happened, at the end of
    /// the engine call that caused them and never inside an evaluation.
    /// Installed is reported for every status a request or publish installs;
    /// on-demand results are never reported. An input change counts when it
    /// is a leaf notification (attributeChanged, sourceMeasurementChanged,
    /// sourceUnitChanged), a preference change, a calculation being requested,
    /// published or restored whose "not requested" answer a requested result
    /// had used, or a registry change (a registration, or a removal with
    /// CalculationRegistry::Removal::Change) that reaches the result.
    ///
    /// The listener may call exportResult() and any const inspection; it must
    /// not mutate the session. An Installed event can be followed in the same
    /// call by a Dropped event for the same result (a deferred invalidation
    /// flushed right after the install): at the Installed event exportResult()
    /// then already returns nullopt.
    ///
    /// Never called for restoreResult()'s own install, clear(), a removal with
    /// CalculationRegistry::Removal::Teardown, the registry's destruction or
    /// the engine's destruction. Travels with the engine (a moved SessionData
    /// keeps it), like the invalidation listener. Nothing is queued while no
    /// listener is set.
    void setExplicitResultListener(ExplicitResultListener l);

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

    // ---- asynchronous request (see preparedcalculation.h) --------------------
    // prepare() here, PreparedCalculation::compute() on any thread, then
    // PreparedCalculation::publish() here. Both paths give identical results.
    struct PrepareOutcome {
        enum class Kind {
            NotFound,       ///< unknown id / name not of this family: nothing changed
            NotExplicit,    ///< only explicit calculations may be prepared: nothing changed
            AlreadyValid,   ///< a valid result is cached (`status`); it is never recomputed
            NothingToRun,   ///< an input is unavailable or on a cycle: cached as request() would (`status`)
            Blocked,        ///< NothingToRun, and requesting `blockers` first can change that
            Ready           ///< inputs captured; `ticket` is set and nothing was cached
        };
        Kind kind = Kind::NotFound;
        ResultStatus status = ResultStatus::NotRequested;   ///< AlreadyValid / NothingToRun / Blocked
        QList<CalculationBlocker> blockers;                 ///< Blocked only
        std::unique_ptr<PreparedCalculation> ticket;        ///< Ready only
        /// The cached names dropped because they had been read while the
        /// calculation was "not requested". NothingToRun / Blocked: the caller
        /// passes them on, like RequestOutcome::invalidated. Ready: informational
        /// (no value changed); publish() reports these names again.
        QSet<DependencyKey> invalidated;
    };
    /// Step 1 of the asynchronous request. Resolves and captures every declared
    /// input, computing on-demand inputs as needed; never runs an explicit
    /// calculation, this one included. Until the ticket is published the
    /// calculation stays "not requested" for every reader.
    PrepareOutcome prepare(const CalculationId &id,
                           const DependencyKey &instanceOutput = DependencyKey::attribute(QString()));

    // ---- stored results (see storedcalculationresult.h) ---------------------
    /// The installed result of the plain explicit calculation `id` as a
    /// snapshot - bundle, detail, leaves, the resolutions of every name it
    /// looked up, and the input fingerprint - or nullopt when there is none to
    /// store: unknown id, a family (or family instance) id, an on-demand
    /// calculation, no registry, nothing cached, a cached status other than Ok
    /// (NotRequested, MissingInput, Cycle, Failed, UndeclaredRead,
    /// InvalidOutput), or a result whose evaluation met a dependency ring (what
    /// provided a name may then have been a provisional answer that no cache
    /// entry holds). Const: never resolves, never computes, never changes the
    /// cache; reads the session state and the preference provider for the
    /// fingerprint. May be called from an explicit-result listener; not from
    /// inside a compute function.
    std::optional<StoredCalculationResult> exportResult(const CalculationId &id) const;

    struct RestoreOutcome {
        enum class Kind {
            NotFound,           ///< no registry, unknown id, or a family: nothing changed
            NotExplicit,        ///< an on-demand calculation: nothing changed
            AlreadyInstalled,   ///< a result is cached (`status`; any but NotRequested): nothing changed
            Stale,              ///< `staleCheck` failed: nothing installed
            Restored            ///< installed with status Ok
        };
        enum class StaleCheck {
            None,
            ResultVersion,      ///< snapshot.resultVersion != the descriptor's
            Bundle,             ///< an output the descriptor does not declare, or detail != bundle.reason()
            InputsUnavailable,  ///< gathering ended MissingInput or Cycle (`status`)
            Resolutions,        ///< the gathering met a ring, or a looked-up name resolved differently (another
                                ///< provider, instance or result version, a name looked up in only one of the two)
            Leaves,             ///< the current leaf list differs from snapshot.leaves
            Fingerprint         ///< same leaves, different input fingerprint
        };
        Kind kind = Kind::NotFound;
        StaleCheck staleCheck = StaleCheck::None;
        /// AlreadyInstalled: the cached status. Restored: Ok. Stale with
        /// InputsUnavailable: the gathering status. Otherwise NotRequested.
        ResultStatus status = ResultStatus::NotRequested;
        /// Cached names dropped because they had been read while the
        /// calculation was "not requested", exactly like
        /// PublishOutcome::invalidated. Non-empty only when gathering ran
        /// (Stale with InputsUnavailable / Resolutions / Leaves / Fingerprint,
        /// or Restored). The caller passes them on so consumers re-read.
        QSet<DependencyKey> invalidated;
    };
    /// Installs `snapshot` as the published result of its calculation, provided
    /// it is still valid here. The checks, in order: ResultVersion, Bundle,
    /// then after gathering InputsUnavailable, Resolutions (the repeated
    /// lookups must give the snapshot's answers), Leaves, Fingerprint. Not a
    /// request: it runs no compute function (on-demand inputs are evaluated as
    /// for a fresh request, as prepare() does), counts no run, creates no
    /// ticket, and queues no Installed event.
    /// On success the edges, status, detail and bundle are those a fresh
    /// publish would install. An outstanding ticket for the same calculation
    /// then publishes as RefusedStale / AlreadyPublished. A stale snapshot
    /// caches nothing for the calculation: it reads "not requested" again.
    /// Main thread, between evaluations; from inside an evaluation it asserts
    /// and returns NotFound.
    RestoreOutcome restoreResult(const StoredCalculationResult &snapshot);

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
    /// The text that goes with a cached result: the bundle's reason
    /// (CalculationResult::setReason) for a clean run, the exception text for
    /// Failed. Empty when there is none or when no result is cached.
    QString resultDetail(const CalculationId &id,
                         const DependencyKey &instanceOutput = DependencyKey::attribute(QString())) const;

    // ---- blocker inspection: may compute on-demand values; never runs an
    // explicit calculation; never changes what a read returns ----------------
    // Performs ordinary reads and const lookups only, so by the idempotency
    // invariant it cannot change any later answer. Must not be called from
    // inside a compute function. An outstanding ticket does not change a report.
    /// Which explicit calculations currently stand between `name` and its
    /// availability, seen through any number of on-demand intermediates.
    BlockerReport blockers(const DependencyKey &name);
    /// Where one calculation instance stands; input availability is decided
    /// before policy. "Can this be requested for this session" is state Ready.
    CalculationReadiness readiness(const CalculationId &id,
                                   const DependencyKey &instanceOutput = DependencyKey::attribute(QString()));

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
    int  edgeCount() const;         ///< total forward edges, those of outstanding tickets included
    int  cachedNodeCount() const;   ///< resolutions and results; outstanding tickets are not counted
    /// Tickets handed out by prepare() that were neither published nor
    /// destroyed yet (a ticket whose inputs went stale is still counted).
    int  preparedCount() const;
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
    friend class PreparedCalculation;   // forget(), publishPrepared()

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
        QString detail;             // resultDetail(): the bundle's reason, or the failure text
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
    // The three steps of an evaluation, shared by request() / ordinary reads
    // and by prepare() / publish(), so the two paths cannot diverge. (The
    // middle one is PreparedCalculation::run().)
    ResultStatus gatherInputs(const CalculationInstance &instance, EvaluationContext &ctx);
    /// The value of a LEAF input - a preference, a source measurement or a
    /// source unit - or nullopt when it is unavailable. The one definition of
    /// leaf availability, for evaluation (gatherInputs) and for inspection
    /// (inspectInstance). A pure read of the providers: no edge is noted and
    /// nothing is cached. Not for Attribute / Measurement inputs (nullopt).
    std::optional<EvaluationContext::InputValue> readLeafInput(const CalcInput &input) const;
    ResultEntry acceptRun(const CalculationInstance &instance, ComputedCalculation &&computed,
                          bool requested, bool logUndeclaredReads);
    bool cachedAnswerUsable(const GraphNode &node, bool sawCycle);
    bool isContextFree(const Scope &closed) const;
    void handUp(const Scope &closed);
    RequestOutcome requestInstance(const CalculationInstance &instance, bool *evaluated = nullptr);
    QSet<DependencyKey> dropNotRequested(const GraphNode &result);
    /// The availability pass of an explicit request, as the root of an
    /// evaluation: pushes the Result scope of `instance`, gathers its inputs
    /// into `context`, pops, and hands back the closed scope. Shared by
    /// prepare() and restoreResult().
    ResultStatus gatherAsRoot(const CalculationInstance &instance, EvaluationContext &context, Scope &closed);

    enum class InstallOrigin { Request, Restore };
    /// The one install step of a requested result: caches `entry` under C and
    /// publishes the edges and provisional verdicts of `scope` (publishEdges).
    /// Request: queues an Installed event when the calculation is explicit.
    /// Restore: queues nothing. Used by requestInstance(), prepare()'s
    /// NothingToRun path, publishPrepared() and restoreResult().
    void installRequested(const GraphNode &C, const ResultEntry &entry, const Scope &scope, InstallOrigin origin);

    // Stored results
    /// What closureOf() collects.
    struct Closure {
        QList<GraphNode> leaves;         ///< StoredAttribute / SourceMeasurement / SourceUnit / Preference; sorted by storedLeafLess, unique
        QList<GraphNode> resolutions;    ///< every Resolution node visited, unique, in no particular order
    };
    /// Everything a result reached from `direct` through the recorded edges:
    /// breadth-first over m_dependsOn following Resolution and Result nodes
    /// (explicit results included); Prepared nodes are ignored. Const; touches
    /// the edge maps only.
    Closure closureOf(const QSet<GraphNode> &direct) const;

    /// The StoredResolution of each node, sorted by storedResolutionLess; nullopt when
    /// one cannot be stated from the cache: a node without an m_resolutions entry, or a
    /// Calculation provider whose Result node has no m_results entry (or no descriptor).
    /// Neither happens for a result whose evaluation met no ring (see exportResult()).
    /// Const; touches m_resolutions / m_results only.
    std::optional<QList<StoredResolution>> storedResolutions(const QList<GraphNode> &resolutionNodes) const;

    // Asynchronous request
    void forget(PreparedCalculation *ticket);       // called by the ticket's destructor
    void withdraw(PreparedCalculation &ticket);
    PublishOutcome publishPrepared(PreparedCalculation &ticket, ComputedCalculation &&computed);

    // Blocker inspection
    struct InstanceInspection {
        CalculationReadiness readiness;
        QList<UnproducedNote> notProduced;  // from unavailable inputs that "ran and did not produce"
        bool notApplicable = false;         // an unavailable input no request could provide
    };
    InstanceInspection inspectInstance(const CalculationInstance &instance, QSet<DependencyKey> &walking);
    BlockerReport inspectName(const DependencyKey &name, QSet<DependencyKey> &walking);
    static CalculationBlocker makeBlocker(const CalculationInstance &instance, const DependencyKey &selectedBy);

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
    /// Whether an invalidation reports the requested explicit results it
    /// drops to the explicit-result listener: Report for input changes and
    /// for registry changes made while the application runs; Suppress for
    /// registrations removed as teardown.
    enum class ExplicitDrops { Report, Suppress };
    QSet<DependencyKey> invalidate(const QList<GraphNode> &seeds, ExplicitDrops report);
    QSet<DependencyKey> notifyLeafChanged(const GraphNode &leaf, const DependencyKey &changedName);
    void deliverBroadcast(const QList<GraphNode> &seeds, ExplicitDrops report);
    void flushPending();
    /// Hands the queued explicit-result events to the listener; nothing while
    /// an evaluation is in progress.
    void deliverExplicitEvents();
    /// A requested result of an explicit calculation: what the explicit-result
    /// listener hears about.
    static bool isReportedExplicit(const ResultEntry &entry);

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

    // Outstanding asynchronous requests by their graph node. A ticket's node
    // has the forward edges its result would have, and nothing depends on it.
    QHash<GraphNode, PreparedCalculation *> m_prepared;
    quint64 m_preparedSerial = 0;

    std::vector<Scope> m_scopes;

    // Notifications that (wrongly) arrived during an evaluation; applied once
    // the evaluation has unwound.
    // m_pendingSeeds' drops are reported to the explicit-result listener (input
    // changes and runtime registry changes); m_pendingTeardownSeeds are the
    // seeds of an invalidation that reports no explicit drops (a teardown
    // removal), kept apart from them.
    QList<GraphNode> m_pendingSeeds;
    QList<GraphNode> m_pendingTeardownSeeds;
    bool m_pendingClear = false;

    InvalidationListener m_listener;
    ExplicitResultListener m_explicitListener;
    QList<ExplicitResultEvent> m_explicitEvents;    // queued; see deliverExplicitEvents()

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
