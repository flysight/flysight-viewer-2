#include <QFormLayout>
#include <QVBoxLayout>
#include "aerodynamicssettingspage.h"
#include "preferencekeys.h"
#include "preferencesmanager.h"

namespace FlySight {

AerodynamicsSettingsPage::AerodynamicsSettingsPage(QWidget *parent)
    : QWidget(parent) {
    QVBoxLayout *layout = new QVBoxLayout(this);

    layout->addWidget(createBodyGroup());
    layout->addStretch();

    connect(m_massSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &AerodynamicsSettingsPage::saveSettings);
    connect(m_areaSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &AerodynamicsSettingsPage::saveSettings);
}

QGroupBox* AerodynamicsSettingsPage::createBodyGroup() {
    QGroupBox *group = new QGroupBox(tr("Default body parameters (used on import)"), this);
    QFormLayout *formLayout = new QFormLayout(group);

    m_massSpinBox = new QDoubleSpinBox(this);
    m_massSpinBox->setRange(0.1, 500.0);
    m_massSpinBox->setDecimals(1);
    m_massSpinBox->setSuffix(tr(" kg"));
    m_massSpinBox->setSingleStep(0.1);

    m_areaSpinBox = new QDoubleSpinBox(this);
    m_areaSpinBox->setRange(0.01, 100.0);
    m_areaSpinBox->setDecimals(2);
    m_areaSpinBox->setSuffix(QString::fromUtf8(" m\u00B2"));
    m_areaSpinBox->setSingleStep(0.01);

    formLayout->addRow(tr("Mass:"), m_massSpinBox);
    formLayout->addRow(tr("Planform area:"), m_areaSpinBox);

    // Initialize from preferences
    PreferencesManager &prefs = PreferencesManager::instance();
    m_massSpinBox->setValue(prefs.getValue(PreferenceKeys::AeroMass).toDouble());
    m_areaSpinBox->setValue(prefs.getValue(PreferenceKeys::AeroArea).toDouble());

    return group;
}

void AerodynamicsSettingsPage::saveSettings() {
    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.setValue(PreferenceKeys::AeroMass, m_massSpinBox->value());
    prefs.setValue(PreferenceKeys::AeroArea, m_areaSpinBox->value());
}

} // namespace FlySight
