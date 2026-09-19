#ifndef UNITCONVERSION_H
#define UNITCONVERSION_H

#include <QHash>
#include <QString>

namespace FlySight {

/**
 * @brief How one recorded unit text maps onto Viewer's internal unit.
 *
 * internal_value = recorded_value * scale + offset
 */
struct ConversionSpec {
    double scale;      // recorded * scale + offset = internal
    double offset;     // for affine transforms (unused for current units)
    QString siUnit;    // normalized label: the unit the converted values are expressed in
};

/**
 * @brief The unit normalization table: recorded unit text -> Viewer's internal unit.
 *
 * Used by the conversion layer at read time (src/conversion/sourceconversion.cpp),
 * never at import: the source layer keeps the unit text exactly as recorded.
 * The table is keyed on unit text (e.g. "g", "gauss", "(m/s)"), not on sensor
 * or column names. It also lists the labels released Viewer versions wrote into
 * logbook files ("m/s^2", "T", "degC"), which are already internal.
 */
class UnitConversion {
public:
    /**
     * @brief Get the conversion specification for a unit string.
     *
     * The lookup ignores surrounding whitespace. Unknown unit text is the
     * normal case for custom columns and is silent: the result is the identity
     * with the label returned verbatim (untrimmed).
     *
     * @param unitText The recorded unit text (e.g., "g", "gauss", "(m/s)")
     * @return ConversionSpec with scale, offset, and the normalized label.
     */
    static ConversionSpec getConversion(const QString& unitText) {
        const auto& table = lookup();
        auto it = table.constFind(unitText.trimmed());
        if (it != table.constEnd()) {
            return it.value();
        }
        return {1.0, 0.0, unitText};
    }

    /**
     * @brief Check whether a unit changes values (not only the label).
     *
     * @param unitText The recorded unit text
     * @return true if scale != 1.0 or offset != 0.0
     */
    static bool requiresConversion(const QString& unitText) {
        ConversionSpec spec = getConversion(unitText);
        return (spec.scale != 1.0) || (spec.offset != 0.0);
    }

private:
    /**
     * @brief Static lookup table mapping unit text strings to conversion specs.
     *
     * This table is initialized once on first access using a lambda.
     */
    static const QHash<QString, ConversionSpec>& lookup() {
        static QHash<QString, ConversionSpec> table = []() {
            QHash<QString, ConversionSpec> t;

            // === Units already internal (scale=1, offset=0) ===
            t["m"]      = {1.0, 0.0, "m"};
            t["m/s"]    = {1.0, 0.0, "m/s"};
            t["Pa"]     = {1.0, 0.0, "Pa"};
            t["s"]      = {1.0, 0.0, "s"};
            t["deg"]    = {1.0, 0.0, "deg"};
            t["deg/s"]  = {1.0, 0.0, "deg/s"};
            t["V"]      = {1.0, 0.0, "V"};
            t["%"]      = {1.0, 0.0, "%"};
            t[""]       = {1.0, 0.0, ""};           // dimensionless (time, numSV, week)
            t["deg C"]  = {1.0, 0.0, "degC"};       // keep Celsius, don't convert to Kelvin

            // === Internal labels written by released Viewer versions into logbook files ===
            t["m/s^2"]  = {1.0, 0.0, "m/s^2"};
            t["T"]      = {1.0, 0.0, "T"};
            t["degC"]   = {1.0, 0.0, "degC"};

            // === Units requiring conversion ===
            t["g"]      = {9.80665, 0.0, "m/s^2"};  // acceleration: g -> m/s^2
            t["gauss"]  = {0.0001, 0.0, "T"};       // magnetic field: gauss -> Tesla

            // === Aliases for FS1 parenthesized format ===
            t["(m)"]    = t["m"];
            t["(m/s)"]  = t["m/s"];
            t["(deg)"]  = t["deg"];

            // === Aliases for common variations ===
            t["volt"]    = t["V"];
            t["percent"] = t["%"];

            return t;
        }();
        return table;
    }
};

} // namespace FlySight

#endif // UNITCONVERSION_H
