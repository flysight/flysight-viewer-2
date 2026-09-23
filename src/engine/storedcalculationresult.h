#ifndef FLYSIGHT_ENGINE_STOREDCALCULATIONRESULT_H
#define FLYSIGHT_ENGINE_STOREDCALCULATIONRESULT_H

#include <optional>

#include <QByteArray>
#include <QList>
#include <QString>

#include "calctypes.h"
#include "calculationresult.h"
#include "sessionstate.h"

namespace FlySight {

/// The installed result of one plain explicit calculation, as a plain value:
/// what CalculationEngine::exportResult() hands out and restoreResult() takes
/// back. The engine and the record format that stores it beside the session
/// exchange exactly this.
///
/// Members:
///  - calculationId: the plain registration id (equal to the instance id;
///    families are never exported).
///  - resultVersion: the descriptor's resultVersion when the result was
///    published; may be empty. A restore refuses a different one.
///  - detail: the text resultDetail() reports. Always equal to
///    bundle.reason(); a restore refuses a snapshot where it is not.
///  - bundle: the installed bundle. Only an Ok result is exported, so there is
///    no status member.
///  - leaves: every StoredAttribute, SourceMeasurement, SourceUnit and
///    Preference node the result reached through the recorded edges, directly
///    or through any number of Resolution and Result nodes, the ones that were
///    looked at and found absent included. Sorted by storedLeafLess(), unique.
///    `b` is empty for attribute and preference leaves; `measurementName` is
///    always false.
///  - inputFingerprint: inputFingerprint() of `leaves` over the state and
///    preferences at export, InputFingerprintSize raw bytes.
///
/// Reading and rebuilding the bundle. A record reads the bundle through its
/// public API: setOutputs() for the order, then per output isAvailable(),
/// attributeValue() or measurementValues() / measurementUnit(), and reason().
/// It rebuilds it in the same order with setAttribute() / setMeasurement(
/// values, unit) for an available output and setUnavailable() for an
/// unavailable one, then setReason(detail). That round-trips exactly, because
/// CalculationResult normalizes on the way in: an output is available exactly
/// when its QVariant is valid / its sample vector is non-empty.
///
/// The code stamps (the calculation-compatibility marker and the calculation
/// environment fingerprint) are deliberately absent: the engine layer does
/// not depend on the built-in calculations that define them. Whoever stores a
/// snapshot adds them.
///
/// Pinned. The leaf kind codes (storedLeafKindCode) and the fingerprint
/// encoding (inputFingerprintEncoding) are part of what is stored on disk and
/// must never change meaning. A different encoding gets a new magic
/// ("flysight-inputs-v2"): every stored fingerprint then mismatches, which is
/// conservative - stored results go stale, nothing is misread.
struct StoredCalculationResult {
    CalculationId      calculationId;
    QString            resultVersion;
    QString            detail;
    CalculationResult  bundle;
    QList<GraphNode>   leaves;
    QByteArray         inputFingerprint;
};

/// Length of StoredCalculationResult::inputFingerprint: a raw SHA-256 digest.
inline constexpr int InputFingerprintSize = 32;

/// StoredAttribute, SourceMeasurement, SourceUnit or Preference.
bool isStoredLeafKind(GraphNode::Kind kind);
/// 0 StoredAttribute, 1 SourceMeasurement, 2 SourceUnit, 3 Preference. Pinned:
/// part of the fingerprint encoding and of the record format, and not tied to
/// the enum's underlying values. Asserts on any other kind and returns 0xFF.
quint8 storedLeafKindCode(GraphNode::Kind kind);
/// The leaf for a code read back from a record (`b` is taken as given for
/// every kind); nullopt for an unknown code.
std::optional<GraphNode> storedLeafFromCode(quint8 code, const QString &a, const QString &b);
/// The order of StoredCalculationResult::leaves: by kind code, then a, then b,
/// each compared with QString::compare (case-sensitive, UTF-16 code units).
bool storedLeafLess(const GraphNode &lhs, const GraphNode &rhs);

/// The canonical byte string behind the input fingerprint (version 1), for the
/// leaves in the order given. All integers little-endian; str(s) is a u32 byte
/// count followed by s.toUtf8(), no terminator.
///
///   magic       the 18 ASCII bytes "flysight-inputs-v1" and one 0x00 byte
///   count       u32, the number of leaves
///   per leaf    u8 storedLeafKindCode, str(a), str(b), u8 present (1 / 0),
///               and only when present the value:
///     StoredAttribute   present = hasStoredAttribute(a). t = the session
///                       file's text (CsvFormat::formatAttributeValue); u8 1 +
///                       str(t), or u8 0 when the value has no text (an
///                       invalid or unrepresentable value). So an attribute
///                       that reloads as text gives the bytes of the number
///                       it was set as.
///     SourceMeasurement present = hasSourceMeasurement(a, b). u64 sample
///                       count, then each sample's u64 IEEE-754 bit pattern;
///                       every NaN is written 0x7FF8000000000000, -0.0 and the
///                       infinities keep their bits.
///     SourceUnit        present = hasSourceMeasurement(a, b) (the unit exists
///                       exactly when the measurement does). str(sourceUnit).
///     Preference        v = preferences ? preferenceValue(a) : invalid;
///                       present = v.isValid(). As for an attribute: u8 1 +
///                       str(text), or u8 0. The text form the environment
///                       fingerprint uses for preferences.
///
/// A pure read of `state` and `preferences`, following ISessionState's contract.
/// Exposed for the known-answer test.
QByteArray inputFingerprintEncoding(const QList<GraphNode> &leaves, const ISessionState &state,
                                    const IPreferenceProvider *preferences);
/// SHA-256 of inputFingerprintEncoding(): InputFingerprintSize raw bytes.
QByteArray inputFingerprint(const QList<GraphNode> &leaves, const ISessionState &state,
                            const IPreferenceProvider *preferences);

/// Every member equal: id, version, detail, leaves, fingerprint bytes; the
/// bundle's setOutputs() order and reason; per output availability, attribute
/// (QVariant ==), samples by bit pattern (NaN == NaN, -0 != +0) and unit. The
/// rule of CalculationEngine::sameValue(), applied to every output.
bool sameContent(const StoredCalculationResult &a, const StoredCalculationResult &b);

} // namespace FlySight

#endif // FLYSIGHT_ENGINE_STOREDCALCULATIONRESULT_H
