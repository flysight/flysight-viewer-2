#ifndef CSVFORMAT_H
#define CSVFORMAT_H

#include <optional>

#include <QByteArray>
#include <QString>
#include <QStringView>
#include <QVariant>

namespace FlySight {

/// The one place where numbers and attribute values become text in a session
/// file, and where that text becomes a number again. DataExporter,
/// DataImporter, and the column-cache environment fingerprint all use it, so
/// the writer and the reader cannot drift apart.
///
/// Numbers. A double is written as the SHORTEST decimal text that parses back
/// to the same bit pattern ("0.1", "45.1234567", "1718900000.123", "1e-320"),
/// so a value survives any number of save / load cycles exactly. Special cases:
///  - negative zero is written as "-0" (it would otherwise lose its sign);
///  - non-finite values are written as exactly "nan", "inf", "-inf". A NaN's
///    sign and payload are not preserved: one quiet NaN comes back.
///
/// Attribute values. Every form formatAttributeValue() produces is read back
/// by the importer as a QString equal to the text written ($VAR values are the
/// verbatim remainder of the line and are never re-typed), and a QString is
/// written verbatim. Therefore save -> load -> save is byte-identical for any
/// attribute map, and toDouble() of the reloaded text returns the original
/// bits for every numeric attribute.
///
/// Line breaks cannot be represented in a line format. They are replaced by
/// one space (singleLine): escaping would alter recorded values that happen to
/// contain the escape sequence, and refusing to save would lose a whole
/// session for one odd character. Commas need nothing in values: the importer
/// takes everything after the second comma of a $VAR line.
namespace CsvFormat {

/// Shortest decimal text that parseDouble() maps back to the same bit pattern.
/// Non-finite: exactly "nan", "inf", "-inf".
QByteArray formatDouble(double v);

/// Inverse used by the importer for every non-timestamp numeric field. Accepts
/// the three canonical non-finite tokens by exact comparison first, then
/// whatever QStringView::toDouble accepts (correctly rounded; "NaN", "+inf",
/// and - because toDouble itself ignores it - surrounding whitespace). This
/// function adds no trimming and no stricter check of its own, so every field
/// that loaded before it existed still loads. *out is untouched on failure.
bool parseDouble(QStringView text, double *out);

/// Text written after "$VAR,<key>,". nullopt = the value cannot be represented
/// (invalid QVariant, unconvertible type); the caller skips it and warns.
/// An invalid value means "unavailable"; "" would reload as an available
/// empty string, so it is not used for it.
std::optional<QString> formatAttributeValue(const QVariant &value);

/// Replaces every "\r\n", '\r', '\n' (and U+2028 / U+2029) by one space.
/// Nothing else changes.
QString singleLine(const QString &text);

/// True iff text is usable as a $VAR key, sensor name, or column label:
/// non-empty, contains no ',', '\r', '\n'.
bool isValidName(const QString &text);
/// Same without the non-empty requirement (unit text may be empty).
bool isValidUnit(const QString &text);

} // namespace CsvFormat
} // namespace FlySight

#endif // CSVFORMAT_H
