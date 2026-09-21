#ifndef FLYSIGHT_ENGINE_PREPAREDCALCULATION_H
#define FLYSIGHT_ENGINE_PREPAREDCALCULATION_H

#include <memory>

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include "calctypes.h"
#include "calculationprogress.h"
#include "calculationregistry.h"
#include "calculationresult.h"
#include "evaluationcontext.h"

// The asynchronous request: an explicit calculation run in three steps.
//
//   1. CalculationEngine::prepare()      MAIN THREAD  resolve and capture the inputs
//   2. PreparedCalculation::compute()    ANY THREAD   run against the captured inputs only
//   3. PreparedCalculation::publish()    MAIN THREAD  install the result, or refuse it
//
// THE THREADING RULE. The engine, the registry, and every session stay on the
// main thread. A PreparedCalculation is created, inspected, published, and
// destroyed there. Only compute() may run elsewhere, once, and the caller
// guarantees it has returned before publish() or the destructor runs (a thread
// join, or a queued "finished" signal, gives that ordering). The worker sees the
// captured inputs and the progress/cancel facility, and nothing else.
//
// There is no lock and no atomic in this library. The fields of a ticket are
// PARTITIONED between the threads instead (see the private section), and the
// captured values are Qt implicitly shared copies taken during prepare: Qt's
// reference counts are atomic and every main-thread writer detaches before it
// writes, so the worker's buffers never change under it and later session
// edits cannot reach them. No deep copy is made.
//
// The compute function of an explicit calculation must therefore be
// re-entrant: no mutable captured state, no statics. It may be running on a
// worker while another session evaluates the same descriptor.
//
// Nothing is logged from compute(). Warnings caused by an asynchronous run are
// emitted by publish(), on the main thread.

namespace FlySight {

class CalculationEngine;

/// What compute() returns: an opaque, movable payload for publish(), plus how
/// the run ended. Safe to move between threads (hand it over; do not share it).
struct ComputedCalculation {
    enum class Kind {
        Completed,          ///< returned normally; the status is decided at publish
        Failed,             ///< threw (not cancellation, not resource exhaustion): published and cached as Failed
        Cancelled,          ///< threw CalculationCancelled: nothing is published, nothing is cached
        ResourceExhausted   ///< threw std::bad_alloc: nothing is published, nothing is cached
    };
    Kind kind = Kind::Cancelled;
    QString failureText;    ///< Failed: what(), or "non-standard exception"

private:
    friend class CalculationEngine;
    friend class PreparedCalculation;

    CalculationResult bundle;               // Completed only
    QList<CalcInput> undeclaredReads;       // what the context recorded during the run
    bool nonStandardException = false;      // Failed: selects the warning text
};

/// What publish() reports. `invalidated` is the caller's to pass on, exactly
/// like CalculationEngine::RequestOutcome::invalidated.
struct PublishOutcome {
    enum class Kind {
        Published,      ///< the result (of any status, Failed included) is installed
        RefusedStale,   ///< the result is no longer wanted here: discarded whole
        RefusedGone,    ///< there is nowhere to install it: discarded whole
        Discarded       ///< the run produced nothing to install
    };
    enum class Reason {
        None,
        InputsChanged,          ///< RefusedStale: something the prepared inputs depended on was invalidated
        AlreadyPublished,       ///< RefusedStale: a synchronous request() published in between
        SessionGone,            ///< RefusedGone: the engine was destroyed
        RegistrationRemoved,    ///< RefusedGone: the calculation was unregistered (even if re-registered since)
        Cancelled,              ///< Discarded: compute() ended as Cancelled
        ResourceExhausted       ///< Discarded: compute() ended as ResourceExhausted
    };
    Kind kind = Kind::RefusedGone;
    Reason reason = Reason::SessionGone;
    ResultStatus status = ResultStatus::NotRequested;   ///< Published only: the cached status
    QString detail;                                     ///< Published only: == CalculationEngine::resultDetail()
    /// Published only. The names that had been read while the calculation was
    /// "not requested" - at any time up to this publication - and whose cached
    /// answer was dropped. The caller must pass them on so consumers re-read.
    QSet<DependencyKey> invalidated;
};

/// One outstanding asynchronous request (a "ticket"), created by
/// CalculationEngine::prepare() and held by std::unique_ptr.
///
/// While a ticket is outstanding the calculation is still "not requested" for
/// every reader and for inspection. Destroying an unpublished ticket leaves it
/// that way, as if it had never been asked.
///
/// The engine decides whether the result may be installed, from its own
/// dependency records: the ticket is a node of the dependency graph with the
/// edges the published result would have had, so whatever would have
/// invalidated that result makes publish() refuse. The ticket may outlive the
/// engine; publish() then refuses too.
class PreparedCalculation {
public:
    /// MAIN THREAD. Withdraws from the engine if it still exists. compute()
    /// must not be running.
    ~PreparedCalculation();
    Q_DISABLE_COPY_MOVE(PreparedCalculation)

    CalculationId registrationId() const;   ///< MAIN THREAD
    QString instanceId() const;             ///< MAIN THREAD
    QString title() const;                  ///< MAIN THREAD; the descriptor's title, else the instance id

    /// ANY THREAD, at most once. Touches the captured inputs, the descriptor,
    /// and `progress` (CalculationProgress::none() when null) - nothing else.
    /// Never throws and never logs. A second call asserts and returns Cancelled.
    ComputedCalculation compute(CalculationProgress *progress = nullptr);

    /// MAIN THREAD, at most once, after compute() has returned. Checks, in this
    /// order: engine destroyed; registration removed; inputs invalidated since
    /// prepare; the run was cancelled / out of memory; a result was published
    /// in between. Otherwise installs the result for all outputs at once, with
    /// the semantics of CalculationEngine::request(). Whatever the outcome the
    /// ticket is spent; a second call asserts and returns RefusedStale.
    PublishOutcome publish(ComputedCalculation computed);

    /// MAIN THREAD. True while a publish() still to come is already certain to
    /// refuse: the engine has marked the ticket stale (something the prepared
    /// inputs depended on was invalidated, the caches were cleared), the
    /// registration was removed, or the engine - the session - is gone. It
    /// reports what the ENGINE recorded; the caller decides nothing and may use
    /// it only to stop a computation nobody can use. Once true it stays true
    /// until publish(). False for a healthy ticket, and false again once
    /// publish() was called, whatever that returned: it says nothing about a
    /// publication that has happened. False does not promise a publication
    /// either: a synchronous request() in between (AlreadyPublished) and an
    /// invalidation the engine had to defer are decided by publish() alone.
    /// compute() never reads this.
    bool willBeRefused() const;
    /// MAIN THREAD. The reason publish() would give for that refusal
    /// (InputsChanged, RegistrationRemoved, SessionGone); None while
    /// willBeRefused() is false.
    PublishOutcome::Reason refusalReason() const;

private:
    friend class CalculationEngine;

    PreparedCalculation(CalculationEngine *engine, const CalculationInstance &instance,
                        std::unique_ptr<EvaluationContext> context, const GraphNode &node);

    /// The one place a compute function is invoked, shared by the synchronous
    /// and the asynchronous path. Classifies what it throws, takes the
    /// undeclared reads out of the context. No engine access, no logging, never
    /// throws; on the std::bad_alloc path it allocates nothing.
    static ComputedCalculation run(const CalculationDescriptor &descriptor, EvaluationContext &context,
                                   CalculationProgress *progress);

    // Engine-side marking (main thread). A ticket that was refused once stays
    // refused; "gone" outranks "stale".
    void markStale();
    void markGone(PublishOutcome::Reason reason);

    // ---- COMPUTE-THREAD FIELDS: written during prepare, then touched by
    // compute() only, until it has returned.
    std::shared_ptr<const CalculationDescriptor> m_descriptor;  // keeps the function alive past unregister
    std::unique_ptr<EvaluationContext> m_context;               // the captured inputs
    bool m_computeStarted = false;

    // ---- MAIN-THREAD FIELDS: never touched by compute().
    CalculationEngine *m_engine;            // nulled by the engine's destructor and by publish
    CalculationInstance m_instance;
    GraphNode m_node;                       // GraphNode::prepared(instance id, serial)
    // The closed scope of the availability pass, for publication:
    QSet<GraphNode> m_looked;
    QHash<GraphNode, ResultStatus> m_provisional;
    bool m_sawCycle = false;
    QSet<DependencyKey> m_droppedAtPrepare; // names read while "not requested", dropped by prepare()
    bool m_spent = false;                   // publish() was called
    bool m_refused = false;                 // a refusal is pending:
    PublishOutcome::Kind m_refusalKind = PublishOutcome::Kind::RefusedStale;
    PublishOutcome::Reason m_refusalReason = PublishOutcome::Reason::InputsChanged;
};

} // namespace FlySight

#endif // FLYSIGHT_ENGINE_PREPAREDCALCULATION_H
