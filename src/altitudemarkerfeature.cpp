#include "altitudemarkerfeature.h"
#include "sessiondata.h"
#include "dependencykey.h"
#include "markerregistry.h"
#include "engine/calculationregistry.h"
#include "preferences/preferencesmanager.h"
#include "preferences/preferencekeys.h"
#include <QColor>
#include <QHash>
#include <QSet>
#include <QSettings>
#include <algorithm>

using namespace FlySight;

AltitudeMarkerManager::AltitudeMarkerManager(QObject *parent)
    : QObject(parent)
{
    // Register altitude-marker preferences with their defaults
    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.registerPreference(PreferenceKeys::AltitudeMarkersUnits, QStringLiteral("Imperial"));
    prefs.registerPreference(PreferenceKeys::AltitudeMarkersColor, QColor(0x87, 0xCE, 0xEB));
    prefs.registerPreference(PreferenceKeys::AltitudeMarkersSize, 0);
    prefs.registerPreference(PreferenceKeys::AltitudeMarkersVersion, 0);

    // Trigger refresh() whenever any altitude-marker preference changes
    connect(&PreferencesManager::instance(), &PreferencesManager::preferenceChanged,
            this, [this](const QString &key, const QVariant &) {
        if (key.startsWith(QStringLiteral("altitudeMarkers/"))) {
            refresh();
        }
    });
}

AltitudeMarkerManager::~AltitudeMarkerManager()
{
    // The registrations belong to this object; each removal invalidates the
    // attribute in every loaded session.
    CalculationRegistry &registry = CalculationRegistry::instance();
    for (const QString &key : std::as_const(m_registeredKeys))
        registry.unregister(calculationId(key));
}

CalculationId AltitudeMarkerManager::calculationId(const QString &attributeKey)
{
    return QStringLiteral("builtin.altitude.") + attributeKey;
}

CalculationDescriptor AltitudeMarkerManager::makeDescriptor(const QString &attributeKey,
                                                            double thresholdMetres)
{
    CalculationDescriptor d;
    d.id = calculationId(attributeKey);
    d.inputs = {
        CalcInput::attribute(SessionKeys::AnalysisStartTime),
        CalcInput::attribute(SessionKeys::AnalysisEndTime),
        CalcInput::measurement("GNSS", "z"),
        CalcInput::measurement("GNSS", SessionKeys::Time)
    };
    d.outputs = { DependencyKey::attribute(attributeKey) };
    d.compute = [attributeKey, thresholdMetres](const EvaluationContext &ctx) -> CalculationResult {
        // Retrieve analysis window bounds
        QVariant asVar = ctx.attribute(SessionKeys::AnalysisStartTime);
        if (!asVar.canConvert<double>())
            return CalculationResult::unavailable();
        double analysisStartSec = asVar.toDouble();

        QVariant aeVar = ctx.attribute(SessionKeys::AnalysisEndTime);
        if (!aeVar.canConvert<double>())
            return CalculationResult::unavailable();
        double analysisEndSec = aeVar.toDouble();

        // Retrieve GNSS altitude AGL and time vectors
        QVector<double> z    = ctx.measurement("GNSS", "z");
        QVector<double> time = ctx.measurement("GNSS", SessionKeys::Time);

        if (z.isEmpty() || time.isEmpty() || z.size() != time.size())
            return CalculationResult::unavailable();

        // Find the last downward crossing of thresholdMetres within the analysis window
        double lastCrossingTime = -1.0;
        bool   foundCrossing    = false;

        for (int i = 1; i < z.size(); ++i) {
            if (time[i] < analysisStartSec) continue;
            if (time[i - 1] > analysisEndSec) break;

            // Downward crossing: z[i-1] >= threshold AND z[i] < threshold
            if (z[i - 1] >= thresholdMetres && z[i] < thresholdMetres) {
                // Linear interpolation to find precise crossing time
                double t_cross = time[i - 1]
                    + (thresholdMetres - z[i - 1]) / (z[i] - z[i - 1])
                    * (time[i] - time[i - 1]);
                lastCrossingTime = t_cross;
                foundCrossing    = true;
            }
        }

        if (!foundCrossing)
            return CalculationResult::unavailable();

        return CalculationResult().setAttribute(attributeKey, lastCrossingTime);
    };
    return d;
}

void AltitudeMarkerManager::refresh()
{
    // Step 1: Read current preferences
    PreferencesManager &prefs = PreferencesManager::instance();
    QString units    = prefs.getValue(PreferenceKeys::AltitudeMarkersUnits).toString();
    QColor  color    = prefs.getValue(PreferenceKeys::AltitudeMarkersColor).value<QColor>();
    bool    isImperial = (units == QStringLiteral("Imperial"));

    // Step 2: Read altitude array using QSettings directly
    QSettings settings;
    int count = settings.beginReadArray(QStringLiteral("altitudeMarkers"));
    QList<int> altitudes;
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        altitudes.append(settings.value(QStringLiteral("value")).toInt());
    }
    settings.endArray();

    // Sort ascending so markers appear in order in the dock. An altitude
    // listed twice is one marker: its attribute key identifies it.
    std::sort(altitudes.begin(), altitudes.end());
    altitudes.erase(std::unique(altitudes.begin(), altitudes.end()), altitudes.end());

    // Step 3: Build unit labels
    QString unitSuffix = isImperial ? QStringLiteral("FT") : QStringLiteral("M");
    QString unitLabel  = isImperial ? QStringLiteral("ft") : QStringLiteral("m");

    // Step 4: Work out the wanted attribute keys, their thresholds, and the
    // marker definition each key would get
    QStringList wantedKeys;
    QHash<QString, double> thresholds;
    QHash<QString, MarkerDefinition> wantedDefs;
    for (int value : altitudes) {
        QString attributeKey = QStringLiteral("_ALTITUDE_%1_%2").arg(value).arg(unitSuffix);
        QString displayName  = QStringLiteral("%1 %2 AGL").arg(value).arg(unitLabel);
        QString shortLabel   = QStringLiteral("%1%2").arg(value).arg(unitLabel);

        // Convert threshold to SI metres at registration time (baked into the calculation)
        double thresholdMetres = isImperial ? value * 0.3048 : static_cast<double>(value);

        wantedKeys.append(attributeKey);
        thresholds.insert(attributeKey, thresholdMetres);

        MarkerDefinition def;
        def.category     = QStringLiteral("Altitude");
        def.displayName  = displayName;
        def.shortLabel   = shortLabel;
        def.color        = color;
        def.attributeKey = attributeKey;
        def.measurements = {};
        def.editable       = false;
        def.groupId        = QStringLiteral("altitude");
        def.defaultEnabled = true;
        wantedDefs.insert(attributeKey, def);
    }

    // Step 5: Diff against what is registered. The key encodes the altitude
    // and unit, so an unchanged key is an unchanged calculation and is left
    // alone (a colour-only change touches no registration and invalidates
    // nothing). Every unregister / register is broadcast by the registry to
    // the engine of every loaded session, which drops the affected results.
    CalculationRegistry &registry = CalculationRegistry::instance();
    const QSet<QString> wanted(wantedKeys.begin(), wantedKeys.end());
    const QSet<QString> registered(m_registeredKeys.begin(), m_registeredKeys.end());

    QStringList removedKeys;
    for (const QString &key : std::as_const(m_registeredKeys)) {
        if (!wanted.contains(key)) {
            registry.unregister(calculationId(key));
            removedKeys.append(key);
        }
    }

    QStringList nowRegistered;
    for (const QString &key : std::as_const(wantedKeys)) {
        if (registered.contains(key)) {
            nowRegistered.append(key);
        } else if (registry.registerCalculation(makeDescriptor(key, thresholds.value(key)))) {
            nowRegistered.append(key);
        } else {
            qWarning() << "AltitudeMarkerManager: could not register the calculation for" << key;
        }
    }
    m_registeredKeys = nowRegistered;

    // Step 6: Markers only for the keys whose calculation is registered (in
    // altitude order): a marker whose calculation was refused could never have
    // a value. Write the shared marker colour so plot rendering finds it via
    // the standard per-marker key lookup (goes through PreferencesManager like
    // all other marker colour writes).
    QVector<MarkerDefinition> defs;
    for (const QString &key : std::as_const(m_registeredKeys)) {
        defs.append(wantedDefs.value(key));
        PreferencesManager::instance().setValue(PreferenceKeys::markerColorKey(key), color);
    }

    // Atomically replace the altitude group so markersChanged() fires only once
    MarkerRegistry::instance()->replaceMarkerGroup(QStringLiteral("altitude"), defs);

    // Clear saved enabled state for removed markers so that re-adding
    // them later falls through to defaultEnabled = true.
    for (const QString &key : std::as_const(removedKeys)) {
        settings.remove(QStringLiteral("state/markers/") + key);
    }
}
