#ifndef CALCULATIONRECORD_H
#define CALCULATIONRECORD_H

#include <optional>
#include <utility>

#include <QByteArray>
#include <QString>
#include <QStringView>

#include "engine/storedcalculationresult.h"

namespace FlySight {

/// One stored requested-calculation result, as a record file holds it: the
/// engine's snapshot plus the code stamp that was current when it was
/// written. The calculation environment fingerprint is not part of a record:
/// what else is registered never makes a stored result stale. The logbook
/// manager does all record I/O; this header only defines the value, its file
/// name and its bytes.
struct CalculationRecord {
    int calculationCompatibility = 0;   ///< CalculationCompatibilityVersion at write time
    StoredCalculationResult result;     ///< the engine's snapshot, with its resolutions

    /// The snapshot with the CURRENT code stamp.
    static CalculationRecord stamped(const StoredCalculationResult &result);
    /// calculationCompatibility == CalculationCompatibilityVersion. The
    /// calculation's own result version and the lookups (what provided each
    /// name, with its result version) are checked by the engine when it
    /// restores the snapshot; nothing else is.
    bool stampsAreCurrent() const;
};

/// Outcome of reading / decoding one record.
enum class CalculationRecordStatus {
    Ok,
    Missing,             ///< no record file (or the session is not in the logbook)
    Unreadable,          ///< the file (or whatever is at its path) exists but could not be opened or
                         ///< read in full; the result store skips it: kept, not restored
    NotARecord,          ///< does not start with the magic
    UnsupportedVersion,  ///< format version other than CalculationRecordFormatVersion
    Corrupt              ///< checksum, structure, trailing bytes, wrong calculation id
};

/// The format version this program writes and the only one it reads.
/// Changing anything after the version field, including the set of attribute
/// types a record can hold, bumps it. History:
///   1 - first format (the environment fingerprint as a second stamp);
///   2 - the environment fingerprint left the record; the resolutions are
///       section 10.
/// A record of any other version is UnsupportedVersion and is deleted as stale
/// when its session loads; there is no migration.
inline constexpr quint32 CalculationRecordFormatVersion = 2;

// ---------------------------------------------------------------- file names
//
// A record of session file stem S (the <uuid> of sessions/<uuid>.csv) and
// calculation id I lives in the logbook's cache/ folder and is named
//
//     S + "." + encodeRecordFileId(I) + "." + calculationRecordExtension()
//
// e.g. "3f2c...-9a1e.builtin%2Efusion%2Efit.fvresult". The encoded id holds no
// '.', so the name splits unambiguously at its last two dots even when the
// stem contains dots. A record is never in sessions/, and never ends in
// ".csv", nor does its QSaveFile temporary ("<name>.XXXXXX"), so the
// logbook's session scan (sessions/*.csv) can never take one for a session.

/// The record file extension, without the dot ("fvresult").
QString calculationRecordExtension();

/// Percent-encodes the UTF-8 bytes of `id`: a byte in [a-z0-9_-] is written
/// as itself, every other byte ('.', '#', '%', upper-case letters, path and
/// shell characters, non-ASCII bytes) as '%' plus two upper-case hex digits.
/// The result contains no '.', only characters valid in a file name on every
/// platform, and is injective under case folding (a literal letter is always
/// lower case, an escape always upper-case hex), so two ids never collide on
/// a case-insensitive file system.
QString encodeRecordFileId(const QString &id);

/// The inverse of encodeRecordFileId(). nullopt unless `text` is non-empty,
/// holds only [a-z0-9_-] and %XX escapes (upper-case hex), decodes to valid
/// UTF-8, and re-encodes to exactly `text` (only the canonical form is
/// accepted).
std::optional<QString> decodeRecordFileId(QStringView text);

/// stem + "." + encodeRecordFileId(calculationId) + "." + extension.
QString recordFileName(const QString &stem, const QString &calculationId);

/// (stem, calculation id) of a record file name. The extension is compared
/// case-sensitively, on every platform: a name that spells it otherwise
/// ("X.FVRESULT") is not a record. The rest splits at its last '.'. The stem
/// must be non-empty and the id must decode. nullopt for anything else.
std::optional<std::pair<QString, QString>> parseRecordFileName(QStringView fileName);

// ---------------------------------------------------------------- the bytes
//
// Format version 2. After the 8-byte magic "FVRESULT" everything is written
// with one QDataStream pinned to Qt_6_0, little-endian, double precision (a
// double is its 8 IEEE-754 bytes; a QString is a quint32 byte length,
// 0xFFFFFFFF for a null string, and UTF-16LE code units):
//
//    1  magic                       8 raw bytes "FVRESULT"
//    2  format version              quint32 (2)
//    3  calculation compatibility   qint32
//    4  calculation id              QString, non-empty
//    5  result version              QString
//    6  reason / detail             QString (the bundle's reason(); written once)
//    7  input fingerprint           QByteArray, InputFingerprintSize bytes
//    8  leaf count                  quint32
//   8a  per leaf                    quint8 storedLeafKindCode, QString a, QString b
//    9  output count                quint32
//   9a  per output (setOutputs()    quint8 key code (1 attribute, 2 measurement),
//       order)                      QString first (key or sensor), QString second
//                                   (measurement name; null for an attribute),
//                                   bool available, then only if available:
//                                   attribute: QVariant (QDataStream's own form);
//                                   measurement: quint32 count, count doubles,
//                                   QString unit
//   10  resolution count            quint32
//  10a  per resolution (snapshot    quint8 name code (1 attribute, 2 measurement: the output
//       order)                      key codes), QString first (key or sensor), QString second
//                                   (measurement name; null for an attribute), quint8
//                                   storedResolutionProviderCode, QString instance id,
//                                   QString result version (both as held: empty/null unless
//                                   Calculation)
//   11  checksum                    32 raw bytes: SHA-256 of every preceding byte
//
// The status is not stored: only Ok results are recorded. An available
// attribute holds one of these QMetaType types (the portable non-date types
// of CsvFormat::formatAttributeValue), each of which round-trips to the same
// type and the same bits on every platform:
//
//   QString, QByteArray, Bool, Double, Float, Int, UInt, LongLong, ULongLong,
//   Short, UShort, Char, SChar, UChar
//
// Float is written as a double (DoublePrecision) and read back as the same
// float. Anything else is refused on both sides, in particular Long and ULong:
// Qt streams them as 64-bit integers but `long` is 32 bits wide on Windows
// and 64 on Linux / macOS, so a value could not come back everywhere.
//
// Pure functions: no file I/O, no engine, no global state. Safe on any thread.

/// The record's bytes, or nullopt (with *error set) when the calculation id is
/// empty, an available attribute has a type a record cannot hold, a sample
/// count is >= 0xFFFFFFFE, or the stream fails. *error is a lower-case phrase,
/// like the decoder's: the logbook manager puts it after "not written: ".
std::optional<QByteArray> encodeCalculationRecord(const CalculationRecord &record, QString *error = nullptr);

/// Decodes `bytes`. Checks, in order: the magic (NotARecord), the format
/// version (UnsupportedVersion, before the checksum, so a future format may
/// change everything after the version field), the checksum and the
/// structure (Corrupt). *out receives the record only for Ok and is untouched
/// otherwise; *error gets a short fixed text for every other status.
CalculationRecordStatus decodeCalculationRecord(const QByteArray &bytes, CalculationRecord *out,
                                                QString *error = nullptr);

} // namespace FlySight

#endif // CALCULATIONRECORD_H
