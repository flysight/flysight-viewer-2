#include <QFormLayout>
#include <QVBoxLayout>
#include "zoomsettingspage.h"
#include "preferencekeys.h"
#include "preferencepagestyle.h"
#include "preferencesmanager.h"
#include "markerregistry.h"

namespace FlySight {

ZoomSettingsPage::ZoomSettingsPage(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    layout->addWidget(createExtentGroup());
    layout->addStretch();

    // Wire up saves
    connect(m_allDataRadio, &QRadioButton::toggled, this, &ZoomSettingsPage::saveSettings);
    connect(m_markerRangeRadio, &QRadioButton::toggled, this, &ZoomSettingsPage::saveSettings);
    connect(m_startMarkerCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &ZoomSettingsPage::saveSettings);
    connect(m_endMarkerCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &ZoomSettingsPage::saveSettings);
    connect(m_marginSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ZoomSettingsPage::saveSettings);

    // Enable/disable marker combos based on mode
    connect(m_allDataRadio, &QRadioButton::toggled, this, &ZoomSettingsPage::updateMarkerCombosEnabled);
}

// Two modes; the two markers belong to the marker-range one, so their rows
// sit under that button; the margin applies in both, so it follows them.
QGroupBox* ZoomSettingsPage::createExtentGroup()
{
    QGroupBox *group = new QGroupBox(tr("Zoom to extent"), this);
    QFormLayout *form = new QFormLayout(group);

    m_allDataRadio = new QRadioButton(tr("All data"), this);
    m_markerRangeRadio = new QRadioButton(tr("Marker range"), this);
    m_startMarkerCombo = new QComboBox(this);
    m_endMarkerCombo = new QComboBox(this);

    m_marginSpinBox = new QDoubleSpinBox(this);
    m_marginSpinBox->setRange(0.0, 25.0);
    m_marginSpinBox->setSingleStep(1.0);
    m_marginSpinBox->setDecimals(1);
    m_marginSpinBox->setSuffix(tr("%"));

    form->addRow(m_allDataRadio);
    form->addRow(m_markerRangeRadio);
    form->addRow(PreferencePage::subordinateLabel(tr("Start marker:"), this), m_startMarkerCombo);
    form->addRow(PreferencePage::subordinateLabel(tr("End marker:"), this), m_endMarkerCombo);
    form->addRow(tr("Margin:"), m_marginSpinBox);

    // Populate combos from MarkerRegistry
    populateMarkerCombos();

    // Initialize from preferences
    PreferencesManager &prefs = PreferencesManager::instance();
    QString mode = prefs.getValue(PreferenceKeys::ZoomExtentMode).toString();
    if (mode == "allData") {
        m_allDataRadio->setChecked(true);
    } else {
        m_markerRangeRadio->setChecked(true);
    }

    // Select saved markers
    QString startKey = prefs.getValue(PreferenceKeys::ZoomExtentStartMarker).toString();
    QString endKey = prefs.getValue(PreferenceKeys::ZoomExtentEndMarker).toString();

    int startIdx = m_startMarkerCombo->findData(startKey);
    if (startIdx >= 0) m_startMarkerCombo->setCurrentIndex(startIdx);

    int endIdx = m_endMarkerCombo->findData(endKey);
    if (endIdx >= 0) m_endMarkerCombo->setCurrentIndex(endIdx);

    m_marginSpinBox->setValue(prefs.getValue(PreferenceKeys::ZoomExtentMarginPct).toDouble());

    updateMarkerCombosEnabled();

    return group;
}

void ZoomSettingsPage::populateMarkerCombos()
{
    const QVector<MarkerDefinition> markers = MarkerRegistry::instance()->allMarkers();
    for (const MarkerDefinition &def : markers) {
        QString label = def.displayName;
        if (!def.category.isEmpty()) {
            label = QStringLiteral("%1 / %2").arg(def.category, def.displayName);
        }
        m_startMarkerCombo->addItem(label, def.attributeKey);
        m_endMarkerCombo->addItem(label, def.attributeKey);
    }
}

void ZoomSettingsPage::updateMarkerCombosEnabled()
{
    bool enabled = m_markerRangeRadio->isChecked();
    m_startMarkerCombo->setEnabled(enabled);
    m_endMarkerCombo->setEnabled(enabled);
}

void ZoomSettingsPage::saveSettings()
{
    PreferencesManager &prefs = PreferencesManager::instance();

    prefs.setValue(PreferenceKeys::ZoomExtentMode,
                   m_allDataRadio->isChecked() ? QStringLiteral("allData") : QStringLiteral("markerRange"));
    prefs.setValue(PreferenceKeys::ZoomExtentStartMarker,
                   m_startMarkerCombo->currentData().toString());
    prefs.setValue(PreferenceKeys::ZoomExtentEndMarker,
                   m_endMarkerCombo->currentData().toString());
    prefs.setValue(PreferenceKeys::ZoomExtentMarginPct, m_marginSpinBox->value());
}

} // namespace FlySight
