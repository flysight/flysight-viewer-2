#include "sessiondata.h"
#include "dependencykey.h"
#include "engine/calculationengine.h"
#include <QDebug>
#include <utility>

namespace FlySight {

SessionData::SessionData() = default;

SessionData::~SessionData() = default;

// A copy has the same stored state and nothing else: it gets its own engine,
// cold, on its first calculated read. The original's cache and invalidation
// listener stay with the original.
SessionData::SessionData(const SessionData &other)
    : m_visible(other.m_visible)
    , m_attributes(other.m_attributes)
    , m_sensors(other.m_sensors)
    , m_units(other.m_units)
{
}

// A move is the same session at a new address: the engine comes along with its
// cache and listener, and is told where its state now lives. The moved-from
// object is left without an engine and lazily gets a new one if it is reused.
SessionData::SessionData(SessionData &&other) noexcept
    : m_visible(other.m_visible)
    , m_attributes(std::move(other.m_attributes))
    , m_sensors(std::move(other.m_sensors))
    , m_units(std::move(other.m_units))
    , m_engine(std::move(other.m_engine))
{
    if (m_engine)
        m_engine->rebind(this);
}

SessionData &SessionData::operator=(const SessionData &other)
{
    if (this == &other)
        return *this;

    m_visible = other.m_visible;
    m_attributes = other.m_attributes;
    m_sensors = other.m_sensors;
    m_units = other.m_units;

    // Still this session (same engine, same listener), with new contents.
    if (m_engine)
        m_engine->clear();
    return *this;
}

SessionData &SessionData::operator=(SessionData &&other) noexcept
{
    if (this == &other)
        return *this;

    m_visible = other.m_visible;
    m_attributes = std::move(other.m_attributes);
    m_sensors = std::move(other.m_sensors);
    m_units = std::move(other.m_units);

    m_engine = std::move(other.m_engine);   // this object's own engine is destroyed
    if (m_engine)
        m_engine->rebind(this);
    return *this;
}

CalculationEngine &SessionData::calculationEngine() const
{
    if (!m_engine)
        m_engine = std::make_unique<CalculationEngine>(this);
    return *m_engine;
}

bool SessionData::isVisible() const {
    return m_visible;
}

void SessionData::setVisible(bool visible) {
    m_visible = visible;
}

QStringList SessionData::attributeKeys() const {
    return m_attributes.keys();
}

bool SessionData::hasAttribute(const QString &key) const {
    return m_attributes.contains(key);
}

QVariant SessionData::getAttribute(const QString &key) const {
    // One read path: the engine resolves the stored value first, then the
    // registered calculations.
    return calculationEngine().attribute(key);
}

QSet<DependencyKey> SessionData::setAttribute(const QString &key, const QVariant &value) {
    m_attributes.insert(key, value);

    // Without an engine nothing is cached, so only the name itself changed.
    if (!m_engine)
        return { DependencyKey::attribute(key) };
    return m_engine->attributeChanged(key);
}

QSet<DependencyKey> SessionData::removeAttribute(const QString &key) {
    // Remove the stored attribute (no-op if key is absent); dependents then
    // recompute from the calculated value.
    m_attributes.remove(key);

    if (!m_engine)
        return { DependencyKey::attribute(key) };
    return m_engine->attributeChanged(key);
}

QStringList SessionData::sensorKeys() const {
    return m_sensors.keys();
}

bool SessionData::hasSensor(const QString &key) const {
    return m_sensors.contains(key);
}

QStringList SessionData::measurementKeys(const QString &sensorKey) const {
    if (!m_sensors.contains(sensorKey)) return QStringList();
    return m_sensors.value(sensorKey).keys();
}

bool SessionData::hasMeasurement(const QString &sensorKey, const QString &measurementKey) const {
    auto sensorIt = m_sensors.find(sensorKey);
    if (sensorIt == m_sensors.end()) return false;
    return sensorIt.value().contains(measurementKey);
}

QVector<double> SessionData::getMeasurement(const QString &sensorKey, const QString &measurementKey) const {
    // One read path: a measurement with source data comes back as the
    // conversion layer's output (the implicitly shared source vector when the
    // conversion is the identity); anything else is calculated.
    return calculationEngine().measurement(sensorKey, measurementKey);
}

QString SessionData::effectiveUnit(const QString &sensorKey, const QString &measurementKey) const {
    return calculationEngine().measurementUnit(sensorKey, measurementKey);
}

SourceData SessionData::sourceData() const {
    SourceData data;
    for (auto sensorIt = m_sensors.constBegin(); sensorIt != m_sensors.constEnd(); ++sensorIt) {
        const QMap<QString, QString> units = m_units.value(sensorIt.key());
        SourceSensor &sensor = data[sensorIt.key()];
        for (auto it = sensorIt.value().constBegin(); it != sensorIt.value().constEnd(); ++it)
            sensor.insert(it.key(), SourceColumn{ it.value(), units.value(it.key()) });
    }
    return data;
}

QSet<DependencyKey> SessionData::setSourceMeasurement(const QString &sensorKey, const QString &measurementKey,
                                                      const QVector<double> &samples, const QString &unit) {
    m_sensors[sensorKey].insert(measurementKey, samples);
    m_units[sensorKey].insert(measurementKey, unit);

    if (!m_engine)
        return { DependencyKey::measurement(sensorKey, measurementKey) };
    QSet<DependencyKey> invalidated = m_engine->sourceMeasurementChanged(sensorKey, measurementKey);
    invalidated.unite(m_engine->sourceUnitChanged(sensorKey, measurementKey));
    return invalidated;
}

QSet<DependencyKey> SessionData::mergeSourceData(const SourceData &incoming) {
    QSet<DependencyKey> invalidated;
    for (auto sensorIt = incoming.constBegin(); sensorIt != incoming.constEnd(); ++sensorIt) {
        for (auto it = sensorIt.value().constBegin(); it != sensorIt.value().constEnd(); ++it) {
            invalidated.unite(setSourceMeasurement(sensorIt.key(), it.key(),
                                                   it.value().samples, it.value().unit));
        }
    }
    return invalidated;
}

QSet<DependencyKey> SessionData::setMeasurement(const QString &sensorKey, const QString &measurementKey, const QVector<double> &data) {
    m_sensors[sensorKey].insert(measurementKey, data);

    if (!m_engine)
        return { DependencyKey::measurement(sensorKey, measurementKey) };
    return m_engine->sourceMeasurementChanged(sensorKey, measurementKey);
}

QSet<DependencyKey> SessionData::setUnit(const QString &sensorKey, const QString &measurementKey, const QString &unitString) {
    // Unit text belongs to a source measurement; samples and unit exist together.
    if (!hasSourceMeasurement(sensorKey, measurementKey)) {
        qWarning().noquote() << "SessionData::setUnit:" << sensorKey + QLatin1Char('/') + measurementKey
                             << "has no source data; unit not stored";
        return {};
    }

    m_units[sensorKey][measurementKey] = unitString;

    if (!m_engine)
        return { DependencyKey::measurement(sensorKey, measurementKey) };
    return m_engine->sourceUnitChanged(sensorKey, measurementKey);
}

QString SessionData::interpolationKey(const QString &timeAttr,
                                      const QString &sensor,
                                      const QString &timeVector,
                                      const QString &dataVector)
{
    return timeAttr
        + QStringLiteral(":")
        + sensor
        + QStringLiteral("/")
        + timeVector
        + QStringLiteral("/")
        + dataVector;
}

// ---- ISessionState: pure reads of the stored state -------------------------

bool SessionData::hasStoredAttribute(const QString &key) const
{
    return m_attributes.contains(key);
}

QVariant SessionData::storedAttribute(const QString &key) const
{
    return m_attributes.value(key);
}

bool SessionData::hasSourceMeasurement(const QString &sensor, const QString &name) const
{
    return hasMeasurement(sensor, name);
}

QVector<double> SessionData::sourceMeasurement(const QString &sensor, const QString &name) const
{
    auto sensorIt = m_sensors.constFind(sensor);
    if (sensorIt == m_sensors.constEnd()) return QVector<double>();
    return sensorIt.value().value(name);
}

QString SessionData::sourceUnit(const QString &sensor, const QString &name) const
{
    // Unit text exists exactly when the measurement has source data.
    if (!hasSourceMeasurement(sensor, name))
        return QString();

    auto sensorIt = m_units.constFind(sensor);
    if (sensorIt == m_units.constEnd()) return QString();
    return sensorIt.value().value(name);
}

} // namespace FlySight
