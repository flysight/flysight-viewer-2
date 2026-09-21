#ifndef FLYSIGHT_ENGINE_BLOCKERREPORT_H
#define FLYSIGHT_ENGINE_BLOCKERREPORT_H

#include <optional>

#include <QList>
#include <QString>

#include "calctypes.h"

// Result types of blocker inspection (CalculationEngine::blockers() and
// readiness()): which explicit calculations stand between a public name and its
// availability. Plain values; produced and consumed on the main thread.

namespace FlySight {

/// One explicit calculation instance, with enough identity to pass to
/// CalculationEngine::prepare() / request() and enough text to label a job.
struct CalculationBlocker {
    CalculationId registrationId;
    /// Selects the family instance; the empty name (isEmptyName) for a plain
    /// calculation. Pass it as the second argument of prepare() / request().
    DependencyKey instanceOutput = DependencyKey::attribute(QString());
    QString instanceId;
    QString title;                  ///< the descriptor's title, else the instance id
};

/// "Ran and did not produce": an explicit calculation that has a valid result
/// which does not make the inspected name available. Requesting it again with
/// the same inputs would give the same answer.
struct UnproducedNote {
    CalculationBlocker calculation;
    /// Ok (the run reported the output unavailable), Failed, UndeclaredRead,
    /// InvalidOutput, or Cycle.
    ResultStatus status = ResultStatus::Ok;
    /// The result's reason (CalculationResult::setReason) for a clean run, the
    /// exception text for Failed; may be empty.
    QString detail;
};

/// Why a public name is or is not available, as far as explicit calculations
/// are concerned.
struct BlockerReport {
    enum class State {
        Available,      ///< an ordinary read returns a value
        Blocked,        ///< requesting `blockers` can make progress towards it
        NotProduced,    ///< an explicit calculation ran and did not produce it
        NotApplicable   ///< unavailable for ordinary reasons (no data, missing input, unknown name)
    };
    State state = State::NotApplicable;
    /// Non-empty iff Blocked. Unique by instanceId, in discovery order
    /// (candidate order, then declared input order). These are the calculations
    /// to request NOW; once they publish, inspect again (chained calculations).
    QList<CalculationBlocker> blockers;
    /// Non-empty when NotProduced; may also be non-empty when Blocked.
    QList<UnproducedNote> notProduced;
};

/// Where one calculation instance stands. Availability of its inputs is decided
/// before its policy is looked at.
struct CalculationReadiness {
    enum class State {
        Unknown,        ///< no such calculation / the name is not of that family
        MissingInput,   ///< an input is unavailable and no request could change that from here
        Blocked,        ///< every unavailable input is waiting for the calculations in `blockers`
        Ready,          ///< explicit, every input available, no valid result: prepare() would run it
        Done            ///< every input available and nothing to request (valid result, or on demand)
    };
    State state = State::Unknown;
    QList<CalculationBlocker> blockers;     ///< Blocked only
    std::optional<ResultStatus> status;     ///< Done: the cached status, when there is one
};

} // namespace FlySight

#endif // FLYSIGHT_ENGINE_BLOCKERREPORT_H
