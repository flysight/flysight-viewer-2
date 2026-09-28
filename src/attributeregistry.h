#ifndef ATTRIBUTEREGISTRY_H
#define ATTRIBUTEREGISTRY_H

#include <QString>
#include <QVector>

namespace FlySight {

enum class AttributeFormatType {
    Text,       // Raw string display
    DateTime,   // UTC seconds -> formatted date/time
    Duration,   // Seconds -> mm:ss display
    Double,     // Numeric double -> string display
    Choice      // A token of the definition's choices -> its label
};

/// One allowed value of a Choice attribute: the token the session stores and
/// the label the logbook shows for it.
struct AttributeChoice {
    QString token;
    QString label;
};

/// One attribute the logbook can show as a column. An aggregate: registrations
/// brace-initialize it, and a member they leave out takes its default.
///
/// A Choice definition lists its allowed values in `choices`, in presentation
/// order. The list is not empty, its tokens are non-empty and unique, and its
/// labels are unique and none is "Default", which the editors offer for
/// removing the stored value. A definition of any other type has an empty
/// list. The registry does not check any of this: the definitions are program
/// constants.
struct AttributeDefinition {
    QString category;          // UI grouping (e.g., "Session", "Location")
    QString displayName;       // Human-readable name (e.g., "Start Time")
    QString attributeKey;      // SessionKeys constant this reads from
    AttributeFormatType formatType; // How to format and sort the value
    bool editable = false;     // Whether the user can edit this value in the logbook
    QString measurementType;   // UnitConverter key (empty = no unit conversion)
    QVector<AttributeChoice> choices; // Choice only: the allowed values, in presentation order

    /// The choice whose token is `token`; nullptr for a token outside the
    /// list, and so for every token of a definition that is not a Choice.
    /// The one lookup from a stored token to its choice.
    const AttributeChoice *findChoice(const QString &token) const;
};

class AttributeRegistry {
public:
    static AttributeRegistry& instance();

    /// Register one attribute definition
    void registerAttribute(const AttributeDefinition& def);

    /// Returns all registered attribute definitions
    QVector<AttributeDefinition> allAttributes() const;

    /// Find an attribute by its key. Returns nullptr if not found.
    const AttributeDefinition* findByKey(const QString &attributeKey) const;

private:
    QVector<AttributeDefinition> m_attributes;
};

} // namespace FlySight

#endif // ATTRIBUTEREGISTRY_H
