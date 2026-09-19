#include "builtincalculations.h"
#include "attributecalculations.h"
#include "gnsscalculations.h"
#include "imucalculations.h"
#include "magcalculations.h"
#include "timecalculations.h"
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
    Calculations::registerSimplificationCalculations(registry);
    Calculations::registerWspCalculations(registry);
    Calculations::registerSpCalculations(registry);
    Calculations::registerInterpolationFamily(registry);
}

QString calculationEnvironmentFingerprint(const CalculationRegistry &registry)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);

    const QList<CalculationId> ids = registry.registeredIds();
    for (const CalculationId &id : ids)
        hash.addData((QStringLiteral("id:") + id + QLatin1Char('\n')).toUtf8());

    const IPreferenceProvider *provider = registry.preferenceProvider();
    const QStringList keys = registry.declaredPreferenceKeys();
    for (const QString &key : keys) {
        // The same text form a session file would carry: exact for doubles,
        // and the same whether QSettings handed back a number or a string.
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
