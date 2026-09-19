#ifndef SESSIONDATA_H
#define SESSIONDATA_H

#include <QList>
#include <QMap>
#include <QString>
#include <QVector>
#include <QVariant>
#include <QSet>
#include <QStringList>
#include <memory>
#include "dependencykey.h"
#include "engine/sessionstate.h"

namespace FlySight {

class CalculationEngine;

namespace SessionKeys {
    constexpr char DeviceId[] = "DEVICE_ID";
    constexpr char SessionId[] = "SESSION_ID";
    constexpr char Description[] = "_DESCRIPTION";
    constexpr char ImportTime[] = "_IMPORT_TIME";
    constexpr char Time[] = "_time";
    constexpr char SystemTime[] = "_system_time";
    constexpr char TimeFitA[] = "_TIME_FIT_A";
    constexpr char TimeFitB[] = "_TIME_FIT_B";
    constexpr char ExitTime[] = "_EXIT_TIME";
    constexpr char SyncTime[] = "_SYNC_TIME";
    constexpr char StartTime[] = "_START_TIME";
    constexpr char Duration[] = "_DURATION";
    constexpr char GroundElev[] = "_GROUND_ELEV";
    constexpr char MaxVelDTime[] = "_MAX_VELD_TIME";
    constexpr char ManoeuvreStartTime[] = "_MANOEUVRE_START_TIME";
    constexpr char MaxVelHTime[] = "_MAX_VELH_TIME";
    constexpr char LandingTime[] = "_LANDING_TIME";

    // Flare detection keys
    constexpr char FlareStartTime[] = "_FLARE_START_TIME";
    constexpr char FlareEndTime[]   = "_FLARE_END_TIME";

    // Analysis range keys
    constexpr char AnalysisStartTime[] = "_ANALYSIS_START_TIME";
    constexpr char AnalysisEndTime[]   = "_ANALYSIS_END_TIME";

    // Wingsuit Performance (WS-P) parameter keys
    constexpr char WspVersion[]      = "_WSP_VERSION";
    constexpr char WspTopAlt[]       = "_WSP_TOP_ALT";
    constexpr char WspBottomAlt[]    = "_WSP_BOTTOM_ALT";
    constexpr char WspTask[]         = "_WSP_TASK";

    // Wingsuit Performance (WS-P) result keys
    constexpr char WspEntryTime[]    = "_WSP_ENTRY_TIME";
    constexpr char WspExitTime[]     = "_WSP_EXIT_TIME";
    constexpr char WspEntryLat[]     = "_WSP_ENTRY_LAT";
    constexpr char WspEntryLon[]     = "_WSP_ENTRY_LON";
    constexpr char WspExitLat[]      = "_WSP_EXIT_LAT";
    constexpr char WspExitLon[]      = "_WSP_EXIT_LON";
    constexpr char WspTimeResult[]   = "_WSP_TIME_RESULT";
    constexpr char WspDistResult[]   = "_WSP_DIST_RESULT";
    constexpr char WspSpeedResult[]  = "_WSP_SPEED_RESULT";
    constexpr char WspSepResult[]    = "_WSP_SEP_RESULT";

    // Wingsuit Performance (WS-P) lane reference keys
    constexpr char WspRef1Time[]     = "_WSP_REF1_TIME";

    // Speed Skydiving (SP) parameter keys
    constexpr char SpPerfWindowHeight[] = "_SP_PERF_WINDOW_HEIGHT";
    constexpr char SpValWindowHeight[]  = "_SP_VAL_WINDOW_HEIGHT";
    constexpr char SpBreakoffAlt[]      = "_SP_BREAKOFF_ALT";

    // Speed Skydiving (SP) result keys
    constexpr char SpWindowStartTime[] = "_SP_WINDOW_START_TIME";
    constexpr char SpWindowStartAlt[]  = "_SP_WINDOW_START_ALT";
    constexpr char SpWindowEndTime[]   = "_SP_WINDOW_END_TIME";
    constexpr char SpBestStartTime[]   = "_SP_BEST_START_TIME";
    constexpr char SpBestEndTime[]     = "_SP_BEST_END_TIME";
    constexpr char SpSpeedResult[]     = "_SP_SPEED_RESULT";
    constexpr char SpMaxSpeedAcc[]     = "_SP_MAX_SPEED_ACC";

    // Wind and course reference keys
    constexpr char WindN[] = "_WIND_N";
    constexpr char WindE[] = "_WIND_E";
    constexpr char CourseRef[] = "_COURSE_REF";
    constexpr char JumperMass[] = "_JUMPER_MASS";
    constexpr char PlanformArea[] = "_PLANFORM_AREA";
}

/// One session: stored attributes, stored measurements with their unit text,
/// and - through a per-session CalculationEngine - every calculated value.
///
/// Ordinary reads (getAttribute / getMeasurement) are resolved by the engine:
/// stored data first, then the registered calculations. Enumeration and
/// presence queries describe stored data only.
///
/// Copying copies the stored state only; the copy computes its own values on
/// demand. Moving carries the engine (its cache and its invalidation listener)
/// to the new object.
class SessionData : public ISessionState {
public:
    SessionData();
    SessionData(const SessionData &other);                 ///< state only: no engine, no cache, no listener
    SessionData(SessionData &&other) noexcept;             ///< state and engine
    SessionData &operator=(const SessionData &other);      ///< state only; keeps its own engine, cleared
    SessionData &operator=(SessionData &&other) noexcept;  ///< state and engine
    ~SessionData() override;

    bool isVisible() const;
    void setVisible(bool visible);

    // Attributes. attributeKeys / hasAttribute describe stored attributes only.
    QStringList attributeKeys() const;
    bool hasAttribute(const QString &key) const;
    QVariant getAttribute(const QString &key) const;        ///< stored, else calculated
    QSet<DependencyKey> setAttribute(const QString &key, const QVariant &value);
    QSet<DependencyKey> removeAttribute(const QString &key);

    // Measurements. The enumeration and presence queries describe stored data only.
    QStringList sensorKeys() const;
    bool hasSensor(const QString &key) const;
    QStringList measurementKeys(const QString &sensorKey) const;
    bool hasMeasurement(const QString& sensorKey, const QString& measurementKey) const;
    QVector<double> getMeasurement(const QString& sensorKey, const QString& measurementKey) const;  ///< stored, else calculated
    QSet<DependencyKey> setMeasurement(const QString& sensorKey, const QString& measurementKey, const QVector<double>& data);

    QSet<DependencyKey> setUnit(const QString& sensorKey, const QString& measurementKey, const QString& unitString);
    QString getUnit(const QString& sensorKey, const QString& measurementKey) const;
    QMap<QString, QString> units(const QString& sensorKey) const;

    /// Drops every calculated value. For code that changed the stored state
    /// without going through the setters (DataImporter).
    QSet<DependencyKey> invalidateAllCalculations();

    /// This session's engine, created on first use and bound to
    /// CalculationRegistry::instance(). For SessionModel (invalidation
    /// listener) and tests (run counts, the fresh-evaluation oracle).
    CalculationEngine &calculationEngine() const;

    /// Builds an interpolation key: "{timeAttr}:{sensor}/{timeVector}/{dataVector}"
    static QString interpolationKey(const QString &timeAttr,
                                    const QString &sensor,
                                    const QString &timeVector,
                                    const QString &dataVector);

    // ISessionState: pure reads of the stored state, for the engine.
    bool hasStoredAttribute(const QString &key) const override;
    QVariant storedAttribute(const QString &key) const override;
    bool hasSourceMeasurement(const QString &sensor, const QString &name) const override;
    QVector<double> sourceMeasurement(const QString &sensor, const QString &name) const override;
    QString sourceUnit(const QString &sensor, const QString &name) const override;

private:
    bool m_visible = false;
    QMap<QString, QVariant> m_attributes;
    QMap<QString, QMap<QString, QVector<double>>> m_sensors;
    QMap<QString, QMap<QString, QString>> m_units;

    // Lazily created. It points back at this object, so it is rebound when it
    // moves with the session and is never shared or copied.
    mutable std::unique_ptr<CalculationEngine> m_engine;

    friend class DataImporter;
};

} // namespace FlySight

#endif // SESSIONDATA_H
