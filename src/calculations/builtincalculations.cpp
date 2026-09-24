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

#include <algorithm>
#include <optional>

#include <QCryptographicHash>
#include <QSet>
#include <QStringList>

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

QString calculationEnvironmentDigest(const QList<DependencyKey> &names, const CalculationRegistry &registry)
{
    // The closure over every candidate, not only the one that would win
    // (which one wins depends on session state): every name an evaluation of
    // `names` can resolve, and every preference it can read.
    QSet<DependencyKey> closureNames;
    QSet<QString> closurePreferences;
    for (const DependencyKey &name : names) {
        const StaticDependencies deps = registry.staticDependencies(name);
        closureNames.unite(deps.names);
        closurePreferences.unite(deps.preferences);
    }
    QList<DependencyKey> sortedNames(closureNames.cbegin(), closureNames.cend());
    std::sort(sortedNames.begin(), sortedNames.end());
    QStringList sortedPreferences(closurePreferences.cbegin(), closurePreferences.cend());
    sortedPreferences.sort();

    QCryptographicHash hash(QCryptographicHash::Sha1);
    const auto escaped = [](QString text) {
        text.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
        text.replace(QLatin1Char('\n'), QLatin1String("\\n"));
        return text;
    };
    const auto addLine = [&hash](const QString &line) {
        hash.addData((line + QLatin1Char('\n')).toUtf8());
    };
    // An id cannot contain '#', so a second '#' delimits the result version
    const auto addCandidates = [&](const QList<CalculationInstance> &candidates) {
        for (const CalculationInstance &candidate : candidates) {
            QString line = QLatin1Char('#') + escaped(candidate.instanceId);
            if (candidate.descriptor && !candidate.descriptor->resultVersion.isEmpty())
                line += QLatin1Char('#') + escaped(candidate.descriptor->resultVersion);
            addLine(line);
        }
    };

    // A sensor or measurement name may contain '/', the separator of the two
    const auto escapedField = [&escaped](const QString &text) {
        QString field = escaped(text);
        field.replace(QLatin1Char('/'), QLatin1String("\\/"));
        return field;
    };

    // Per name, what resolution tries for it, in the order it is tried
    for (const DependencyKey &name : std::as_const(sortedNames)) {
        if (name.type == DependencyKey::Type::Measurement) {
            addLine(QStringLiteral("measurement:") + escapedField(name.measurementKey.first) + QLatin1Char('/')
                    + escapedField(name.measurementKey.second));
        } else {
            addLine(QStringLiteral("attribute:") + escaped(name.attributeKey));
        }
        addCandidates(registry.candidatesFor(name));
        if (name.type == DependencyKey::Type::Measurement) {
            if (registry.hasSourceConversions()) {
                addLine(QStringLiteral("conversions"));
                addCandidates(registry.sourceConversionsFor(name.measurementKey.first, name.measurementKey.second));
            } else {
                addLine(QStringLiteral("no conversions"));
            }
        }
    }

    const IPreferenceProvider *provider = registry.preferenceProvider();
    for (const QString &key : std::as_const(sortedPreferences)) {
        // The same text form a session file would carry: exact for doubles,
        // and the same whether the settings store handed back a number or a string.
        const QVariant value = provider ? provider->preferenceValue(key) : QVariant();
        const QString text = CsvFormat::formatAttributeValue(value).value_or(QString());
        addLine(QStringLiteral("pref:") + escaped(key) + QLatin1Char('=') + escaped(text));
    }

    return QString::fromLatin1(hash.result().toHex());
}

void registerBuiltInCalculationMetadata()
{
    Calculations::registerWspMetadata();
    Calculations::registerSpMetadata();
}

} // namespace FlySight
