#include "preparedcalculation.h"

#include <exception>
#include <new>
#include <utility>

#include "calculationengine.h"

namespace FlySight {

PreparedCalculation::PreparedCalculation(CalculationEngine *engine, const CalculationInstance &instance,
                                         std::unique_ptr<EvaluationContext> context, const GraphNode &node)
    : m_descriptor(instance.descriptor)
    , m_context(std::move(context))
    , m_engine(engine)
    , m_instance(instance)
    , m_node(node)
{
}

PreparedCalculation::~PreparedCalculation()
{
    // Both this and the engine's destructor run on the main thread, so a plain
    // back-pointer that each side nulls for the other is enough.
    if (m_engine)
        m_engine->forget(this);
}

CalculationId PreparedCalculation::registrationId() const { return m_instance.registrationId; }
QString PreparedCalculation::instanceId() const { return m_instance.instanceId; }

QString PreparedCalculation::title() const
{
    return m_instance.descriptor->title.isEmpty() ? m_instance.instanceId : m_instance.descriptor->title;
}

// =============================================================================
// Compute (any thread)
// =============================================================================

ComputedCalculation PreparedCalculation::run(const CalculationDescriptor &descriptor,
                                             EvaluationContext &context, CalculationProgress *progress)
{
    ComputedCalculation computed;
    context.m_progress = progress;

    // The order of the handlers is the classification. CalculationCancelled is
    // not a std::exception; std::bad_alloc is one, so it has to come first.
    try {
        computed.bundle = descriptor.compute(context);
        computed.kind = ComputedCalculation::Kind::Completed;
    } catch (const CalculationCancelled &) {
        computed.kind = ComputedCalculation::Kind::Cancelled;
    } catch (const std::bad_alloc &) {
        // Memory is what ran out: build no text here.
        computed.kind = ComputedCalculation::Kind::ResourceExhausted;
    } catch (const std::exception &ex) {
        computed.kind = ComputedCalculation::Kind::Failed;
        try {
            computed.failureText = QString::fromUtf8(ex.what());
        } catch (...) {
            // No memory for the text: the failure is reported without it.
        }
    } catch (...) {
        computed.kind = ComputedCalculation::Kind::Failed;
        computed.nonStandardException = true;
        computed.failureText = QStringLiteral("non-standard exception");
    }

    context.m_progress = nullptr;
    computed.undeclaredReads = std::move(context.m_undeclaredReads);
    context.m_undeclaredReads.clear();
    return computed;
}

ComputedCalculation PreparedCalculation::compute(CalculationProgress *progress)
{
    // Reads the compute-thread fields only: never m_engine, never the
    // staleness fields, never the registry or a session.
    if (m_computeStarted) {
        Q_ASSERT_X(false, "PreparedCalculation", "compute() called twice");
        return ComputedCalculation();   // Cancelled: publishes nothing
    }
    m_computeStarted = true;
    return run(*m_descriptor, *m_context, progress);
}

// =============================================================================
// Publish (main thread)
// =============================================================================

PublishOutcome PreparedCalculation::publish(ComputedCalculation computed)
{
    PublishOutcome outcome;
    if (m_spent) {
        Q_ASSERT_X(false, "PreparedCalculation", "publish() called twice");
        outcome.kind = PublishOutcome::Kind::RefusedStale;
        outcome.reason = PublishOutcome::Reason::InputsChanged;
        return outcome;
    }
    m_spent = true;

    if (!m_engine)
        return outcome;     // the default: RefusedGone / SessionGone

    // The engine decides, and withdraws the ticket whatever it decides.
    return m_engine->publishPrepared(*this, std::move(computed));
}

bool PreparedCalculation::willBeRefused() const
{
    // The engine's own marks, read back: nothing is decided here. A ticket
    // whose engine is gone was marked by the engine's destructor.
    return !m_spent && (m_refused || !m_engine);
}

PublishOutcome::Reason PreparedCalculation::refusalReason() const
{
    if (!willBeRefused())
        return PublishOutcome::Reason::None;
    return m_refused ? m_refusalReason : PublishOutcome::Reason::SessionGone;
}

void PreparedCalculation::markStale()
{
    if (m_refused)
        return;
    m_refused = true;
    m_refusalKind = PublishOutcome::Kind::RefusedStale;
    m_refusalReason = PublishOutcome::Reason::InputsChanged;
}

void PreparedCalculation::markGone(PublishOutcome::Reason reason)
{
    m_refused = true;
    m_refusalKind = PublishOutcome::Kind::RefusedGone;
    m_refusalReason = reason;
}

} // namespace FlySight
