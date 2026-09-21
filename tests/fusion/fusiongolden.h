#ifndef FLYSIGHTTEST_FUSIONGOLDEN_H
#define FLYSIGHTTEST_FUSIONGOLDEN_H

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include "fusion/fusion.h"
#include "fusionfixtures.h"

namespace FlySightTest {

/// What sensor-fusion-clean-port produced for one fixture (tests/data/fusion/,
/// see tests/README.md, "Fusion golden parity").
struct FusionGolden {
    QString outcome;                  ///< "succeeded" or "rejected"
    QJsonObject diagnostics;          ///< the reference's diagnostics object
    QStringList progress;             ///< the reference's progress texts, in order
    QJsonObject trace;                ///< initializer result and optimizer history (successes only)
    QHash<QString, QVector<double>> channels;   ///< by column name (successes only), exact bits
};

/// The seventeen output channels in golden column order: "_time", "north", ...
const QStringList &fusionChannelNames();

/// The array of `result` that the column `name` of fusionChannelNames() holds.
const QVector<double> &fusionChannel(const FlySight::Fusion::Result &result, const QString &name);

/// Loads <FLYSIGHT_FUSION_GOLDEN_DIR>/<name>.json and its channels file.
/// Tolerates CRLF line endings; aborts the test executable (qFatal) on a
/// missing or malformed file, so a broken golden cannot look like a pass.
FusionGolden loadFusionGolden(const QString &name);

/// True when FLYSIGHT_FUSION_EXACT=1: every number must equal the golden bit
/// for bit. This is the mode that decides parity on the capture configuration.
/// Otherwise the portable bound |got - golden| <= 1e-9 + 1e-7 * |golden|
/// applies to everything that is not a count, a time stamp or text.
bool exactParityRequested();

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
