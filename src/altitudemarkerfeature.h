#ifndef ALTITUDEMARKERFEATURE_H
#define ALTITUDEMARKERFEATURE_H

#include <QObject>
#include <QStringList>

#include "engine/calculationdescriptor.h"

namespace FlySight {

class SessionModel;

/// Owns the altitude markers configured in preferences: one marker definition
/// and one registered calculation ("builtin.altitude.<attributeKey>") per
/// altitude. The only component that registers calculations at run time.
class AltitudeMarkerManager : public QObject
{
    Q_OBJECT

public:
    explicit AltitudeMarkerManager(SessionModel *sessionModel, QObject *parent = nullptr);
    ~AltitudeMarkerManager() override;  ///< unregisters the calculations it registered

    void registerAll();
    void refresh();

    /// Identity of the calculation behind an altitude marker attribute,
    /// e.g. "builtin.altitude._ALTITUDE_300_FT".
    static CalculationId calculationId(const QString &attributeKey);

    /// The calculation for one altitude marker: the last downward crossing of
    /// `thresholdMetres` AGL within the analysis window. The attribute key
    /// encodes the altitude and its unit, so the threshold is part of the
    /// calculation's identity and is baked in rather than read as an input.
    static CalculationDescriptor makeDescriptor(const QString &attributeKey, double thresholdMetres);

private:
    /// Brings registrations and marker definitions in line with preferences.
    void apply();

    SessionModel *m_sessionModel;
    QStringList   m_registeredKeys;
};

} // namespace FlySight

#endif // ALTITUDEMARKERFEATURE_H
