#ifndef PREFERENCEKEYS_H
#define PREFERENCEKEYS_H

#include <QString>

namespace FlySight {
namespace PreferenceKeys {

// ============================================================================
// General Preferences
// ============================================================================
inline const QString GeneralUnits = QStringLiteral("general/units");
inline const QString GeneralLogbookFolder = QStringLiteral("general/logbookFolder");

// ============================================================================
// Import Preferences
// ============================================================================
inline const QString ImportGroundReferenceMode = QStringLiteral("import/groundReferenceMode");
inline const QString ImportFixedElevation = QStringLiteral("import/fixedElevation");
inline const QString ImportDescentPauseSeconds = QStringLiteral("import/descentPauseSeconds");
inline const QString ImportHideOthersOnImport = QStringLiteral("import/hideOthersOnImport");
/// The orientation token stored into each newly imported recording
/// (SessionKeys::Orientation): a fact of the import, chosen on the Import
/// preferences page from the 24 orientations of Fusion::Orientation.
inline const QString ImportOrientation = QStringLiteral("import/orientation");

// ============================================================================
// Plots Preferences (global)
// ============================================================================
inline const QString PlotsLineThickness = QStringLiteral("plots/lineThickness");
inline const QString PlotsTextSize = QStringLiteral("plots/textSize");
inline const QString PlotsCrosshairColor = QStringLiteral("plots/crosshairColor");
inline const QString PlotsCrosshairThickness = QStringLiteral("plots/crosshairThickness");
inline const QString PlotsYAxisPadding = QStringLiteral("plots/yAxisPadding");

// ============================================================================
// Legend Preferences
// ============================================================================
inline const QString LegendTextSize = QStringLiteral("legend/textSize");

// ============================================================================
// Map Preferences
// ============================================================================
inline const QString MapLineThickness = QStringLiteral("map/lineThickness");
inline const QString MapLargeDotSize = QStringLiteral("map/largeDotSize");
inline const QString MapSmallDotSize = QStringLiteral("map/smallDotSize");
inline const QString MapTrackOpacity = QStringLiteral("map/trackOpacity");
inline const QString MapType = QStringLiteral("map/type");

// ============================================================================
// Per-Plot Preference Key Generators
// ============================================================================

/**
 * @brief Generate the preference key for a plot's color setting.
 * @param sensorID The sensor identifier (e.g., "GNSS", "IMU")
 * @param measurementID The measurement identifier (e.g., "velH", "ax")
 * @return The preference key in format "plots/{sensorID}/{measurementID}/color"
 */
inline QString plotColorKey(const QString &sensorID, const QString &measurementID) {
    return QStringLiteral("plots/%1/%2/color").arg(sensorID, measurementID);
}

/**
 * @brief Generate the preference key for a plot's Y-axis mode setting.
 * @param sensorID The sensor identifier
 * @param measurementID The measurement identifier
 * @return The preference key in format "plots/{sensorID}/{measurementID}/yAxisMode"
 */
inline QString plotYAxisModeKey(const QString &sensorID, const QString &measurementID) {
    return QStringLiteral("plots/%1/%2/yAxisMode").arg(sensorID, measurementID);
}

/**
 * @brief Generate the preference key for a plot's Y-axis minimum value.
 * @param sensorID The sensor identifier
 * @param measurementID The measurement identifier
 * @return The preference key in format "plots/{sensorID}/{measurementID}/yAxisMin"
 */
inline QString plotYAxisMinKey(const QString &sensorID, const QString &measurementID) {
    return QStringLiteral("plots/%1/%2/yAxisMin").arg(sensorID, measurementID);
}

/**
 * @brief Generate the preference key for a plot's Y-axis maximum value.
 * @param sensorID The sensor identifier
 * @param measurementID The measurement identifier
 * @return The preference key in format "plots/{sensorID}/{measurementID}/yAxisMax"
 */
inline QString plotYAxisMaxKey(const QString &sensorID, const QString &measurementID) {
    return QStringLiteral("plots/%1/%2/yAxisMax").arg(sensorID, measurementID);
}

// ============================================================================
// Per-Marker Preference Key Generators
// ============================================================================

/**
 * @brief Generate the preference key for a marker's color setting.
 * @param attributeKey The marker's unique attribute key (e.g., "exitTime", "startTime")
 * @return The preference key in format "markers/{attributeKey}/color"
 */
inline QString markerColorKey(const QString &attributeKey) {
    return QStringLiteral("markers/%1/color").arg(attributeKey);
}

// ============================================================================
// Zoom Preferences
// ============================================================================
inline const QString ZoomExtentMode        = QStringLiteral("zoom/extentMode");
inline const QString ZoomExtentStartMarker = QStringLiteral("zoom/extentStartMarker");
inline const QString ZoomExtentEndMarker   = QStringLiteral("zoom/extentEndMarker");
inline const QString ZoomExtentMarginPct   = QStringLiteral("zoom/extentMarginPct");

// ============================================================================
// Altitude Markers Preferences
// ============================================================================
inline const QString AltitudeMarkersUnits = QStringLiteral("altitudeMarkers/units");
inline const QString AltitudeMarkersColor = QStringLiteral("altitudeMarkers/color");
inline const QString AltitudeMarkersSize  = QStringLiteral("altitudeMarkers/size");
inline const QString AltitudeMarkersVersion = QStringLiteral("altitudeMarkers/version");

/**
 * @brief Generate the preference key for a specific altitude marker's value.
 * @param index 1-based index of the altitude marker (matches QSettings array convention)
 * @return The preference key in format "altitudeMarkers/{index}/value"
 */
inline QString altitudeMarkerValueKey(int index) {
    return QStringLiteral("altitudeMarkers/%1/value").arg(index);
}

// ============================================================================
// Logbook Preferences
// ============================================================================
inline const QString LogbookColumnsVersion = QStringLiteral("logbook/columnsVersion");
inline const QString LogbookCacheSize = QStringLiteral("logbook/cacheSize");

// ============================================================================
// Analysis Preferences
// ============================================================================
inline const QString AnalysisMethod = QStringLiteral("analysis/method");

// ============================================================================
// Aerodynamics Preferences
// ============================================================================
inline const QString AeroMass = QStringLiteral("aero/mass");
inline const QString AeroArea = QStringLiteral("aero/area");

} // namespace PreferenceKeys
} // namespace FlySight

#endif // PREFERENCEKEYS_H
