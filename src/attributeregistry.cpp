#include "attributeregistry.h"

using namespace FlySight;

const AttributeChoice *AttributeDefinition::findChoice(const QString &token) const
{
    for (const AttributeChoice &choice : choices) {
        if (choice.token == token)
            return &choice;
    }
    return nullptr;
}

AttributeRegistry& AttributeRegistry::instance() {
    static AttributeRegistry R;
    return R;
}

void AttributeRegistry::registerAttribute(const AttributeDefinition& def) {
    m_attributes.append(def);
}

QVector<AttributeDefinition> AttributeRegistry::allAttributes() const {
    return m_attributes;
}

const AttributeDefinition* AttributeRegistry::findByKey(const QString &attributeKey) const {
    for (const auto &def : m_attributes) {
        if (def.attributeKey == attributeKey)
            return &def;
    }
    return nullptr;
}
