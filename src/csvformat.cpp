#include "csvformat.h"

#include <cmath>
#include <limits>

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QMetaType>
#include <QTime>

namespace FlySight {
namespace CsvFormat {

QByteArray formatDouble(double v)
{
    // Non-finite values: the literals are part of the file format, so they are
    // written here rather than left to Qt's spelling.
    if (std::isnan(v))
        return QByteArrayLiteral("nan");
    if (std::isinf(v))
        return v > 0 ? QByteArrayLiteral("inf") : QByteArrayLiteral("-inf");

    // Qt's formatter deliberately drops the sign of negative zero.
    if (v == 0.0 && std::signbit(v))
        return QByteArrayLiteral("-0");

    // double-conversion SHORTEST mode: the shortest digit string that parses
    // back to the same double.
    return QByteArray::number(v, 'g', QLocale::FloatingPointShortest);
}

bool parseDouble(QStringView text, double *out)
{
    double value = 0.0;
    bool ok = true;

    if (text == u"nan") {
        value = std::numeric_limits<double>::quiet_NaN();
    } else if (text == u"inf") {
        value = std::numeric_limits<double>::infinity();
    } else if (text == u"-inf") {
        value = -std::numeric_limits<double>::infinity();
    } else {
        value = text.toDouble(&ok);
    }

    if (ok && out)
        *out = value;
    return ok;
}

std::optional<QString> formatAttributeValue(const QVariant &value)
{
    if (!value.isValid())
        return std::nullopt;

    switch (value.typeId()) {
    case QMetaType::QString:
        return singleLine(value.toString());

    case QMetaType::Double:
    case QMetaType::Float:
        // A float is widened first; the text round-trips the widened double.
        return QString::fromLatin1(formatDouble(value.toDouble()));

    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::Short:
    case QMetaType::Long:
    case QMetaType::Char:
    case QMetaType::SChar:
        return QString::number(value.toLongLong());

    case QMetaType::UInt:
    case QMetaType::ULongLong:
    case QMetaType::UShort:
    case QMetaType::ULong:
    case QMetaType::UChar:
        return QString::number(value.toULongLong());

    case QMetaType::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");

    case QMetaType::QDateTime:
        return value.toDateTime().toUTC().toString(Qt::ISODateWithMs);
    case QMetaType::QDate:
        return value.toDate().toString(Qt::ISODate);
    case QMetaType::QTime:
        return value.toTime().toString(Qt::ISODateWithMs);

    case QMetaType::QByteArray:
        return singleLine(QString::fromUtf8(value.toByteArray()));

    default:
        break;
    }

    if (value.canConvert<QString>())
        return singleLine(value.toString());
    return std::nullopt;
}

QString singleLine(const QString &text)
{
    QString result;
    result.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\r')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n'))
                ++i;    // "\r\n" is one line break
            result.append(QLatin1Char(' '));
        } else if (c == QLatin1Char('\n') || c == QChar(0x2028) || c == QChar(0x2029)) {
            result.append(QLatin1Char(' '));
        } else {
            result.append(c);
        }
    }
    return result;
}

bool isValidUnit(const QString &text)
{
    return !text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('\r'))
        && !text.contains(QLatin1Char('\n'));
}

bool isValidName(const QString &text)
{
    return !text.isEmpty() && isValidUnit(text);
}

} // namespace CsvFormat
} // namespace FlySight
