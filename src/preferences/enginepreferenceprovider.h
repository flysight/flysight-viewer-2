#ifndef ENGINEPREFERENCEPROVIDER_H
#define ENGINEPREFERENCEPROVIDER_H

#include <memory>
#include <vector>

#include <QObject>
#include <QString>
#include <QVariant>

#include "../engine/calculationregistry.h"
#include "../engine/sessionstate.h"

namespace FlySight {

/// Adapts PreferencesManager to the calculation engine: pull access for
/// preferences that calculations declare as inputs, and push notification of
/// their changes to every engine enrolled with a registry.
///
/// Lives in flysight_core; the engine itself (flysight_model) knows nothing
/// about PreferencesManager.
class EnginePreferenceProvider : public QObject, public IPreferenceProvider {
    Q_OBJECT
public:
    /// Idempotent. Sets the provider on the registry and connects
    /// PreferencesManager::preferenceChanged to
    /// registry.notifyPreferenceChanged(key). The connection is direct, so
    /// dependents are invalidated inside PreferencesManager::setValue, before
    /// it returns. Call after the preferences have been registered.
    ///
    /// A registry that does not live for the whole process (a private registry
    /// in a test) must be passed to uninstall() before it is destroyed.
    static void install(CalculationRegistry &registry = CalculationRegistry::instance());

    /// Disconnects and removes the adapter installed for `registry`, and
    /// clears the registry's provider. No-op when none was installed.
    static void uninstall(CalculationRegistry &registry);

    /// An unregistered key is "no such preference" (invalid QVariant); it
    /// never reaches PreferencesManager::getValue, which asserts on it.
    QVariant preferenceValue(const QString &key) const override;

private:
    explicit EnginePreferenceProvider(CalculationRegistry &registry);

    static std::vector<std::unique_ptr<EnginePreferenceProvider>> &adapters();

    CalculationRegistry *m_registry;
};

} // namespace FlySight

#endif // ENGINEPREFERENCEPROVIDER_H
