#include "calculationprogress.h"

namespace FlySight {

namespace {

/// No state at all: safe to share between threads without synchronization.
class NoProgress final : public CalculationProgress {
public:
    void report(const QString &) override {}
    bool isCancelled() const override { return false; }
};

} // namespace

CalculationProgress::~CalculationProgress() = default;

void CalculationProgress::throwIfCancelled() const
{
    if (isCancelled())
        throw CalculationCancelled();
}

CalculationProgress &CalculationProgress::none()
{
    // Initialization of a function-local static is thread-safe (C++11), and
    // the object has no members to race on afterwards.
    static NoProgress instance;
    return instance;
}

} // namespace FlySight
