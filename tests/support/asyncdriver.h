#ifndef FLYSIGHTTEST_ASYNCDRIVER_H
#define FLYSIGHTTEST_ASYNCDRIVER_H

#include <atomic>
#include <memory>
#include <thread>

#include <QString>
#include <QStringList>
#include <QThread>
#include <QtTest>

#include "engine/calculationprogress.h"
#include "engine/preparedcalculation.h"

// Drives step 2 of an asynchronous request (PreparedCalculation::compute())
// for tests: inline, on a std::thread, or on a QThread. The engine library
// creates no thread and holds no lock or atomic; the ones here belong to the
// test, which plays the part of the job queue.
//
// Every helper returns only after compute() has returned (join / wait), which
// is the ordering the threading rule asks of the caller before publish().

namespace FlySightTest {

enum class ComputeMode { Inline, StdThread, QtThread };

/// Starts compute() in the given mode and joins it on finish() (or on
/// destruction). Inline mode runs compute() inside the constructor.
class ComputeRun {
public:
    ComputeRun(ComputeMode mode, FlySight::PreparedCalculation &ticket,
               FlySight::CalculationProgress *progress = nullptr)
    {
        FlySight::PreparedCalculation *t = &ticket;
        switch (mode) {
        case ComputeMode::Inline:
            m_result = t->compute(progress);
            break;
        case ComputeMode::StdThread:
            m_stdThread = std::thread([this, t, progress] { m_result = t->compute(progress); });
            break;
        case ComputeMode::QtThread:
            // QThread::create needs no event loop.
            m_qtThread.reset(QThread::create([this, t, progress] { m_result = t->compute(progress); }));
            m_qtThread->start();
            break;
        }
    }
    ~ComputeRun() { join(); }
    ComputeRun(const ComputeRun &) = delete;
    ComputeRun &operator=(const ComputeRun &) = delete;

    /// Waits for compute() to return and hands over what it returned.
    FlySight::ComputedCalculation finish()
    {
        join();
        return std::move(m_result);
    }

private:
    void join()
    {
        if (m_stdThread.joinable())
            m_stdThread.join();
        if (m_qtThread) {
            m_qtThread->wait();
            m_qtThread.reset();
        }
    }

    // Written by the worker, read only after the join.
    FlySight::ComputedCalculation m_result;
    std::thread m_stdThread;
    std::unique_ptr<QThread> m_qtThread;
};

/// compute() from start to finish in the given mode.
inline FlySight::ComputedCalculation computeOn(ComputeMode mode, FlySight::PreparedCalculation &ticket,
                                               FlySight::CalculationProgress *progress = nullptr)
{
    return ComputeRun(mode, ticket, progress).finish();
}

/// For `_data()` functions: one row per mode, column "mode" (an int, because
/// QFETCH of an enum would need a metatype): `QFETCH(int, mode);` then
/// `ComputeMode(mode)`.
inline void addComputeModeRows()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("inline") << int(ComputeMode::Inline);
    QTest::newRow("std::thread") << int(ComputeMode::StdThread);
    QTest::newRow("QThread") << int(ComputeMode::QtThread);
}

/// A progress facility as the job queue would implement it: the cancel flag is
/// the one thing both threads touch, so it is atomic; the texts are appended
/// by the worker and read only after the join.
class RecordingProgress : public FlySight::CalculationProgress {
public:
    void report(const QString &text) override { m_texts.append(text); }
    bool isCancelled() const override { return m_cancelled.load(); }

    void cancel() { m_cancelled.store(true); }
    QStringList texts() const { return m_texts; }   ///< after the join only

private:
    std::atomic<bool> m_cancelled{false};
    QStringList m_texts;
};

} // namespace FlySightTest

#endif // FLYSIGHTTEST_ASYNCDRIVER_H
