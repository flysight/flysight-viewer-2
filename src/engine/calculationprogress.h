#ifndef FLYSIGHT_ENGINE_CALCULATIONPROGRESS_H
#define FLYSIGHT_ENGINE_CALCULATIONPROGRESS_H

#include <QString>

namespace FlySight {

/// Thrown by a compute function to abandon its run after a cancellation
/// request (see CalculationProgress::throwIfCancelled()).
///
/// Deliberately NOT derived from std::exception: a compute function, or a
/// library underneath it, that catches std::exception cannot swallow a
/// cancellation by accident.
///
/// Only the asynchronous path (PreparedCalculation::compute()) has a way to
/// request cancellation. Thrown on the synchronous path it is an ordinary
/// non-standard exception and the result is cached as Failed.
class CalculationCancelled {};

/// What a long-running compute function may use besides its declared inputs:
/// a place to report progress text and a way to observe a cancellation request.
///
/// The facility is not an input. It cannot influence the result except by
/// abandoning it: a compute function that returns normally returns the same
/// bundle whatever was reported and whether or not cancellation was requested.
///
/// Implemented by whoever calls PreparedCalculation::compute() (the job queue).
/// Both virtual functions are called ON THE COMPUTE THREAD, possibly many times,
/// and must not throw; making them safe to call from there is the
/// implementer's business (the engine library itself holds no lock and no
/// atomic). The engine never calls them from any other thread.
class CalculationProgress {
public:
    virtual ~CalculationProgress();

    /// Latest human-readable progress text ("Optimizing, iteration 12").
    virtual void report(const QString &text) = 0;
    /// Whether the caller has asked the calculation to stop.
    virtual bool isCancelled() const = 0;

    /// Throws CalculationCancelled when isCancelled(). Compute functions call
    /// this at the boundaries where stopping is cheap.
    void throwIfCancelled() const;

    /// The facility of a run nobody watches: drops text, is never cancelled.
    /// Stateless, so one instance serves every thread.
    static CalculationProgress &none();
};

} // namespace FlySight

#endif // FLYSIGHT_ENGINE_CALCULATIONPROGRESS_H
