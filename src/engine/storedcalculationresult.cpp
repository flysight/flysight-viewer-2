#include "storedcalculationresult.h"

#include <cmath>
#include <cstring>

#include <QCryptographicHash>
#include <QtEndian>

#include "../csvformat.h"

namespace FlySight {

namespace {

// The version-1 magic, terminator included (sizeof counts it).
constexpr char kFingerprintMagic[] = "flysight-inputs-v1";
constexpr quint64 kCanonicalNaN = Q_UINT64_C(0x7FF8000000000000);

void appendU8(QByteArray &out, quint8 v)
{
    out.append(char(v));
}

void appendU32(QByteArray &out, quint32 v)
{
    char bytes[sizeof v];
    qToLittleEndian(v, bytes);
    out.append(bytes, sizeof bytes);
}

void appendU64(QByteArray &out, quint64 v)
{
    char bytes[sizeof v];
    qToLittleEndian(v, bytes);
    out.append(bytes, sizeof bytes);
}

void appendString(QByteArray &out, const QString &s)
{
    const QByteArray utf8 = s.toUtf8();
    appendU32(out, quint32(utf8.size()));
    out.append(utf8);
}

/// u8 1 + str(text), or u8 0 when the value has no session-file text.
void appendAttributeText(QByteArray &out, const QVariant &value)
{
    const std::optional<QString> text = CsvFormat::formatAttributeValue(value);
    if (text) {
        appendU8(out, 1);
        appendString(out, *text);
    } else {
        appendU8(out, 0);
    }
}

quint64 canonicalBits(double v)
{
    if (std::isnan(v))
        return kCanonicalNaN;       // sign and payload do not survive a session file
    quint64 bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return bits;
}

bool sameSamples(const QVector<double> &a, const QVector<double> &b)
{
    // Bit pattern, not ==: NaN equals NaN, and -0.0 differs from 0.0.
    return a.size() == b.size()
        && (a.isEmpty()
            || std::memcmp(a.constData(), b.constData(), size_t(a.size()) * sizeof(double)) == 0);
}

template <typename T>
bool sameBits(const QVariant &a, const QVariant &b)
{
    const T x = a.value<T>();
    const T y = b.value<T>();
    return std::memcmp(&x, &y, sizeof x) == 0;
}

bool sameAttribute(const QVariant &a, const QVariant &b)
{
    // Same metatype first: QVariant == would call 1 and 1.0 equal.
    if (a.isValid() != b.isValid())
        return false;
    if (!a.isValid())
        return true;
    if (a.metaType() != b.metaType())
        return false;
    switch (a.metaType().id()) {
    case QMetaType::Double:
        return sameBits<double>(a, b);   // NaN == same NaN bits, -0 != +0
    case QMetaType::Float:
        return sameBits<float>(a, b);
    default:
        return a == b;
    }
}

} // namespace

bool isStoredLeafKind(GraphNode::Kind kind)
{
    switch (kind) {
    case GraphNode::Kind::StoredAttribute:
    case GraphNode::Kind::SourceMeasurement:
    case GraphNode::Kind::SourceUnit:
    case GraphNode::Kind::Preference:
        return true;
    case GraphNode::Kind::Resolution:
    case GraphNode::Kind::Result:
    case GraphNode::Kind::Prepared:
        break;
    }
    return false;
}

quint8 storedLeafKindCode(GraphNode::Kind kind)
{
    switch (kind) {
    case GraphNode::Kind::StoredAttribute:   return 0;
    case GraphNode::Kind::SourceMeasurement: return 1;
    case GraphNode::Kind::SourceUnit:        return 2;
    case GraphNode::Kind::Preference:        return 3;
    case GraphNode::Kind::Resolution:
    case GraphNode::Kind::Result:
    case GraphNode::Kind::Prepared:
        break;
    }
    Q_ASSERT_X(false, "storedLeafKindCode", "not a leaf kind");
    return 0xFF;
}

std::optional<GraphNode> storedLeafFromCode(quint8 code, const QString &a, const QString &b)
{
    GraphNode n;
    switch (code) {
    case 0: n = GraphNode::storedAttribute(a); break;
    case 1: n = GraphNode::sourceMeasurement(a, b); break;
    case 2: n = GraphNode::sourceUnit(a, b); break;
    case 3: n = GraphNode::preference(a); break;
    default:
        return std::nullopt;
    }
    n.b = b;    // faithful to what was read, for every kind
    return n;
}

bool storedLeafLess(const GraphNode &lhs, const GraphNode &rhs)
{
    const quint8 lk = storedLeafKindCode(lhs.kind);
    const quint8 rk = storedLeafKindCode(rhs.kind);
    if (lk != rk)
        return lk < rk;
    if (const int c = QString::compare(lhs.a, rhs.a, Qt::CaseSensitive))
        return c < 0;
    return QString::compare(lhs.b, rhs.b, Qt::CaseSensitive) < 0;
}

QByteArray inputFingerprintEncoding(const QList<GraphNode> &leaves, const ISessionState &state,
                                    const IPreferenceProvider *preferences)
{
    QByteArray out;
    out.append(kFingerprintMagic, sizeof kFingerprintMagic);
    appendU32(out, quint32(leaves.size()));

    for (const GraphNode &leaf : leaves) {
        appendU8(out, storedLeafKindCode(leaf.kind));
        appendString(out, leaf.a);
        appendString(out, leaf.b);

        switch (leaf.kind) {
        case GraphNode::Kind::StoredAttribute: {
            const bool present = state.hasStoredAttribute(leaf.a);
            appendU8(out, present ? 1 : 0);
            if (present)
                appendAttributeText(out, state.storedAttribute(leaf.a));
            break;
        }
        case GraphNode::Kind::SourceMeasurement: {
            const bool present = state.hasSourceMeasurement(leaf.a, leaf.b);
            appendU8(out, present ? 1 : 0);
            if (present) {
                const QVector<double> samples = state.sourceMeasurement(leaf.a, leaf.b);
                appendU64(out, quint64(samples.size()));
                for (double v : samples)
                    appendU64(out, canonicalBits(v));
            }
            break;
        }
        case GraphNode::Kind::SourceUnit: {
            // The unit exists exactly when the measurement does.
            const bool present = state.hasSourceMeasurement(leaf.a, leaf.b);
            appendU8(out, present ? 1 : 0);
            if (present)
                appendString(out, state.sourceUnit(leaf.a, leaf.b));
            break;
        }
        case GraphNode::Kind::Preference: {
            const QVariant value = preferences ? preferences->preferenceValue(leaf.a) : QVariant();
            appendU8(out, value.isValid() ? 1 : 0);
            if (value.isValid())
                appendAttributeText(out, value);
            break;
        }
        case GraphNode::Kind::Resolution:
        case GraphNode::Kind::Result:
        case GraphNode::Kind::Prepared:
            // storedLeafKindCode() has asserted; release builds encode the
            // node as absent, so the fingerprint can never match a real one.
            appendU8(out, 0);
            break;
        }
    }
    return out;
}

QByteArray inputFingerprint(const QList<GraphNode> &leaves, const ISessionState &state,
                            const IPreferenceProvider *preferences)
{
    return QCryptographicHash::hash(inputFingerprintEncoding(leaves, state, preferences),
                                    QCryptographicHash::Sha256);
}

bool sameContent(const StoredCalculationResult &a, const StoredCalculationResult &b)
{
    if (a.calculationId != b.calculationId || a.resultVersion != b.resultVersion
        || a.detail != b.detail || a.leaves != b.leaves || a.inputFingerprint != b.inputFingerprint)
        return false;

    const QList<DependencyKey> outputs = a.bundle.setOutputs();
    if (outputs != b.bundle.setOutputs() || a.bundle.reason() != b.bundle.reason())
        return false;

    for (const DependencyKey &out : outputs) {
        const bool available = a.bundle.isAvailable(out);
        if (available != b.bundle.isAvailable(out))
            return false;
        if (!available)
            continue;
        if (out.type == DependencyKey::Type::Attribute) {
            if (!sameAttribute(a.bundle.attributeValue(out.attributeKey), b.bundle.attributeValue(out.attributeKey)))
                return false;
        } else {
            const QString &sensor = out.measurementKey.first;
            const QString &name = out.measurementKey.second;
            if (!sameSamples(a.bundle.measurementValues(sensor, name), b.bundle.measurementValues(sensor, name))
                || a.bundle.measurementUnit(sensor, name) != b.bundle.measurementUnit(sensor, name))
                return false;
        }
    }
    return true;
}

} // namespace FlySight
