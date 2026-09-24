#ifndef CALCULATIONRESULTSTORE_H
#define CALCULATIONRESULTSTORE_H

#include <QSet>
#include <QString>

#include "calculationrecord.h"
#include "engine/calculationengine.h"

namespace FlySight {

/// Stored results of explicitly requested calculations (the spec
/// "store-requested-calculations"): the rules between a session's engine and
/// its record files in the logbook's cache/ folder. Main thread only.
/// SessionModel owns one and is its only product caller. Tests may drive it
/// directly.
///
/// It holds no state but its counters: every record lives on disk, and all
/// record I/O goes through LogbookManager.
///
///  - An explicit result installed with status Ok (a job's publish or a
///    synchronous request) is exported and written as a record. An install
///    with any other status writes nothing and deletes nothing.
///  - A result dropped by an input change, or by a registry change made while
///    the application runs, deletes its record (both arrive as
///    DroppedByInputChange). A teardown removal reports nothing.
///  - restoreSession() installs a session's valid records into its engine
///    and deletes the stale ones. It is not a request: it starts nothing.
///  - A record that exists but cannot be read (Unreadable) is skipped:
///    neither restored nor deleted; the calculation reads as not requested and
///    the logbook manager keeps the column values over it out of index.json
///    (LogbookManager::markCalculationRecordSkipped()). A record whose inputs
///    stay unavailable only because it reads the result of a skipped record is
///    skipped too. The next load tries again.
///  - Explicit family instances ("<familyId>#<key>") are not stored: their
///    events are ignored (CalculationEngine::exportResult() refuses them).
class CalculationResultStore {
public:
    struct Stats {
        int recordsWritten = 0;         ///< Ok installs whose record was committed
        int writeFailures = 0;          ///< Ok installs whose record could not be encoded or written
        int restoreCalls = 0;           ///< restoreSession() calls
        int recordListings = 0;         ///< ... that listed the session's record files (the manager knew some)
        int recordsRead = 0;            ///< records read by restoreSession() (listed or known ids)
        int recordsRestored = 0;        ///< ... installed into the engine
        int recordsKept = 0;            ///< ... not installed because a result was already installed (AlreadyInstalled)
        int staleRecordsDeleted = 0;    ///< deleted by restoreSession(): not a record, damaged, another format
                                        ///< version, another compatibility marker, stale (RestoreOutcome),
                                        ///< unknown calculation
        int recordsSkipped = 0;         ///< kept, not restored: could not be read, or read the result of one that could not
        int droppedRecordsDeleted = 0;  ///< deleted because an input or registry change dropped the in-memory result
        qint64 restoreNanoseconds = 0;  ///< wall time inside restoreSession() (listing, reading, checks, restore)
        qint64 writeNanoseconds = 0;    ///< wall time exporting, stamping, encoding and writing records
    };

    struct RestoreSummary {
        int restored = 0;
        int kept = 0;
        int deleted = 0;
        int skipped = 0;
        QSet<DependencyKey> invalidated;   ///< union of RestoreOutcome::invalidated over every restoreResult() call
    };

    /// The engine's explicit-result listener, for the engine of session `sessionId`.
    /// May be called only from that listener (it calls engine.exportResult()).
    void onExplicitResultEvent(const QString &sessionId, const CalculationEngine &engine,
                               const CalculationEngine::ExplicitResultEvent &event);

    /// Installs every valid record of `sessionId` into `engine` and deletes
    /// the stale ones. Call it once, when the session has just been installed
    /// into a model row and before the row is published to readers. Never
    /// from inside an evaluation or an engine callback, never for a temporary
    /// load. Starts nothing.
    RestoreSummary restoreSession(const QString &sessionId, CalculationEngine &engine);

    const Stats &stats() const { return m_stats; }
    void resetStats() { m_stats = Stats(); }

private:
    /// Removes the record through the manager; true when a record file was
    /// removed (false when there was none or its removal failed). Counts nothing.
    bool deleteRecord(const QString &sessionId, const QString &calculationId, const char *why);
    Stats m_stats;
};

} // namespace FlySight

#endif // CALCULATIONRESULTSTORE_H
