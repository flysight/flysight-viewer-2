#include "calculationrecord.h"

#include <cstring>

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QMetaType>
#include <QStringDecoder>
#include <QVariant>
#include <QVector>
#include <QtEndian>

namespace FlySight {

namespace {

// The only spelling of the extension and of the magic in the sources: every
// other file asks calculationRecordExtension() or the name functions.
const char kExtension[] = "fvresult";
const char kMagic[] = "FVRESULT";
constexpr int kMagicSize = 8;                       // the magic without its terminator
constexpr int kPrefixSize = kMagicSize + 4;         // magic + quint32 format version
constexpr int kChecksumSize = 32;                   // SHA-256

// Output key codes: the record's own stable numbers, not a cast of
// DependencyKey::Type.
constexpr quint8 kAttributeCode = 1;
constexpr quint8 kMeasurementCode = 2;

// Smallest possible entries, for the count checks before anything is
// allocated: a leaf is a code and two string lengths, an output a code, two
// string lengths and the availability byte.
constexpr qint64 kMinLeafBytes = 1 + 4 + 4;
constexpr qint64 kMinOutputBytes = 1 + 4 + 4 + 1;
constexpr qint64 kSampleBytes = 8;

// Qt reserves 0xFFFFFFFE and 0xFFFFFFFF as size markers.
constexpr quint64 kMaxSampleCount = 0xFFFFFFFDu;

void pinStream(QDataStream &stream)
{
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}

/// The attribute types a record can hold: the non-date types
/// CsvFormat::formatAttributeValue handles. Widening the set is a format
/// change (bump CalculationRecordFormatVersion).
bool isRecordableAttributeType(int typeId)
{
    switch (typeId) {
    case QMetaType::QString:
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::Short:
    case QMetaType::Long:
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::UInt:
    case QMetaType::ULongLong:
    case QMetaType::UShort:
    case QMetaType::ULong:
    case QMetaType::UChar:
    case QMetaType::Bool:
    case QMetaType::QByteArray:
        return true;
    default:
        return false;
    }
}

bool isLiteralIdByte(uchar c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/// Value of an upper-case hex digit, or -1.
int upperHexValue(QChar ch)
{
    const char16_t c = ch.unicode();
    if (c >= u'0' && c <= u'9')
        return c - u'0';
    if (c >= u'A' && c <= u'F')
        return c - u'A' + 10;
    return -1;
}

QString outputName(const DependencyKey &key)
{
    return key.type == DependencyKey::Type::Attribute
        ? key.attributeKey
        : key.measurementKey.first + QLatin1Char('/') + key.measurementKey.second;
}

} // namespace

// ============================================================================
// Stamps
// ============================================================================

CalculationRecord CalculationRecord::stamped(const StoredCalculationResult &result,
                                             const CalculationRegistry &registry)
{
    CalculationRecord record;
    record.calculationCompatibility = CalculationCompatibilityVersion;
    record.calculationEnvironment = calculationEnvironmentFingerprint(registry);
    record.result = result;
    return record;
}

bool CalculationRecord::stampsAreCurrent(const CalculationRegistry &registry) const
{
    return calculationCompatibility == CalculationCompatibilityVersion
        && calculationEnvironment == calculationEnvironmentFingerprint(registry);
}

// ============================================================================
// File names
// ============================================================================

QString calculationRecordExtension()
{
    return QString::fromLatin1(kExtension);
}

QString encodeRecordFileId(const QString &id)
{
    static const char hex[] = "0123456789ABCDEF";
    const QByteArray utf8 = id.toUtf8();
    QString out;
    out.reserve(utf8.size() * 3);
    for (const char ch : utf8) {
        const uchar c = uchar(ch);
        if (isLiteralIdByte(c)) {
            out.append(QLatin1Char(ch));
        } else {
            out.append(QLatin1Char('%'));
            out.append(QLatin1Char(hex[c >> 4]));
            out.append(QLatin1Char(hex[c & 0x0F]));
        }
    }
    return out;
}

std::optional<QString> decodeRecordFileId(QStringView text)
{
    if (text.isEmpty())
        return std::nullopt;

    QByteArray utf8;
    utf8.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t c = text[i].unicode();
        if (c < 0x80 && isLiteralIdByte(uchar(c))) {
            utf8.append(char(c));
            continue;
        }
        if (c != u'%' || i + 2 >= text.size())
            return std::nullopt;
        const int high = upperHexValue(text[i + 1]);
        const int low = upperHexValue(text[i + 2]);
        if (high < 0 || low < 0)
            return std::nullopt;
        utf8.append(char(high * 16 + low));
        i += 2;
    }

    // Stateless: an incomplete sequence at the end is an error. The initial
    // BOM is kept: it is part of the id like any other character.
    QStringDecoder decoder(QStringDecoder::Utf8,
                           QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom);
    const QString id = decoder.decode(utf8);
    if (decoder.hasError())
        return std::nullopt;

    // Only the canonical form: e.g. "%61" (an escaped literal) is refused.
    if (encodeRecordFileId(id) != text)
        return std::nullopt;
    return id;
}

QString recordFileName(const QString &stem, const QString &calculationId)
{
    return stem + QLatin1Char('.') + encodeRecordFileId(calculationId)
        + QLatin1Char('.') + calculationRecordExtension();
}

std::optional<std::pair<QString, QString>> parseRecordFileName(QStringView fileName)
{
    const QString suffix = QLatin1Char('.') + calculationRecordExtension();
    if (!fileName.endsWith(suffix, Qt::CaseInsensitive))
        return std::nullopt;

    const QStringView rest = fileName.chopped(suffix.size());
    const qsizetype dot = rest.lastIndexOf(u'.');
    if (dot <= 0)                   // no dot, or an empty stem
        return std::nullopt;

    const std::optional<QString> id = decodeRecordFileId(rest.sliced(dot + 1));
    if (!id)
        return std::nullopt;
    return std::make_pair(rest.first(dot).toString(), *id);
}

// ============================================================================
// Encoding
// ============================================================================

std::optional<QByteArray> encodeCalculationRecord(const CalculationRecord &record, QString *error)
{
    if (error)
        error->clear();
    const auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return std::optional<QByteArray>();
    };

    const StoredCalculationResult &result = record.result;
    if (result.calculationId.isEmpty())
        return fail(QStringLiteral("The record has no calculation id"));
    if (result.inputFingerprint.size() != InputFingerprintSize)
        return fail(QStringLiteral("The input fingerprint has %1 bytes, not %2")
                        .arg(result.inputFingerprint.size()).arg(InputFingerprintSize));

    QByteArray bytes;
    {
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        pinStream(stream);

        stream.writeRawData(kMagic, kMagicSize);
        stream << quint32(CalculationRecordFormatVersion);
        stream << qint32(record.calculationCompatibility);
        stream << record.calculationEnvironment;
        stream << result.calculationId;
        stream << result.resultVersion;
        // Written once; the decoder restores it as the bundle's reason and as
        // the snapshot's detail, which the snapshot keeps equal.
        stream << result.bundle.reason();
        stream << result.inputFingerprint;

        stream << quint32(result.leaves.size());
        for (const GraphNode &leaf : result.leaves) {
            if (!isStoredLeafKind(leaf.kind))
                return fail(QStringLiteral("Leaf '%1' is not an input a record can hold").arg(leaf.a));
            stream << storedLeafKindCode(leaf.kind) << leaf.a << leaf.b;
        }

        const CalculationResult &bundle = result.bundle;
        const QList<DependencyKey> outputs = bundle.setOutputs();
        stream << quint32(outputs.size());
        for (const DependencyKey &key : outputs) {
            const bool available = bundle.isAvailable(key);
            if (key.type == DependencyKey::Type::Attribute) {
                stream << kAttributeCode << key.attributeKey << QString() << available;
                if (!available)
                    continue;
                const QVariant value = bundle.attributeValue(key.attributeKey);
                if (!isRecordableAttributeType(value.typeId())) {
                    return fail(QStringLiteral("Attribute '%1' has a type a record cannot hold (%2)")
                                    .arg(key.attributeKey, QString::fromLatin1(value.metaType().name())));
                }
                stream << value;
            } else {
                const QString &sensor = key.measurementKey.first;
                const QString &name = key.measurementKey.second;
                stream << kMeasurementCode << sensor << name << available;
                if (!available)
                    continue;
                const QVector<double> samples = bundle.measurementValues(sensor, name);
                if (quint64(samples.size()) > kMaxSampleCount) {
                    return fail(QStringLiteral("Measurement '%1' has too many samples for a record (%2)")
                                    .arg(outputName(key)).arg(samples.size()));
                }
                // The bytes of operator<<(QList<double>) at the pinned
                // settings: a quint32 count, then each double's 8 bytes.
                stream << quint32(samples.size());
                QByteArray raw(samples.size() * kSampleBytes, Qt::Uninitialized);
                qToLittleEndian<double>(samples.constData(), samples.size(), raw.data());
                stream.writeRawData(raw.constData(), raw.size());
                stream << bundle.measurementUnit(sensor, name);
            }
        }

        if (stream.status() != QDataStream::Ok)
            return fail(QStringLiteral("The record could not be encoded (stream status %1)")
                            .arg(int(stream.status())));
    }

    bytes.append(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
    return bytes;
}

// ============================================================================
// Decoding
// ============================================================================

CalculationRecordStatus decodeCalculationRecord(const QByteArray &bytes, CalculationRecord *out,
                                                QString *error)
{
    using Status = CalculationRecordStatus;
    if (error)
        error->clear();
    const auto fail = [error](Status status, const QString &text) {
        if (error)
            *error = text;
        return status;
    };
    const QString truncated = QStringLiteral("the record is truncated");

    // 1. The magic
    if (bytes.size() < kMagicSize || std::memcmp(bytes.constData(), kMagic, kMagicSize) != 0)
        return fail(Status::NotARecord, QStringLiteral("not a calculation record"));

    // 2. The format version, before the checksum: a future format may change
    //    everything after this field.
    if (bytes.size() < kPrefixSize)
        return fail(Status::Corrupt, truncated);
    const quint32 version = qFromLittleEndian<quint32>(bytes.constData() + kMagicSize);
    if (version != CalculationRecordFormatVersion)
        return fail(Status::UnsupportedVersion,
                    QStringLiteral("format version %1 is not supported").arg(version));

    // 3. The checksum
    if (bytes.size() < kPrefixSize + kChecksumSize)
        return fail(Status::Corrupt, truncated);
    const qsizetype bodyEnd = bytes.size() - kChecksumSize;
    if (QCryptographicHash::hash(QByteArrayView(bytes).first(bodyEnd), QCryptographicHash::Sha256)
        != bytes.last(kChecksumSize))
        return fail(Status::Corrupt, QStringLiteral("checksum mismatch"));

    // 4. The structure. The payload shares the caller's bytes (no copy).
    const QByteArray payload = QByteArray::fromRawData(bytes.constData() + kPrefixSize,
                                                       bodyEnd - kPrefixSize);
    QDataStream stream(payload);
    pinStream(stream);
    const auto remaining = [&stream]() { return stream.device()->bytesAvailable(); };

    CalculationRecord record;
    StoredCalculationResult &result = record.result;
    qint32 compatibility = 0;
    QString reason;
    stream >> compatibility >> record.calculationEnvironment >> result.calculationId
           >> result.resultVersion >> reason >> result.inputFingerprint;
    if (stream.status() != QDataStream::Ok)
        return fail(Status::Corrupt, truncated);
    record.calculationCompatibility = compatibility;
    if (result.calculationId.isEmpty())
        return fail(Status::Corrupt, QStringLiteral("the record has no calculation id"));
    if (result.inputFingerprint.size() != InputFingerprintSize)
        return fail(Status::Corrupt, QStringLiteral("the input fingerprint has %1 bytes")
                                         .arg(result.inputFingerprint.size()));

    // Leaves
    quint32 leafCount = 0;
    stream >> leafCount;
    if (stream.status() != QDataStream::Ok)
        return fail(Status::Corrupt, truncated);
    if (leafCount > remaining() / kMinLeafBytes)
        return fail(Status::Corrupt, QStringLiteral("leaf count %1 exceeds the record").arg(leafCount));
    result.leaves.reserve(leafCount);
    for (quint32 i = 0; i < leafCount; ++i) {
        quint8 code = 0;
        QString a, b;
        stream >> code >> a >> b;
        if (stream.status() != QDataStream::Ok)
            return fail(Status::Corrupt, truncated);
        const std::optional<GraphNode> leaf = storedLeafFromCode(code, a, b);
        if (!leaf)
            return fail(Status::Corrupt, QStringLiteral("unknown leaf kind %1").arg(int(code)));
        result.leaves.append(*leaf);
    }

    // Outputs, rebuilt through the public setters in file order
    quint32 outputCount = 0;
    stream >> outputCount;
    if (stream.status() != QDataStream::Ok)
        return fail(Status::Corrupt, truncated);
    if (outputCount > remaining() / kMinOutputBytes)
        return fail(Status::Corrupt, QStringLiteral("output count %1 exceeds the record").arg(outputCount));

    CalculationResult &bundle = result.bundle;
    for (quint32 i = 0; i < outputCount; ++i) {
        quint8 code = 0;
        QString first, second;
        bool available = false;
        stream >> code >> first >> second >> available;
        if (stream.status() != QDataStream::Ok)
            return fail(Status::Corrupt, truncated);

        DependencyKey key;
        if (code == kAttributeCode)
            key = DependencyKey::attribute(first);
        else if (code == kMeasurementCode)
            key = DependencyKey::measurement(first, second);
        else
            return fail(Status::Corrupt, QStringLiteral("unknown output kind %1").arg(int(code)));

        if (bundle.contains(key))
            return fail(Status::Corrupt, QStringLiteral("output '%1' appears twice").arg(outputName(key)));

        if (!available) {
            bundle.setUnavailable(key);
            continue;
        }

        if (key.type == DependencyKey::Type::Attribute) {
            // Refuse the type before Qt reads the value: a container type
            // would reserve whatever count the bytes claim.
            if (remaining() < 4)
                return fail(Status::Corrupt, truncated);
            const QByteArray typeBytes = stream.device()->peek(4);
            const quint32 typeId = qFromLittleEndian<quint32>(typeBytes.constData());
            if (!isRecordableAttributeType(int(typeId)))
                return fail(Status::Corrupt, QStringLiteral("attribute '%1' has a type a record cannot hold")
                                                 .arg(first));
            QVariant value;
            stream >> value;
            if (stream.status() != QDataStream::Ok)
                return fail(Status::Corrupt, truncated);
            if (!value.isValid() || !isRecordableAttributeType(value.typeId()))
                return fail(Status::Corrupt, QStringLiteral("attribute '%1' has a type a record cannot hold")
                                                 .arg(first));
            bundle.setAttribute(first, value);
        } else {
            quint32 count = 0;
            stream >> count;
            if (stream.status() != QDataStream::Ok)
                return fail(Status::Corrupt, truncated);
            // Checked before anything is allocated
            if (qint64(count) > remaining() / kSampleBytes)
                return fail(Status::Corrupt, QStringLiteral("sample count %1 exceeds the record").arg(count));
            if (count == 0)
                return fail(Status::Corrupt, QStringLiteral("measurement '%1' is available but has no samples")
                                                 .arg(outputName(key)));
            QByteArray raw(qsizetype(count) * kSampleBytes, Qt::Uninitialized);
            if (stream.readRawData(raw.data(), raw.size()) != raw.size())
                return fail(Status::Corrupt, truncated);
            QVector<double> samples(count);
            qFromLittleEndian<double>(raw.constData(), samples.size(), samples.data());
            QString unit;
            stream >> unit;
            if (stream.status() != QDataStream::Ok)
                return fail(Status::Corrupt, truncated);
            bundle.setMeasurement(first, second, samples, unit);
        }
    }

    if (!stream.atEnd())
        return fail(Status::Corrupt, QStringLiteral("unexpected bytes after the last output"));

    bundle.setReason(reason);
    result.detail = reason;
    if (out)
        *out = std::move(record);
    return Status::Ok;
}

} // namespace FlySight
