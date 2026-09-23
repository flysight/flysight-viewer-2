#ifndef FLYSIGHTTEST_FUSIONGOLDEN_H
#define FLYSIGHTTEST_FUSIONGOLDEN_H

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include "fusion/fusion.h"
#include "fusionfixtures.h"

namespace FlySightTest {

/// What the product kernel produced for one fixture when the goldens were
/// last captured by fusion_golden_capture (tests/data/fusion/, see
/// tests/README.md section 11).
struct FusionGolden {
    QString outcome;                  ///< "succeeded" or "rejected"
    QJsonObject diagnostics;          ///< the kernel's diagnostics object
    QStringList progress;             ///< the kernel's progress texts, in order
    QJsonObject trace;                ///< initializer result and optimizer history (successes only)
    QHash<QString, QVector<double>> channels;   ///< by column name (successes only), exact bits
};

/// The seventeen output channels in golden column order: "_time", "north", ...
const QStringList &fusionChannelNames();

/// The array of `result` that the column `name` of fusionChannelNames() holds.
const QVector<double> &fusionChannel(const FlySight::Fusion::Result &result, const QString &name);

/// The channels file of `result`, byte for byte as loadFusionGolden() reads it
/// back: the v1 header line, the column line, then one line per output sample
/// with the seventeen channels of fusionChannelNames() as toHexBits(), space
/// separated; LF line endings, a final LF, nothing else. The writer used by
/// fusion_golden_capture; channelsWriterIsTheInverseOfTheLoader
/// (tst_fusion_golden) holds it to the committed files.
QByteArray fusionChannelsText(const FlySight::Fusion::Result &result);

/// One sample of a channels file: the sixteen upper-case hexadecimal digits of
/// the IEEE-754 bit pattern of `value` (exact by construction, locale-proof),
/// and its inverse, which aborts the executable (qFatal, naming `path`) on
/// anything that is not sixteen hexadecimal digits.
QByteArray toHexBits(double value);
double fromHexBits(const QByteArray &text, const QString &path);

/// Loads <FLYSIGHT_FUSION_GOLDEN_DIR>/<name>.json and its channels file.
/// Tolerates CRLF line endings; aborts the test executable (qFatal) on a
/// missing or malformed file, so a broken golden cannot look like a pass.
FusionGolden loadFusionGolden(const QString &name);

/// True when FLYSIGHT_FUSION_EXACT=1: every number must equal the golden bit
/// for bit. This is the mode that proves a rebuild on the capture configuration
/// is bit-identical to the capture. Otherwise the portable bound
/// (withinPortableBound) applies to everything that is not a count, a time
/// stamp or text.
bool exactParityRequested();

/// The portable bound: |got - golden| <= floor + kPortableRelative * |golden|.
/// The numbers and the observations they rest on are in tests/README.md,
/// "Tolerance policy". The floor is 1e-7 in the solver's own units (m, m/s,
/// m/s^2, rad, rad/s, unit quaternion components), ten times the largest
/// difference any CI compiler has shown (1.06e-8) and far below anything
/// physical. A number expressed in degrees is the solver's angle times
/// 180/pi, and so is its floor: kPortableAbsoluteDegrees applies to roll,
/// pitch and yaw and to JSON keys ending in "_deg" (portableFloor()). The
/// relative term serves the large numbers (positions in metres, unwrapped
/// angles, costs). False for a NaN on either side.
constexpr double kPortableAbsolute = 1e-7;
constexpr double kPortableAbsoluteDegrees = kPortableAbsolute * 57.295779513082323;
constexpr double kPortableRelative = 1e-7;
double portableFloor(const QString &channelOrKey);
bool withinPortableBound(double got, double golden, double floor = kPortableAbsolute);

/// For a value the TEST recomputes from floating-point products (accH from
/// accN and accE, say), as opposed to a golden or a copy. Exact mode: the same
/// bits, which holds on the capture compiler (no contraction). Portable mode:
/// within 4 ulp, because a compiler that contracts a*a + b*b into a fused
/// multiply-add (clang on arm64 by default) may do so in the library and not
/// in the test, or the other way round, and the last bit then differs.
bool sameRecomputedValue(double got, double recomputed);

/// Accumulated over compareSamples() calls, for the per-fixture log line.
struct ParityStatistics {
    qsizetype samples = 0, notBitIdentical = 0;
    double worstAbsolute = 0, worstRelative = 0;
    QString summary() const;
};

/// Compares one channel with its golden. Empty when it passes; otherwise the
/// first difference and the worst one (index, both values, absolute and
/// relative difference). "_time" is exact in both modes.
QString compareSamples(const QString &name, const QVector<double> &got,
                       const QVector<double> &golden, ParityStatistics *statistics = nullptr);

/// Compares two JSON values recursively: same types, same keys, same lengths,
/// equal strings / bools / nulls; numbers by the mode, except counts and the
/// epoch, which are always exact. `path` names the value in the message.
QString compareJson(const QString &path, const QJsonValue &got, const QJsonValue &golden);

/// Field-by-field copy of a fixture into the kernel's input.
FlySight::Fusion::Channels toChannels(const FusionFixture &fixture);

} // namespace FlySightTest

#endif // FLYSIGHTTEST_FUSIONGOLDEN_H
