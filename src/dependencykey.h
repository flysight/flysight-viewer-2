#ifndef DEPENDENCYKEY_H
#define DEPENDENCYKEY_H

#include <QString>
#include <QPair>
#include <QHash>  // Needed for qHash

namespace FlySight {

struct DependencyKey {
    enum class Type { Attribute, Measurement };

    Type type;

    // Valid if type == Attribute
    QString attributeKey;

    // Valid if type == Measurement
    QPair<QString, QString> measurementKey;

    // Static factory methods for clarity
    static DependencyKey attribute(const QString& key) {
        DependencyKey dk;
        dk.type = Type::Attribute;
        dk.attributeKey = key;
        return dk;
    }

    static DependencyKey measurement(const QString& sensorKey, const QString& measurementKey) {
        DependencyKey dk;
        dk.type = Type::Measurement;
        dk.measurementKey = qMakePair(sensorKey, measurementKey);
        return dk;
    }
};

// Equality operator
inline bool operator==(const DependencyKey &lhs, const DependencyKey &rhs) {
    if (lhs.type != rhs.type)
        return false;
    if (lhs.type == DependencyKey::Type::Attribute)
        return lhs.attributeKey == rhs.attributeKey;
    return lhs.measurementKey == rhs.measurementKey;
}

// Less-than operator for sorted containers (e.g. QMap)
inline bool operator<(const DependencyKey &lhs, const DependencyKey &rhs) {
    if (static_cast<int>(lhs.type) != static_cast<int>(rhs.type))
        return static_cast<int>(lhs.type) < static_cast<int>(rhs.type);

    if (lhs.type == DependencyKey::Type::Attribute)
        return lhs.attributeKey < rhs.attributeKey;
    // For measurement keys, compare the first, then the second.
    if (lhs.measurementKey.first != rhs.measurementKey.first)
        return lhs.measurementKey.first < rhs.measurementKey.first;
    return lhs.measurementKey.second < rhs.measurementKey.second;
}

// qHash overload so that DependencyKey can be used in QHash and QSet.
inline uint qHash(const DependencyKey &key, uint seed = 0) {
    // Use the global ::qHash for built-in types to avoid ADL conflicts.
    uint result = ::qHash(static_cast<int>(key.type), seed);
    if (key.type == DependencyKey::Type::Attribute) {
        result = ::qHash(key.attributeKey, result);
    } else { // Measurement
        result = ::qHash(key.measurementKey.first, result);
        result = ::qHash(key.measurementKey.second, result);
    }
    return result;
}

} // namespace FlySight

#endif // DEPENDENCYKEY_H
