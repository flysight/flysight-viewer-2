#ifndef SESSIONMERGE_H
#define SESSIONMERGE_H

#include <QPair>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVector>

#include "dependencykey.h"
#include "sessiondata.h"

namespace FlySight {

/// What merging one incoming file into one existing session would do. Built by
/// SessionMerge::plan(), which validates everything and mutates nothing, so
/// "validate before mutating" is structural: a plan that is not ok() is never
/// applied, and an ok() plan cannot fail.
struct MergePlan {
    QVector<QPair<QString, QVariant>> attributesToSet;   ///< key order; values exactly as the incoming file holds them
    SourceData                        columnsToSet;      ///< only columns that differ from the session's
    QString                           error;             ///< non-empty = the merge must not happen

    bool ok() const      { return error.isEmpty(); }
    bool isEmpty() const { return attributesToSet.isEmpty() && columnsToSet.isEmpty(); }

    /// The stored names the plan writes: attribute(k) and measurement(sensor, name).
    QSet<DependencyKey> changedKeys() const;
};

/// The merge rules. They work on stored attributes and source data only: no
/// effective value is ever read and no calculation engine is touched, because
/// copying effective values into the source layer would apply the conversion
/// layer twice.
///
/// Attributes (incoming key against the session):
///  - key absent in the session: added;
///  - header key (no leading '_'), equal value: nothing; different value: a
///    CONFLICT - the file is not merged;
///  - Viewer key (leading '_') present in the session: the session's value
///    wins, never a conflict;
///  - DEVICE_ID equal to SessionKeys::DeviceIdUnknown is a Viewer placeholder,
///    not a recorded fact: ignored when incoming, counts as absent when it is
///    the session's value.
/// Keys only the session has are untouched: absence is never a conflict.
///
/// Measurements: an incoming column replaces the same-named column of the same
/// sensor or is added, samples and unit text together; columns and sensors the
/// file does not mention are kept. A column identical to the session's is not
/// part of the plan, so re-importing an identical file yields an empty plan.
///
/// Ragged rule: a merge that would leave a sensor with columns of unequal
/// length is rejected like a conflict (such a sensor cannot be saved; replacing
/// the whole sensor would drop measurements, padding would invent samples).
namespace SessionMerge {

/// Never mutates either argument, never reads effective values, never touches
/// an engine. All attribute conflicts are reported in one error, sorted by
/// key; the ragged check runs only when there is no conflict.
MergePlan plan(const SessionData &existing, const SessionData &incoming);

/// Applies an ok() plan. Cannot fail. Returns the union of the invalidation sets.
QSet<DependencyKey> apply(SessionData &existing, const MergePlan &plan);

/// Equality of the text that is, or would be, on disk
/// (CsvFormat::formatAttributeValue): exact, untrimmed, type-independent, so a
/// session compares equal to its own saved-and-reloaded form. Two
/// unrepresentable values are equal to each other.
bool sameAttributeValue(const QVariant &a, const QVariant &b);

/// Same unit text, same size, and bitwise-equal samples (NaN equals NaN, 0.0
/// differs from -0.0).
bool sameColumn(const SourceColumn &a, const SourceColumn &b);

} // namespace SessionMerge
} // namespace FlySight

#endif // SESSIONMERGE_H
