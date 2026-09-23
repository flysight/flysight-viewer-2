#include "calculationresultstore.h"

#include <QElapsedTimer>
#include <QList>
#include <QtDebug>

#include "logbookmanager.h"

namespace FlySight {

namespace {

using RestoreOutcome = CalculationEngine::RestoreOutcome;

const char *staleCheckName(RestoreOutcome::StaleCheck check)
{
    switch (check) {
    case RestoreOutcome::StaleCheck::None:              return "none";
    case RestoreOutcome::StaleCheck::ResultVersion:     return "result version";
    case RestoreOutcome::StaleCheck::Bundle:            return "bundle";
    case RestoreOutcome::StaleCheck::InputsUnavailable: return "inputs unavailable";
    case RestoreOutcome::StaleCheck::Leaves:            return "leaves";
    case RestoreOutcome::StaleCheck::Fingerprint:       return "fingerprint";
    }
    return "unknown";
}

// Adds the elapsed time to a counter on every return of the scope.
class ScopedNanoseconds {
public:
    explicit ScopedNanoseconds(qint64 &sink) : m_sink(sink) { m_timer.start(); }
    ~ScopedNanoseconds() { m_sink += m_timer.nsecsElapsed(); }
    ScopedNanoseconds(const ScopedNanoseconds &) = delete;
    ScopedNanoseconds &operator=(const ScopedNanoseconds &) = delete;
private:
    qint64 &m_sink;
    QElapsedTimer m_timer;
};

} // namespace

void CalculationResultStore::onExplicitResultEvent(const QString &sessionId, const CalculationEngine &engine,
                                                   const CalculationEngine::ExplicitResultEvent &event)
{
    using Kind = CalculationEngine::ExplicitResultEvent::Kind;

    if (event.kind == Kind::Installed) {
        // Only an Ok result is stored. Any other install writes nothing and
        // deletes nothing: a record on disk describes an Ok result of the
        // same inputs, or is removed by the input change that made it stale.
        if (event.status != ResultStatus::Ok)
            return;

        ScopedNanoseconds timing(m_stats.writeNanoseconds);

        // nullopt when a deferred invalidation of the same engine call has
        // already dropped the result; its DroppedByInputChange event follows.
        const std::optional<StoredCalculationResult> snapshot = engine.exportResult(event.instanceId);
        if (!snapshot)
            return;

        // Every model row has a logbook identity (a stem the manager knows,
        // or one reserved at import), so no session-file check is made. A
        // failure has been warned about by the manager; the previous record
        // (if any) is intact and the in-memory result untouched. Never
        // retried: the next Ok publish of the pair tries again.
        QString error;
        if (LogbookManager::instance().writeCalculationRecord(sessionId, CalculationRecord::stamped(*snapshot), &error))
            ++m_stats.recordsWritten;
        else
            ++m_stats.writeFailures;
        return;
    }

    // DroppedByInputChange, whatever its status: a record, if any, describes
    // older inputs. The listing (names only) keeps the counter meaningful.
    const bool existed = LogbookManager::instance().calculationRecordIds(sessionId).contains(event.instanceId);
    deleteRecord(sessionId, event.instanceId, "an input changed");
    if (existed)
        ++m_stats.droppedRecordsDeleted;
}

CalculationResultStore::RestoreSummary CalculationResultStore::restoreSession(const QString &sessionId,
                                                                              CalculationEngine &engine)
{
    RestoreSummary summary;
    ScopedNanoseconds timing(m_stats.restoreNanoseconds);
    LogbookManager &logbook = LogbookManager::instance();

    // 1. The listing is the source of record ids (not the registry), so that
    //    a record of a calculation no longer registered is found and deleted.
    ++m_stats.restoreCalls;
    const QStringList ids = logbook.calculationRecordIds(sessionId);
    if (ids.isEmpty())
        return summary;

    const auto staleDelete = [&](const QString &calculationId, const char *why) {
        deleteRecord(sessionId, calculationId, why);
        ++summary.deleted;
    };

    // 2. Read and stamp check, in id order.
    QList<CalculationRecord> pending;
    for (const QString &id : ids) {
        CalculationRecordRead read = logbook.readCalculationRecord(sessionId, id);
        ++m_stats.recordsRead;
        switch (read.status) {
        case CalculationRecordStatus::Missing:
            break;      // vanished since the listing
        case CalculationRecordStatus::Ok:
            if (!read.record->stampsAreCurrent())
                staleDelete(id, "calculation compatibility or environment changed");
            else
                pending.append(std::move(*read.record));
            break;
        case CalculationRecordStatus::Unreadable:
        case CalculationRecordStatus::NotARecord:
        case CalculationRecordStatus::UnsupportedVersion:
        case CalculationRecordStatus::Corrupt:
            staleDelete(id, qPrintable(read.error));
            break;
        }
    }

    // 3. Restore in passes. A record whose inputs are unavailable may depend
    //    on another explicit result that a later record of this pass (or of
    //    the next one) restores, so it waits; it is stale only after a pass
    //    that restored nothing. Each pass that continues restores at least
    //    one record, so there are at most as many passes as records.
    while (!pending.isEmpty()) {
        QList<CalculationRecord> next;
        bool progressed = false;
        for (const CalculationRecord &record : std::as_const(pending)) {
            const RestoreOutcome outcome = engine.restoreResult(record.result);
            summary.invalidated.unite(outcome.invalidated);
            const QString &id = record.result.calculationId;
            switch (outcome.kind) {
            case RestoreOutcome::Kind::Restored:
                ++summary.restored;
                progressed = true;
                break;
            case RestoreOutcome::Kind::AlreadyInstalled:
                // A result installed some other way wins; its record stays.
                ++summary.kept;
                break;
            case RestoreOutcome::Kind::Stale:
                if (outcome.staleCheck == RestoreOutcome::StaleCheck::InputsUnavailable)
                    next.append(record);
                else
                    staleDelete(id, staleCheckName(outcome.staleCheck));
                break;
            case RestoreOutcome::Kind::NotFound:
            case RestoreOutcome::Kind::NotExplicit:
                // Current stamps make this unreachable in practice (the
                // environment fingerprint covers registrations); no record is
                // kept that can never be used.
                staleDelete(id, "calculation not registered as explicit");
                break;
            }
        }
        if (!progressed) {
            for (const CalculationRecord &record : std::as_const(next))
                staleDelete(record.result.calculationId, "inputs unavailable");
            break;
        }
        pending = std::move(next);
    }

    // 4.
    m_stats.recordsRestored += summary.restored;
    m_stats.recordsKept += summary.kept;
    m_stats.staleRecordsDeleted += summary.deleted;
    return summary;
}

bool CalculationResultStore::deleteRecord(const QString &sessionId, const QString &calculationId, const char *why)
{
    // A stale record or a dropped result is an expected state: qDebug only.
    // A removal failure has already been warned about by the manager.
    qDebug("CalculationResultStore: stored %s of %s dropped: %s",
           qPrintable(calculationId), qPrintable(sessionId), why);
    return LogbookManager::instance().removeCalculationRecord(sessionId, calculationId);
}

} // namespace FlySight
