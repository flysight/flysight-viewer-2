#include "fusiongolden.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace FlySightTest {

namespace {

const char kChannelsHeader[] = "# flysight fusion golden channels v1";

QString goldenPath(const QString &fileName)
{
    return QStringLiteral(FLYSIGHT_FUSION_GOLDEN_DIR "/") + fileName;
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Fusion golden: cannot read %s", qPrintable(path));
    return file.readAll();
}

bool sameBitPattern(double a, double b)
{
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

/// Line 2 of a channels file: the one expression the loader checks against
/// and the writer produces, so the two cannot differ.
QByteArray channelsColumnsLine()
{
    return "# columns: " + fusionChannelNames().join(QLatin1Char(' ')).toLatin1();
}

} // namespace

// The sample form of a channels file, reader and writer side by side: each is
// exactly the inverse of the other (bit pattern, not value: NaN payloads, -0.0
// and subnormals survive the round trip).

double fromHexBits(const QByteArray &text, const QString &path)
{
    bool ok = false;
    const quint64 bits = text.toULongLong(&ok, 16);
    if (!ok || text.size() != 16)
        qFatal("Fusion golden: bad sample '%s' in %s", text.constData(), qPrintable(path));
    double value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

QByteArray toHexBits(double value)
{
    quint64 bits;
    std::memcpy(&bits, &value, sizeof bits);
    char text[17];
    std::snprintf(text, sizeof text, "%016llX", static_cast<unsigned long long>(bits));
    return QByteArray(text, 16);
}

namespace {

QHash<QString, QVector<double>> loadChannels(const QString &fileName, int rows)
{
    const QString path = goldenPath(fileName);
    QList<QByteArray> lines = readAll(path).split('\n');
    for (QByteArray &line : lines)
        line = line.trimmed();                     // drops the CR of a CRLF checkout
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();

    const QStringList &names = fusionChannelNames();
    const QByteArray columns = channelsColumnsLine();
    if (lines.size() != rows + 2 || lines.at(0) != kChannelsHeader || lines.at(1) != columns)
        qFatal("Fusion golden: %s does not have the v1 header, the expected columns and %d rows",
               qPrintable(path), rows);

    QHash<QString, QVector<double>> channels;
    for (const QString &name : names)
        channels[name].reserve(rows);
    for (int row = 0; row < rows; ++row) {
        const QList<QByteArray> fields = lines.at(row + 2).split(' ');
        if (fields.size() != names.size())
            qFatal("Fusion golden: row %d of %s does not have %d columns",
                   row, qPrintable(path), int(names.size()));
        for (qsizetype column = 0; column < names.size(); ++column)
            channels[names.at(column)].append(fromHexBits(fields.at(column), path));
    }
    return channels;
}

/// Numbers under these keys are counts or copies of an input (the tuning
/// thresholds under `stopping` among them, and the initializer account's
/// counts, flags, copied lengths and fix times: an epoch-relative fix time is
/// one exact-rounded subtraction of two fixture doubles, the same bits on
/// every IEEE platform), never the result of solver arithmetic: they are
/// exact in both modes.
bool isExactKey(const QString &key)
{
    static const QSet<QString> keys{
        QStringLiteral("gnss_states"), QStringLiteral("imu_outputs"), QStringLiteral("iterations"),
        QStringLiteral("imu_count"), QStringLiteral("gnss_count"), QStringLiteral("origin_index"),
        QStringLiteral("origin"), QStringLiteral("epoch_utc_s"), QStringLiteral("node"),
        QStringLiteral("rows"), QStringLiteral("passes"), QStringLiteral("window"),
        QStringLiteral("bias_settled_tolerance"), QStringLiteral("max_mean_relative_decrease"),
        QStringLiteral("max_nrms"),
        QStringLiteral("index"), QStringLiteral("prefix_fits"), QStringLiteral("segment_length_s"),
        QStringLiteral("prefix_length_s"), QStringLiteral("prefix_start_s"), QStringLiteral("prefix_end_s"),
        QStringLiteral("start_s"), QStringLiteral("end_s"), QStringLiteral("anchor_s"),
        QStringLiteral("anchor_sacc_m_s"), QStringLiteral("fallback_segments"),
        QStringLiteral("prefix_iterations"), QStringLiteral("prefix_passes"),
        QStringLiteral("prefix_on_limit"), QStringLiteral("segment_on_limit"), QStringLiteral("growth_stop")};
    return keys.contains(key);
}

QString describeNumbers(double got, double golden)
{
    const double absolute = std::abs(got - golden);
    const double relative = golden != 0 ? absolute / std::abs(golden) : absolute;
    return QStringLiteral("got %1, golden %2 (absolute %3, relative %4)")
        .arg(got, 0, 'g', 17).arg(golden, 0, 'g', 17).arg(absolute, 0, 'g', 3).arg(relative, 0, 'g', 3);
}

/// `key` is the nearest enclosing object key: array elements inherit it.
QString compareJsonUnder(const QString &path, const QString &key,
                         const QJsonValue &got, const QJsonValue &golden)
{
    if (got.type() != golden.type())
        return QStringLiteral("%1: JSON type %2, golden has type %3")
            .arg(path).arg(int(got.type())).arg(int(golden.type()));

    switch (golden.type()) {
    case QJsonValue::Double: {
        const double a = got.toDouble(), b = golden.toDouble();
        const bool exact = exactParityRequested() || isExactKey(key);
        if (exact ? (a == b) : withinPortableBound(a, b, portableFloor(key)))
            return QString();
        return QStringLiteral("%1: %2").arg(path, describeNumbers(a, b));
    }
    case QJsonValue::Array: {
        const QJsonArray a = got.toArray(), b = golden.toArray();
        if (a.size() != b.size())
            return QStringLiteral("%1: %2 elements, golden has %3").arg(path).arg(a.size()).arg(b.size());
        for (qsizetype i = 0; i < b.size(); ++i) {
            const QString difference =
                compareJsonUnder(QStringLiteral("%1[%2]").arg(path).arg(i), key, a.at(i), b.at(i));
            if (!difference.isEmpty())
                return difference;
        }
        return QString();
    }
    case QJsonValue::Object: {
        const QJsonObject a = got.toObject(), b = golden.toObject();
        if (a.keys() != b.keys())
            return QStringLiteral("%1: keys {%2}, golden has {%3}")
                .arg(path, a.keys().join(QLatin1Char(',')), b.keys().join(QLatin1Char(',')));
        for (auto it = b.constBegin(); it != b.constEnd(); ++it) {
            const QString difference = compareJsonUnder(path + QLatin1Char('.') + it.key(), it.key(),
                                                        a.value(it.key()), it.value());
            if (!difference.isEmpty())
                return difference;
        }
        return QString();
    }
    default:   // String, Bool, Null
        if (got == golden)
            return QString();
        return QStringLiteral("%1: '%2', golden has '%3'")
            .arg(path, got.toVariant().toString(), golden.toVariant().toString());
    }
}

} // namespace

const QStringList &fusionChannelNames()
{
    static const QStringList names{
        QStringLiteral("_time"), QStringLiteral("north"), QStringLiteral("east"), QStringLiteral("down"),
        QStringLiteral("velN"), QStringLiteral("velE"), QStringLiteral("velD"),
        QStringLiteral("accN"), QStringLiteral("accE"), QStringLiteral("accD"),
        QStringLiteral("roll"), QStringLiteral("pitch"), QStringLiteral("yaw"),
        QStringLiteral("qx"), QStringLiteral("qy"), QStringLiteral("qz"), QStringLiteral("qw")};
    return names;
}

const QVector<double> &fusionChannel(const FlySight::Fusion::Result &r, const QString &name)
{
    const QVector<double> *arrays[] = {
        &r.time, &r.north, &r.east, &r.down, &r.velN, &r.velE, &r.velD, &r.accN, &r.accE, &r.accD,
        &r.roll, &r.pitch, &r.yaw, &r.qx, &r.qy, &r.qz, &r.qw};
    const qsizetype index = fusionChannelNames().indexOf(name);
    if (index < 0)
        qFatal("Fusion golden: no channel named %s", qPrintable(name));
    return *arrays[index];
}

QByteArray fusionChannelsText(const FlySight::Fusion::Result &result)
{
    const QStringList &names = fusionChannelNames();
    QList<const QVector<double> *> columns;
    for (const QString &name : names) {
        const QVector<double> &channel = fusionChannel(result, name);
        if (channel.size() != result.time.size())
            qFatal("Fusion golden: channel %s has %lld samples, _time has %lld",
                   qPrintable(name), qlonglong(channel.size()), qlonglong(result.time.size()));
        columns.append(&channel);
    }

    QByteArray text = QByteArray(kChannelsHeader) + '\n' + channelsColumnsLine() + '\n';
    for (qsizetype row = 0; row < result.time.size(); ++row) {
        for (qsizetype column = 0; column < columns.size(); ++column) {
            if (column > 0)
                text += ' ';
            text += toHexBits(columns.at(column)->at(row));
        }
        text += '\n';
    }
    return text;
}

FusionGolden loadFusionGolden(const QString &name)
{
    const QString path = goldenPath(name + QStringLiteral(".json"));
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(readAll(path), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        qFatal("Fusion golden: %s is not a JSON object (%s)",
               qPrintable(path), qPrintable(error.errorString()));
    const QJsonObject object = document.object();

    FusionGolden golden;
    golden.outcome = object.value(QStringLiteral("outcome")).toString();
    golden.diagnostics = object.value(QStringLiteral("diagnostics")).toObject();
    const bool succeeded = golden.outcome == QStringLiteral("succeeded");
    if (object.value(QStringLiteral("fixture")).toString() != name
        || (!succeeded && golden.outcome != QStringLiteral("rejected"))
        || golden.diagnostics.isEmpty()
        || !object.value(QStringLiteral("progress")).isArray())
        qFatal("Fusion golden: %s lacks fixture / outcome / diagnostics / progress",
               qPrintable(path));

    const QJsonArray progress = object.value(QStringLiteral("progress")).toArray();
    for (const QJsonValue &text : progress)
        golden.progress.append(text.toString());

    if (succeeded) {
        golden.trace = object.value(QStringLiteral("trace")).toObject();
        const int rows = object.value(QStringLiteral("rows")).toInt(-1);
        const QString channelsFile = object.value(QStringLiteral("channels_file")).toString();
        if (golden.trace.isEmpty() || rows < 1 || channelsFile.isEmpty())
            qFatal("Fusion golden: %s lacks trace / rows / channels_file", qPrintable(path));
        golden.channels = loadChannels(channelsFile, rows);
    }
    return golden;
}

bool exactParityRequested()
{
    return qEnvironmentVariableIntValue("FLYSIGHT_FUSION_EXACT") == 1;
}

double portableFloor(const QString &channelOrKey)
{
    const bool degrees = channelOrKey == QStringLiteral("roll") || channelOrKey == QStringLiteral("pitch")
                         || channelOrKey == QStringLiteral("yaw")
                         || channelOrKey.endsWith(QStringLiteral("_deg"));
    return degrees ? kPortableAbsoluteDegrees : kPortableAbsolute;
}

bool withinPortableBound(double got, double golden, double floor)
{
    return std::abs(got - golden) <= floor + kPortableRelative * std::abs(golden);
}

bool sameRecomputedValue(double got, double recomputed)
{
    if (sameBitPattern(got, recomputed))
        return true;
    if (exactParityRequested())
        return false;
    // The floor lets two subnormal results differ by their last bit as well.
    const double scale = std::max(std::abs(got), std::abs(recomputed));
    return std::abs(got - recomputed) <= 4 * std::numeric_limits<double>::epsilon() * scale
                                             + std::numeric_limits<double>::denorm_min();
}

QString ParityStatistics::summary() const
{
    return QStringLiteral("%1 of %2 samples not bit-identical; worst absolute difference %3, "
                          "worst relative difference %4")
        .arg(notBitIdentical).arg(samples).arg(worstAbsolute, 0, 'g', 3).arg(worstRelative, 0, 'g', 3);
}

QString compareSamples(const QString &name, const QVector<double> &got,
                       const QVector<double> &golden, ParityStatistics *statistics)
{
    if (got.size() != golden.size())
        return QStringLiteral("%1: %2 samples, golden has %3").arg(name).arg(got.size()).arg(golden.size());

    // Output time is one IEEE addition of the epoch: exact everywhere.
    const bool exact = exactParityRequested() || name == QStringLiteral("_time");
    const double floor = portableFloor(name);
    qsizetype firstBad = -1, worstBad = -1;
    double worstExcess = -1;
    for (qsizetype i = 0; i < golden.size(); ++i) {
        const bool identical = sameBitPattern(got[i], golden[i]);
        const double absolute = std::abs(got[i] - golden[i]);
        const double relative = golden[i] != 0 ? absolute / std::abs(golden[i]) : absolute;
        if (statistics) {
            ++statistics->samples;
            if (!identical) {
                ++statistics->notBitIdentical;
                statistics->worstAbsolute = std::max(statistics->worstAbsolute, absolute);
                statistics->worstRelative = std::max(statistics->worstRelative, relative);
            }
        }
        // A NaN that is not the golden's NaN is outside every bound, so it fails.
        const bool passes = exact ? identical : (identical || withinPortableBound(got[i], golden[i], floor));
        if (passes)
            continue;
        if (firstBad < 0)
            firstBad = i;
        if (!(absolute <= worstExcess)) {
            worstExcess = absolute;
            worstBad = i;
        }
    }
    if (firstBad < 0)
        return QString();
    return QStringLiteral("%1: first difference at sample %2: %3; worst at sample %4: %5")
        .arg(name).arg(firstBad).arg(describeNumbers(got[firstBad], golden[firstBad]))
        .arg(worstBad).arg(describeNumbers(got[worstBad], golden[worstBad]));
}

QString compareJson(const QString &path, const QJsonValue &got, const QJsonValue &golden)
{
    return compareJsonUnder(path, QString(), got, golden);
}

FlySight::Fusion::Channels toChannels(const FusionFixture &f)
{
    FlySight::Fusion::Channels c;
    c.gnssTime = f.gnssTime;
    c.north = f.north;  c.east = f.east;  c.down = f.down;
    c.velN = f.velN;    c.velE = f.velE;  c.velD = f.velD;
    c.hAcc = f.hAcc;    c.vAcc = f.vAcc;  c.sAcc = f.sAcc;
    c.imuTime = f.imuTime;
    c.ax = f.ax;  c.ay = f.ay;  c.az = f.az;
    c.wx = f.wx;  c.wy = f.wy;  c.wz = f.wz;
    c.originIndex = f.originIndex;
    c.originLat = f.originLat;
    c.originLon = f.originLon;
    c.originHMSL = f.originHMSL;
    return c;
}

} // namespace FlySightTest
