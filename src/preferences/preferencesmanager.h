#ifndef PREFERENCESMANAGER_H
#define PREFERENCESMANAGER_H

#include <QObject>
#include <QSettings>
#include <QVariant>

namespace FlySight {

struct Preference {
    QVariant defaultValue;
};

class PreferencesManager : public QObject {
    Q_OBJECT

public:
    static PreferencesManager& instance() {
        static PreferencesManager instance;
        return instance;
    }

    void registerPreference(const QString &key, const QVariant &defaultValue) {
        m_preferences[key] = Preference{defaultValue};

        // Set the default value in QSettings if the key doesn't exist yet
        if (!m_settings.contains(key)) {
            m_settings.setValue(key, defaultValue);
        }
    }

    bool hasPreference(const QString &key) const { return m_preferences.contains(key); }

    QVariant getValue(const QString &key) const {
        if (!m_preferences.contains(key)) {
            qWarning() << "Requested value for an unregistered preference:" << key;
            Q_ASSERT(false && "Requested value for an unregistered preference!");
        }
        return m_settings.value(key, m_preferences.value(key).defaultValue);
    }

    QVariant getDefaultValue(const QString &key) const {
        if (!m_preferences.contains(key)) {
            qWarning() << "Requested default value for an unregistered preference:" << key;
            return QVariant();
        }
        return m_preferences.value(key).defaultValue;
    }

    // Sets the value for a registered preference key and emits preferenceChanged()
    // if the value actually changed.
    //
    // IMPORTANT — preferenceChanged() is emitted synchronously: all connected slots
    // run to completion before setValue() returns. If a slot reads back from QSettings
    // (e.g. via beginReadArray), it sees exactly the state that exists at the moment
    // the slot runs.
    //
    // Consequence for settings pages that save both a QSettings array and scalar
    // preference keys under the same namespace: always write the QSettings array
    // BEFORE calling setValue() for the scalar keys. If you call setValue() first,
    // any preferenceChanged handler that reads the array may read a stale copy.
    // See AltitudeMarkersSettingsPage::saveSettings() for a worked example.
    void setValue(const QString &key, const QVariant &value) {
        if (m_settings.value(key) != value) {
            m_settings.setValue(key, value);
            emit preferenceChanged(key, value);
        }
    }

signals:
    void preferenceChanged(const QString &key, const QVariant &value);

private:
    PreferencesManager() : m_settings() {}

    QSettings m_settings;
    QMap<QString, Preference> m_preferences;

    Q_DISABLE_COPY(PreferencesManager)
};

} // namespace FlySight

#endif // PREFERENCESMANAGER_H
