#include "calculationresultstore.h"

#include <utility>

#include <QElapsedTimer>
#include <QList>
#include <QSet>
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
    case RestoreOutcome::StaleCheck::Resolutions:       return "resolutions";
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

// True when `record` names, as a Calculation provider of a looked-up name,
// one of `ids` other than itself.
bool namesRecordIn(const CalculationRecord &record, const QSet<QString> &ids)
{
    for (const StoredResolution &r : record.result.resolutions) {
        if (r.provider == StoredResolution::Provider::Calculation && r.instanceId != record.result.calculationId
            && ids.contains(r.instanceId))
            return true;
    }
    return false;
}

} // namespace

void CalculationResultStore::onExplicitResultEvent(const QString &sessionId, const CalculationEngine &engine,
                                                   const CalculationEngine::ExplicitResultEvent &event)
{
    using Kind = CalculationEngine::ExplicitResultEvent::Kind;

    // An explicit family instance ("<familyId>#<instanceKey>"; a plain id
    // never contains '#', calctypes.h): exportResult() refuses it, so there is
    // never a record to write or to delete. Nothing is looked up on disk.
    if (event.instanceId.contains(QLatin1Char('#')))
        return;

    if (event.kind == Kind::Installed) {
        // Only an Ok result is stored. Any other install writes nothing and
        // deletes nothing: a record on disk describes an Ok result of the
        // same inputs, or is removed by the input change that made it stale.
        if (event.status != ResultStatus::Ok)
            return;

        ScopedNanoseconds timing(m_stats.writeNanoseconds);

        // nullopt when a deferred invalidation of the same engine call has
        // already dropped the result; its Dropped event follows.
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

    // Dropped, whatever its status: an input of the result, or a registry
    // change made while the application runs, reached it; a record, if any,
    // no longer describes it. No listing: the removal looks at the one path,
    // and tells whether a file was there.
    if (deleteRecord(sessionId, event.instanceId, "an input or the registry changed"))
        ++m_stats.droppedRecordsDeleted;
}

CalculationResultStore::RestoreSummary CalculationResultStore::restoreSession(const QString &sessionId,
                                                                              CalculationEngine &engine)
{
    RestoreSummary summary;
    ScopedNanoseconds timing(m_stats.restoreNanoseconds);
    LogbookManager &logbook = LogbookManager::instance();

    // 1. A session the manager knows no record of has none to restore, and
    //    the cache/ folder is not listed. The known set holds every record file
    //    the application can have produced: initialize() adopts the name of
    //    every record whose stem is a session file of the index (after its
    //    stray pass), the manager's own writes and removals keep it current,
    //    and remapSessionId() moves it with the id. A stem reserved at import
    //    is a fresh uuid, so its files are all the manager's own writes. Only
    //    a file put there behind the application's back is missing from it,
    //    until the next initialize() (the same rule as the record stamps).
    //    When the manager knows some record, the ids to read are the union of
    //    the listing (names only) and the known set, never the registry. The
    //    listing finds records the known set does not (a calculation no longer
    //    registered, a file put there behind the application's back while
    //    another record was known); the known set finds a record the listing
    //    cannot see because something other than a file stands at its path (a
    //    directory), which must be read - and found Unreadable - rather than
    //    silently ignored. A known id with nothing at its path reads Missing
    //    and is passed over.
    ++m_stats.restoreCalls;
    const QSet<QString> known = logbook.knownCalculationRecords(sessionId);
    if (known.isEmpty())
        return summary;
    ++m_stats.recordListings;
    QSet<QString> idSet = known;
    for (const QString &id : logbook.calculationRecordIds(sessionId))
        idSet.insert(id);
    QStringList ids(idSet.cbegin(), idSet.cend());
    ids.sort();

    const auto staleDelete = [&](const QString &calculationId, const char *why) {
        deleteRecord(sessionId, calculationId, why);
        ++summary.deleted;
    };

    // A record kept for the next load: not restored, not deleted, and the
    // manager keeps the column values over it out of index.json while the
    // loaded engine does not hold what it holds.
    QSet<QString> skipped;
    const auto skip = [&](const QString &calculationId, const QString &reason) {
        logbook.markCalculationRecordSkipped(sessionId, calculationId);
        skipped.insert(calculationId);
        ++summary.skipped;
        qWarning("CalculationResultStore: stored %s of %s skipped (kept for the next load): %s",
                 qPrintable(calculationId), qPrintable(sessionId), qPrintable(reason));
    };

    // 2. Read and stamp check, in id order.
    QList<CalculationRecord> pending;
    for (const QString &id : ids) {
        CalculationRecordRead read = logbook.readCalculationRecord(sessionId, id);
        ++m_stats.recordsRead;
        switch (read.status) {
        case CalculationRecordStatus::Missing:
            break;      // vanished since the listing, or a known id with nothing at its path
        case CalculationRecordStatus::Ok:
            if (!read.record->stampsAreCurrent())
                staleDelete(id, "calculation compatibility changed");
            else
                pending.append(std::move(*read.record));
            break;
        case CalculationRecordStatus::Unreadable:
            // A transient failure to read (a lock, a permission) is not
            // evidence that the record is wrong.
            skip(id, read.error);
            break;
        case CalculationRecordStatus::NotARecord:
        case CalculationRecordStatus::UnsupportedVersion:
        case CalculationRecordStatus::Corrupt:
            staleDelete(id, qPrintable(read.error));
            break;
        }
    }

    // 3. Restore in passes, upstream records first. A record's resolutions
    //    name, as a Calculation provider, every calculation its lookups went
    //    through, explicit results included. A record that names another
    //    pending record waits until that one has been restored or dropped:
    //    restored before it, the downstream result would resolve as though
    //    its upstream were not requested (to a fallback candidate, when there
    //    is one) and be refused as stale. A record that names a skipped
    //    record is skipped too, whatever its own checks would say: deleting it
    //    would destroy a good record because of another file's transient
    //    failure. Rings are never exported, so records cannot wait for each
    //    other in a circle; should they, the pass tries them all.
    //    A record whose inputs are unavailable although it waits for nothing
    //    (a dependency its resolutions do not show) waits for the next pass;
    //    it is stale only after a pass that changed nothing else. Every pass
    //    removes at least one record from the pending list, so there are at
    //    most as many passes as records.
    while (!pending.isEmpty()) {
        // a. Readers of a skipped record, transitively
        bool skippedAny = true;
        while (skippedAny) {
            skippedAny = false;
            QList<CalculationRecord> rest;
            for (CalculationRecord &record : pending) {
                if (namesRecordIn(record, skipped)) {
                    skip(record.result.calculationId,
                         QStringLiteral("it reads a stored result that could not be read"));
                    skippedAny = true;
                } else {
                    rest.append(std::move(record));
                }
            }
            pending = std::move(rest);
        }

        // b. Held back: records that name another pending record
        QSet<QString> pendingIds;
        for (const CalculationRecord &record : std::as_const(pending))
            pendingIds.insert(record.result.calculationId);
        QList<CalculationRecord> ready;
        QList<CalculationRecord> held;
        for (CalculationRecord &record : pending)
            (namesRecordIn(record, pendingIds) ? held : ready).append(std::move(record));
        if (ready.isEmpty())
            std::swap(ready, held);     // a cycle, which export rules out

        // c. The ready ones
        QList<CalculationRecord> unavailable;
        for (const CalculationRecord &record : std::as_const(ready)) {
            const RestoreOutcome outcome = engine.restoreResult(record.result);
            summary.invalidated.unite(outcome.invalidated);
            const QString &id = record.result.calculationId;
            switch (outcome.kind) {
            case RestoreOutcome::Kind::Restored:
                ++summary.restored;
                break;
            case RestoreOutcome::Kind::AlreadyInstalled:
                // A result installed some other way wins; its record stays.
                ++summary.kept;
                break;
            case RestoreOutcome::Kind::Stale:
                if (outcome.staleCheck == RestoreOutcome::StaleCheck::InputsUnavailable)
                    unavailable.append(record);
                else
                    staleDelete(id, staleCheckName(outcome.staleCheck));
                break;
            case RestoreOutcome::Kind::NotFound:
            case RestoreOutcome::Kind::NotExplicit:
                // The calculation is not registered, or not as an explicit one
                // (a plug-in calculation removed, an id re-registered on
                // demand): no record is kept that can never be used.
                staleDelete(id, "calculation not registered as explicit");
                break;
            }
        }

        // d. Nothing but unavailable inputs in this pass: nothing can make
        //    them available any more. The held records are tried next.
        if (unavailable.size() == ready.size()) {
            for (const CalculationRecord &record : std::as_const(unavailable))
                staleDelete(record.result.calculationId, "inputs unavailable");
            unavailable.clear();
        }
        pending = std::move(held);
        pending.append(std::move(unavailable));
    }

    // 4. The counters
    m_stats.recordsRestored += summary.restored;
    m_stats.recordsKept += summary.kept;
    m_stats.staleRecordsDeleted += summary.deleted;
    m_stats.recordsSkipped += summary.skipped;
    return summary;
}

bool CalculationResultStore::deleteRecord(const QString &sessionId, const QString &calculationId, const char *why)
{
    // A stale record or a dropped result is an expected state: qDebug only,
    // and only when a file went. A removal failure has already been warned
    // about by the manager.
    bool removed = false;
    LogbookManager::instance().removeCalculationRecord(sessionId, calculationId, &removed);
    if (removed) {
        qDebug("CalculationResultStore: stored %s of %s dropped: %s",
               qPrintable(calculationId), qPrintable(sessionId), why);
    }
    return removed;
}

} // namespace FlySight
