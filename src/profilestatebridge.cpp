#include "profilestatebridge.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QSettings>

#include "mainwindow.h"
#include "plotmodel.h"
#include "markermodel.h"
#include "plotregistry.h"
#include "markerregistry.h"
#include "plotviewsettingsmodel.h"
#include "preferences/preferencesmanager.h"
#include "preferences/preferencekeys.h"
#include "logbookcolumn.h"
#include "ui/docks/analysis/AnalysisDockFeature.h"
#include "ui/docks/plotselection/PlotSelectionDockFeature.h"
#include "ui/docks/markerselection/MarkerSelectionDockFeature.h"

namespace FlySight {

// ============================================================================
// captureCurrentState
// ============================================================================

Profile captureCurrentState(MainWindow *mainWindow)
{
    Profile profile;

    // 1. Enabled plots
    {
        QStringList enabledList;
        const QVector<PlotValue> allPlots = PlotRegistry::instance().dependentPlots();
        PlotModel *pm = mainWindow->plotModel();
        for (const PlotValue &pv : allPlots) {
            if (pm->isPlotEnabled(pv.sensorID, pv.measurementID))
                enabledList.append(PlotModel::makePlotId(pv));
        }
        profile.enabledPlots = enabledList;
    }

    // 2. Enabled markers
    {
        QStringList enabledList;
        const QVector<MarkerDefinition> allMarkers = MarkerRegistry::instance()->allMarkers();
        MarkerModel *mm = mainWindow->markerModel();
        for (const MarkerDefinition &def : allMarkers) {
            if (mm->isMarkerEnabled(def.attributeKey))
                enabledList.append(def.attributeKey);
        }
        profile.enabledMarkers = enabledList;
    }

    // 3. Reference marker
    {
        PlotViewSettingsModel *pvsm = mainWindow->plotViewSettingsModel();
        profile.referenceMarker = pvsm->referenceMarkerKey();
    }

    // 4. X-axis variable
    {
        PlotViewSettingsModel *pvsm = mainWindow->plotViewSettingsModel();
        profile.xAxisVariable = pvsm->xVariable();
    }

    // 5. Zoom extent
    {
        auto &prefs = PreferencesManager::instance();
        QJsonObject zoom;
        zoom[QStringLiteral("mode")] = prefs.getValue(PreferenceKeys::ZoomExtentMode).toString();
        zoom[QStringLiteral("startMarker")] = prefs.getValue(PreferenceKeys::ZoomExtentStartMarker).toString();
        zoom[QStringLiteral("endMarker")] = prefs.getValue(PreferenceKeys::ZoomExtentEndMarker).toString();
        zoom[QStringLiteral("marginPct")] = prefs.getValue(PreferenceKeys::ZoomExtentMarginPct).toDouble();
        profile.zoomExtent = zoom;
    }

    // 6. Logbook columns
    {
        const QVector<LogbookColumn> cols = LogbookColumnStore::instance().columns();
        QJsonArray arr;
        for (const LogbookColumn &col : cols) {
            QJsonObject obj;
            obj[QStringLiteral("type")] = static_cast<int>(col.type);
            obj[QStringLiteral("attributeKey")] = col.attributeKey;
            obj[QStringLiteral("sensorID")] = col.sensorID;
            obj[QStringLiteral("measurementID")] = col.measurementID;
            obj[QStringLiteral("measurementType")] = col.measurementType;
            obj[QStringLiteral("markerAttributeKey")] = col.markerAttributeKey;
            obj[QStringLiteral("marker2AttributeKey")] = col.marker2AttributeKey;
            obj[QStringLiteral("enabled")] = col.enabled;
            obj[QStringLiteral("customLabel")] = col.customLabel;
            arr.append(obj);
        }
        profile.logbookColumns = arr;
    }

    // 7. Dock layout
    {
        profile.dockLayout = mainWindow->captureDockLayout();
    }

    // 8. Tree expansion state
    {
        QJsonObject expansion;

        auto *plotFeature = mainWindow->findFeature<PlotSelectionDockFeature>();
        if (plotFeature) {
            QJsonArray plotArr;
            for (const QString &cat : plotFeature->expandedCategories())
                plotArr.append(cat);
            expansion[QStringLiteral("plotExpansion")] = plotArr;
        }

        auto *markerFeature = mainWindow->findFeature<MarkerSelectionDockFeature>();
        if (markerFeature) {
            QJsonArray markerArr;
            for (const QString &cat : markerFeature->expandedCategories())
                markerArr.append(cat);
            expansion[QStringLiteral("markerExpansion")] = markerArr;
        }

        profile.treeExpansionState = expansion;
    }

    // 9. Altitude markers
    {
        QSettings settings;
        int count = settings.beginReadArray(QStringLiteral("altitudeMarkers"));
        QJsonArray altitudes;
        for (int i = 0; i < count; ++i) {
            settings.setArrayIndex(i);
            altitudes.append(settings.value(QStringLiteral("value")).toInt());
        }
        settings.endArray();

        QJsonObject altObj;
        altObj[QStringLiteral("units")] = PreferencesManager::instance()
            .getValue(PreferenceKeys::AltitudeMarkersUnits).toString();
        altObj[QStringLiteral("altitudes")] = altitudes;
        profile.altitudeMarkers = altObj;
    }

    // 10. Analysis method
    {
        auto *analysisFeature = mainWindow->findFeature<AnalysisDockFeature>();
        if (analysisFeature)
            profile.analysisMethod = analysisFeature->currentMethodName();
    }

    return profile;
}

// ============================================================================
// applyProfile
// ============================================================================

void applyProfile(const Profile &profile, MainWindow *mainWindow)
{
    // 1. Enabled plots. The rule, including that a listed plot the
    // application no longer has is ignored silently, is the plot model's.
    if (profile.enabledPlots.has_value())
        mainWindow->plotModel()->setEnabledPlotIds(*profile.enabledPlots);

    // 2. Altitude markers (must come before enabled-markers so the markers exist)
    if (profile.altitudeMarkers.has_value()) {
        const QJsonObject &alt = *profile.altitudeMarkers;

        // Write units
        if (alt.contains(QStringLiteral("units")))
            PreferencesManager::instance().setValue(
                PreferenceKeys::AltitudeMarkersUnits,
                alt[QStringLiteral("units")].toString());

        // Write altitude array to QSettings
        if (alt.contains(QStringLiteral("altitudes"))) {
            const QJsonArray altArr = alt[QStringLiteral("altitudes")].toArray();
            QSettings settings;
            settings.beginWriteArray(QStringLiteral("altitudeMarkers"), altArr.size());
            for (int i = 0; i < altArr.size(); ++i) {
                settings.setArrayIndex(i);
                settings.setValue(QStringLiteral("value"), altArr[i].toInt());
            }
            settings.endArray();
        }

        // Increment version to trigger AltitudeMarkerManager::refresh()
        auto &prefs = PreferencesManager::instance();
        int version = prefs.getValue(PreferenceKeys::AltitudeMarkersVersion).toInt();
        prefs.setValue(PreferenceKeys::AltitudeMarkersVersion, version + 1);
    }

    // 3. Enabled markers
    if (profile.enabledMarkers.has_value()) {
        const QSet<QString> enabledSet(profile.enabledMarkers->begin(), profile.enabledMarkers->end());
        const QVector<MarkerDefinition> allMarkers = MarkerRegistry::instance()->allMarkers();
        MarkerModel *mm = mainWindow->markerModel();
        for (const MarkerDefinition &def : allMarkers) {
            mm->setMarkerEnabled(def.attributeKey, enabledSet.contains(def.attributeKey));
        }
    }

    // 4. Reference marker
    if (profile.referenceMarker.has_value()) {
        mainWindow->plotViewSettingsModel()->setReferenceMarkerKey(*profile.referenceMarker);
    }

    // 5. X-axis variable
    if (profile.xAxisVariable.has_value()) {
        mainWindow->plotViewSettingsModel()->setXVariable(*profile.xAxisVariable);
    }

    // 6. Zoom extent
    if (profile.zoomExtent.has_value()) {
        auto &prefs = PreferencesManager::instance();
        const QJsonObject &zoom = *profile.zoomExtent;

        if (zoom.contains(QStringLiteral("mode")))
            prefs.setValue(PreferenceKeys::ZoomExtentMode, zoom[QStringLiteral("mode")].toString());
        if (zoom.contains(QStringLiteral("startMarker")))
            prefs.setValue(PreferenceKeys::ZoomExtentStartMarker, zoom[QStringLiteral("startMarker")].toString());
        if (zoom.contains(QStringLiteral("endMarker")))
            prefs.setValue(PreferenceKeys::ZoomExtentEndMarker, zoom[QStringLiteral("endMarker")].toString());
        if (zoom.contains(QStringLiteral("marginPct")))
            prefs.setValue(PreferenceKeys::ZoomExtentMarginPct, zoom[QStringLiteral("marginPct")].toDouble());
    }

    // 7. Logbook columns
    if (profile.logbookColumns.has_value()) {
        QVector<LogbookColumn> cols;
        const QJsonArray &arr = *profile.logbookColumns;
        for (const QJsonValue &val : arr) {
            QJsonObject obj = val.toObject();
            LogbookColumn col;
            int typeInt = obj[QStringLiteral("type")].toInt(0);
            if (typeInt < 0 || typeInt > static_cast<int>(ColumnType::Delta))
                typeInt = 0;
            col.type = static_cast<ColumnType>(typeInt);
            col.attributeKey = obj[QStringLiteral("attributeKey")].toString();
            col.sensorID = obj[QStringLiteral("sensorID")].toString();
            col.measurementID = obj[QStringLiteral("measurementID")].toString();
            col.measurementType = obj[QStringLiteral("measurementType")].toString();
            col.markerAttributeKey = obj[QStringLiteral("markerAttributeKey")].toString();
            col.marker2AttributeKey = obj[QStringLiteral("marker2AttributeKey")].toString();
            col.enabled = obj[QStringLiteral("enabled")].toBool(true);
            col.customLabel = obj[QStringLiteral("customLabel")].toString();
            cols.append(col);
        }
        LogbookColumnStore::instance().setColumns(cols);
    }

    // 8. Dock layout (apply after model state so widgets exist)
    if (profile.dockLayout.has_value()) {
        mainWindow->applyDockLayout(*profile.dockLayout);
    }

    // 9. Tree expansion state (apply last so tree views exist after dock layout restore)
    if (profile.treeExpansionState.has_value()) {
        const QJsonObject &expansion = *profile.treeExpansionState;

        auto *plotFeature = mainWindow->findFeature<PlotSelectionDockFeature>();
        if (plotFeature && expansion.contains(QStringLiteral("plotExpansion"))) {
            QSet<QString> categories;
            const QJsonArray plotArr = expansion[QStringLiteral("plotExpansion")].toArray();
            for (const QJsonValue &v : plotArr)
                categories.insert(v.toString());
            plotFeature->setExpandedCategories(categories);
        }

        auto *markerFeature = mainWindow->findFeature<MarkerSelectionDockFeature>();
        if (markerFeature && expansion.contains(QStringLiteral("markerExpansion"))) {
            QSet<QString> categories;
            const QJsonArray markerArr = expansion[QStringLiteral("markerExpansion")].toArray();
            for (const QJsonValue &v : markerArr)
                categories.insert(v.toString());
            markerFeature->setExpandedCategories(categories);
        }
    }

    // 10. Analysis method (after dock layout so the analysis dock widget exists)
    if (profile.analysisMethod.has_value()) {
        auto *analysisFeature = mainWindow->findFeature<AnalysisDockFeature>();
        if (analysisFeature)
            analysisFeature->setCurrentMethodByName(*profile.analysisMethod);
    }
}

} // namespace FlySight
