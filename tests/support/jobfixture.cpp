#include "jobfixture.h"

#include <new>
#include <stdexcept>

#include <QMutexLocker>
#include <QtTest>

#include "builtinfixture.h"
#include "engine/calculationdescriptor.h"
#include "engine/calculationprogress.h"
#include "engine/calculationregistry.h"
#include "engine/calculationresult.h"
#include "engine/evaluationcontext.h"
#include "fakesessionstate.h"
#include "jobqueue.h"

using namespace FlySight;

namespace FlySightTest {

// ---- Gate ---------------------------------------------------------------------

void Gate::enter(int inputValue)
{
    const int now = running.fetch_add(1) + 1;
    int seen = maxRunning.load();
    while (now > seen && !maxRunning.compare_exchange_weak(seen, now)) {
    }
    {
        const QMutexLocker lock(&m_mutex);
        m_startOrder.append(inputValue);
    }
    entered.release();
}

bool Gate::waitEntered(int timeoutMs)
{
    return QTest::qWaitFor([this] { return entered.tryAcquire(1); }, timeoutMs);
}

void Gate::leave()
{
    running.fetch_sub(1);
}

QList<int> Gate::startOrder() const
{
    const QMutexLocker lock(&m_mutex);
    return m_startOrder;
}

namespace {

/// Inside the gate for as long as it lives, however the compute function leaves.
class GateScope {
public:
    GateScope(Gate &gate, int inputValue) : m_gate(gate) { m_gate.enter(inputValue); }
    ~GateScope() { m_gate.leave(); }
    Q_DISABLE_COPY_MOVE(GateScope)
private:
    Gate &m_gate;
};

QString inputOf(const char *prefix) { return QString::fromLatin1(prefix) + QStringLiteral("_IN"); }
QString outputOf(const char *prefix) { return QString::fromLatin1(prefix) + QStringLiteral("_OUT"); }

/// An explicit, titled calculation <prefix>_OUT = f(<prefix>_IN).
CalculationDescriptor explicitCalculation(const char *id, const char *title, const char *prefix)
{
    CalculationDescriptor d;
    d.id = QString::fromLatin1(id);
    d.title = QString::fromLatin1(title);
    d.policy = EvaluationPolicy::Explicit;
    d.inputs = {CalcInput::attribute(inputOf(prefix))};
    d.outputs = {DependencyKey::attribute(outputOf(prefix))};
    return d;
}

CalculationDescriptor gated(const std::shared_ptr<Gate> &gate)
{
    CalculationDescriptor d = explicitCalculation("gated", "Gated", "G");
    d.compute = [gate](const EvaluationContext &ctx) {
        const int in = ctx.attribute(inputOf("G")).toInt();
        const GateScope inside(*gate, in);
        ctx.progress().report(QStringLiteral("step 1"));
        // A timed poll of the cancel flag: what a real compute function does at
        // its solver boundaries. Not a sleep used for synchronization.
        while (!gate->proceed.tryAcquire(1, 1))
            ctx.progress().throwIfCancelled();
        ctx.progress().report(QStringLiteral("step 2"));
        return CalculationResult().setAttribute(outputOf("G"), in + 1);
    };
    return d;
}

CalculationDescriptor stubborn(const std::shared_ptr<Gate> &gate)
{
    CalculationDescriptor d = explicitCalculation("stubborn", "Stubborn", "S");
    d.compute = [gate](const EvaluationContext &ctx) {
        const int in = ctx.attribute(inputOf("S")).toInt();
        const GateScope inside(*gate, in);
        ctx.progress().report(QStringLiteral("step 1"));
        while (!gate->proceed.tryAcquire(1, 1)) {
            if (!ctx.progress().isCancelled())
                continue;
            // Notices the request, finishes its "solver step", and returns a
            // complete result instead of throwing.
            volatile double sum = 0.0;
            for (int i = 0; i < 1000000; ++i)
                sum = sum + double(i);
            break;
        }
        return CalculationResult().setAttribute(outputOf("S"), in + 1);
    };
    return d;
}

CalculationDescriptor thrower()
{
    CalculationDescriptor d = explicitCalculation("thrower", "Thrower", "T");
    d.compute = [](const EvaluationContext &) -> CalculationResult {
        throw std::runtime_error("synthetic failure");
    };
    return d;
}

CalculationDescriptor exhausted()
{
    CalculationDescriptor d = explicitCalculation("exhausted", "Exhausted", "X");
    // Test-local state: real compute functions hold none
    const auto calls = std::make_shared<std::atomic<int>>(0);
    d.compute = [calls](const EvaluationContext &ctx) {
        if (calls->fetch_add(1) == 0)
            throw std::bad_alloc();
        return CalculationResult().setAttribute(outputOf("X"), ctx.attribute(inputOf("X")).toInt() + 1);
    };
    return d;
}

CalculationDescriptor deepStack()
{
    CalculationDescriptor d = explicitCalculation("deepstack", "Deep stack", "D");
    d.compute = [](const EvaluationContext &ctx) {
        // Far beyond a default thread stack (1 MiB on Windows). Touched page by
        // page, in order, so that the guard page can grow the stack.
        constexpr int kSize = 16 * 1024 * 1024;
        constexpr int kPage = 4096;
        volatile char buffer[kSize];
        for (int i = 0; i < kSize; i += kPage)
            buffer[i] = char((i / kPage) & 0x7f);
        long long sum = 0;
        for (int i = 0; i < kSize; i += kPage)
            sum += buffer[i];
        // 4096 pages holding 0..127 thirty-two times: 32 * (127 * 128 / 2)
        if (sum != 260096)
            throw std::runtime_error("deep stack buffer was not written");
        return CalculationResult().setAttribute(outputOf("D"), ctx.attribute(inputOf("D")).toInt() + 1);
    };
    return d;
}

} // namespace

// ---- JobWorld -------------------------------------------------------------------

JobWorld::JobWorld()
    : m_gate(std::make_shared<Gate>())
{
    CalculationRegistry &registry = CalculationRegistry::instance();
    const QStringList before = registry.registeredIds();

    Synthetic::registerExplicitWorld(registry);
    registry.registerCalculation(gated(m_gate));
    registry.registerCalculation(stubborn(m_gate));
    registry.registerCalculation(thrower());
    registry.registerCalculation(exhausted());
    registry.registerCalculation(deepStack());

    // Remember what was added, not what was asked for: a refused registration
    // must not unregister somebody else's calculation later.
    const QStringList after = registry.registeredIds();
    for (const QString &id : after) {
        if (!before.contains(id))
            m_ids.append(id);
    }
}

JobWorld::~JobWorld()
{
    CalculationRegistry &registry = CalculationRegistry::instance();
    for (auto it = m_ids.crbegin(); it != m_ids.crend(); ++it)
        registry.unregister(*it);
}

QList<SessionData> JobWorld::sessions(const QStringList &ids)
{
    QList<SessionData> result;
    for (const QString &id : ids)
        result.append(DescentFixture::load(id.toLatin1()));
    return result;
}

bool waitIdle(JobQueue &queue, int timeoutMs)
{
    return QTest::qWaitFor([&queue] { return queue.isIdle(); }, timeoutMs);
}

} // namespace FlySightTest
