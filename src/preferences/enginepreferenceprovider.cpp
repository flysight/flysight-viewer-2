#include "enginepreferenceprovider.h"

#include <memory>
#include <vector>

#include "preferencesmanager.h"

namespace FlySight {

EnginePreferenceProvider::EnginePreferenceProvider(CalculationRegistry &registry)
    : m_registry(&registry)
{
    connect(&PreferencesManager::instance(), &PreferencesManager::preferenceChanged,
            this, [this](const QString &key, const QVariant &) {
                m_registry->notifyPreferenceChanged(key);
            },
            Qt::DirectConnection);
}

std::vector<std::unique_ptr<EnginePreferenceProvider>> &EnginePreferenceProvider::adapters()
{
    // One adapter per registry. In the application there is exactly one (the
    // global registry), kept for the life of the process.
    static std::vector<std::unique_ptr<EnginePreferenceProvider>> list;
    return list;
}

void EnginePreferenceProvider::install(CalculationRegistry &registry)
{
    for (const auto &adapter : adapters()) {
        if (adapter->m_registry == &registry) {
            registry.setPreferenceProvider(adapter.get());
            return;
        }
    }

    adapters().emplace_back(new EnginePreferenceProvider(registry));
    registry.setPreferenceProvider(adapters().back().get());
}

void EnginePreferenceProvider::uninstall(CalculationRegistry &registry)
{
    auto &list = adapters();
    for (auto it = list.begin(); it != list.end(); ++it) {
        if ((*it)->m_registry == &registry) {
            registry.setPreferenceProvider(nullptr);
            list.erase(it);     // destroying the QObject drops the connection
            return;
        }
    }
}

QVariant EnginePreferenceProvider::preferenceValue(const QString &key) const
{
    const PreferencesManager &prefs = PreferencesManager::instance();
    if (!prefs.hasPreference(key))
        return QVariant();
    return prefs.getValue(key);
}

} // namespace FlySight
