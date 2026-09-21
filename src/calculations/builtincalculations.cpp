#include "builtincalculations.h"
#include "attributecalculations.h"
#include "gnsscalculations.h"
#include "imucalculations.h"
#include "magcalculations.h"
#include "timecalculations.h"
#include "localcoordinatecalculations.h"
#include "simplificationcalculations.h"
#include "wspcalculations.h"
#include "spcalculations.h"
#include "interpolationcalculations.h"
#include "../conversion/sourceconversion.h"
#include "../csvformat.h"

#include <QCryptographicHash>

namespace FlySight {

void registerBuiltInCalculations(CalculationRegistry &registry)
{
    // The conversion layer (source -> effective values) first. Source
    // conversions are kept in their own ordered list, so their position among
    // the built-ins does not affect any candidate order; it only keeps
    // registeredIds() readable.
    Calculations::registerSourceConversions(registry);

    // The order is fixed: it decides which of several candidates for one output
    // is tried first (only _START_TIME / _DURATION have several). Interpolation
    // comes last, after every calculation with an explicit output name.
    Calculations::registerAttributeCalculations(registry);
    Calculations::registerGnssCalculations(registry);
    Calculations::registerImuCalculations(registry);
    Calculations::registerMagCalculations(registry);
    Calculations::registerTimeCalculations(registry);
    Calculations::registerLocalCoordinateCalculations(registry);
    Calculations::registerSimplificationCalculations(registry);
    Calculations::registerWspCalculations(registry);
    Calculations::registerSpCalculations(registry);
    Calculations::registerInterpolationFamily(registry);
}

QString calculationEnvironmentFingerprint(const CalculationRegistry &registry)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);

    // Registrations, as far as their order can affect a result: for every
    // output name (sorted) the candidates in the order they are tried. The
    // relative order of calculations that share no output is left out, so
    // that a calculation registered at run time (appended) hashes the same as
    // after the next start, where it may be registered between others. An id
    // cannot contain '#'.
    const auto addList = [&hash](const QString &label, const QList<CalculationId> &ids) {
        hash.addData((label + QLatin1Char('\n')).toUtf8());
        for (const CalculationId &id : ids)
            hash.addData((QLatin1Char('#') + id + QLatin1Char('\n')).toUtf8());
    };
    const CandidateOrder order = registry.candidateOrder();
    for (const auto &output : order.byOutput) {
        const DependencyKey &name = output.first;
        addList(name.type == DependencyKey::Type::Measurement
                    ? QStringLiteral("measurement:") + name.measurementKey.first
                          + QLatin1Char('/') + name.measurementKey.second
                    : QStringLiteral("attribute:") + name.attributeKey,
                output.second);
    }
    addList(QStringLiteral("families"), order.families);
    addList(QStringLiteral("conversions"), order.sourceConversions);

    const IPreferenceProvider *provider = registry.preferenceProvider();
    const QStringList keys = registry.declaredPreferenceKeys();
    for (const QString &key : keys) {
        // The same text form a session file would carry: exact for doubles,
        // and the same whether the settings store handed back a number or a string.
        const QVariant value = provider ? provider->preferenceValue(key) : QVariant();
        const QString text = CsvFormat::formatAttributeValue(value).value_or(QString());
        hash.addData((QStringLiteral("pref:") + key + QLatin1Char('=') + text + QLatin1Char('\n')).toUtf8());
    }

    return QString::fromLatin1(hash.result().toHex());
}

void registerBuiltInCalculationMetadata()
{
    Calculations::registerWspMetadata();
    Calculations::registerSpMetadata();
}

} // namespace FlySight
