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
    /// Placeholder Viewer stores as DEVICE_ID when a new session's file records
    /// none and no FLYSIGHT.TXT is found. Not a recorded fact: a merge treats
    /// it as absent on either side.
    constexpr char DeviceIdUnknown[] = "n/a";
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

/// One column of the source layer: samples and unit text exactly as recorded.
struct SourceColumn {
    QVector<double> samples;
    QString unit;               ///< recorded unit text, verbatim

    bool operator==(const SourceColumn &other) const
    {
        return samples == other.samples && unit == other.unit;
    }
    bool operator!=(const SourceColumn &other) const { return !(*this == other); }
};
using SourceSensor = QMap<QString, SourceColumn>;   ///< measurement name -> column
using SourceData   = QMap<QString, SourceSensor>;   ///< sensor name -> columns

/// One session: stored attributes, source measurements with their unit text,
/// and - through a per-session CalculationEngine - every calculated value.
///
/// A measurement has one name and two layers behind it:
///  - the SOURCE layer holds samples and unit text exactly as recorded. It is
///    read only through the explicit source accessors, which never compute and
///    never fall back to a derived value;
///  - the EFFECTIVE layer is what ordinary reads (getMeasurement) return: the
///    source passed through the built-in conversion layer (schema correction,
///    then unit normalization), or a derived value when there is no source.
///
/// Ordinary attribute reads return the stored attribute when present and a
/// calculated one otherwise. Enumeration and presence queries describe stored
/// source data and stored attributes only.
///
/// Copying copies the stored state only; the copy computes its own values on
/// demand, from a cold cache (so never copy a session just to read it). Copy
/// assignment clears the target's cache without notifying its invalidation
/// listener. Moving carries the engine (its cache and its invalidation listener)
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

    // ---- enumeration: stored source data only (never calculation outputs) ----
    QStringList sensorKeys() const;
    bool hasSensor(const QString &key) const;
    QStringList measurementKeys(const QString &sensorKey) const;
    bool hasMeasurement(const QString& sensorKey, const QString& measurementKey) const;  ///< == hasSourceMeasurement

    // ---- effective layer: ordinary consumers; may run calculations ----------
    /// Effective samples: the conversion layer's output for a measurement with
    /// source data, a derived value otherwise, empty when unavailable.
    QVector<double> getMeasurement(const QString& sensorKey, const QString& measurementKey) const;
    /// The unit the effective samples are expressed in: the normalized label
    /// for a measurement with source data; for a derived measurement whatever
    /// its calculation reported ("" = not reported); "" when unavailable.
    QString effectiveUnit(const QString& sensorKey, const QString& measurementKey) const;

    // ---- source layer: never computes, never touches the engine's cache ------
    // (hasSourceMeasurement / sourceMeasurement / sourceUnit are declared with
    // the ISessionState overrides below.)
    /// Everything in the source layer. Shares the sample buffers.
    SourceData sourceData() const;
    /// Samples and unit text move together.
    QSet<DependencyKey> setSourceMeasurement(const QString& sensorKey, const QString& measurementKey,
                                             const QVector<double>& samples, const QString& unit);
    /// Bulk source-set path (import, merge): every incoming column replaces the
    /// same-named column or is added, samples and unit together; columns and
    /// sensors not mentioned are kept. Shares the incoming sample buffers.
    QSet<DependencyKey> mergeSourceData(const SourceData &incoming);
    /// Replaces the source samples; the recorded unit text is kept.
    QSet<DependencyKey> setMeasurement(const QString& sensorKey, const QString& measurementKey, const QVector<double>& data);
    /// Replaces the recorded unit text. A measurement without source data has
    /// no unit to set: warns, stores nothing, returns an empty set.
    QSet<DependencyKey> setUnit(const QString& sensorKey, const QString& measurementKey, const QString& unitString);

    /// This session's engine, created on first use and bound to
    /// CalculationRegistry::instance(). For SessionModel (invalidation
    /// listener) and tests (run counts, the fresh-evaluation oracle).
    CalculationEngine &calculationEngine() const;

    /// Builds an interpolation key: "{timeAttr}:{sensor}/{timeVector}/{dataVector}"
    static QString interpolationKey(const QString &timeAttr,
                                    const QString &sensor,
                                    const QString &timeVector,
                                    const QString &dataVector);

    // ISessionState: pure reads of the stored state, for the engine - and the
    // explicit source accessors for everything that must see recorded data
    // (exporter, merge). They never compute and never create the engine.
    // Absence is reported by value: false / empty vector / empty string. The
    // source of a purely derived name (e.g. IMU/wTotal) is absent.
    bool hasStoredAttribute(const QString &key) const override;
    QVariant storedAttribute(const QString &key) const override;
    bool hasSourceMeasurement(const QString &sensor, const QString &name) const override;
    QVector<double> sourceMeasurement(const QString &sensor, const QString &name) const override;
    QString sourceUnit(const QString &sensor, const QString &name) const override;   ///< "" unless hasSourceMeasurement

private:
    bool m_visible = false;
    QMap<QString, QVariant> m_attributes;
    QMap<QString, QMap<QString, QVector<double>>> m_sensors;
    QMap<QString, QMap<QString, QString>> m_units;

    // Lazily created. It points back at this object, so it is rebound when it
    // moves with the session and is never shared or copied.
    mutable std::unique_ptr<CalculationEngine> m_engine;
};

} // namespace FlySight

#endif // SESSIONDATA_H
