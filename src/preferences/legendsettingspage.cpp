#include "legendsettingspage.h"
#include "preferencesmanager.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QMessageBox>

namespace FlySight {

// Preference key constants
static const QString KEY_TEXT_SIZE = QStringLiteral("legend/textSize");

// Default values
static const int DEFAULT_TEXT_SIZE = 9;

LegendSettingsPage::LegendSettingsPage(QWidget *parent)
    : QWidget(parent)
{
    // Register preferences with defaults
    PreferencesManager::instance().registerPreference(KEY_TEXT_SIZE, DEFAULT_TEXT_SIZE);

    // Build UI
    QVBoxLayout *layout = new QVBoxLayout(this);

    layout->addWidget(createTextSettingsGroup());
    layout->addStretch();
    layout->addWidget(createResetSection());

    // Load current settings
    loadSettings();

    // Connect signals for immediate save
    connect(m_textSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &LegendSettingsPage::saveSettings);

    // Connect reset button
    connect(m_resetButton, &QPushButton::clicked,
            this, &LegendSettingsPage::resetToDefaults);
}

QGroupBox* LegendSettingsPage::createTextSettingsGroup()
{
    QGroupBox *group = new QGroupBox(tr("Legend text"), this);
    QFormLayout *form = new QFormLayout(group);

    m_textSizeSpinBox = new QSpinBox(this);
    m_textSizeSpinBox->setRange(6, 24);
    m_textSizeSpinBox->setSuffix(tr(" pt"));
    m_textSizeSpinBox->setToolTip(tr("Font size for legend text (6-24 points)"));
    form->addRow(tr("Text size:"), m_textSizeSpinBox);

    return group;
}

QWidget* LegendSettingsPage::createResetSection()
{
    QWidget *container = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    m_resetButton = new QPushButton(tr("Reset to Defaults"), this);
    m_resetButton->setToolTip(tr("Restore all legend settings to their default values"));

    layout->addStretch();
    layout->addWidget(m_resetButton);

    return container;
}

void LegendSettingsPage::loadSettings()
{
    PreferencesManager &prefs = PreferencesManager::instance();

    // Block signals while loading to prevent triggering saveSettings
    m_textSizeSpinBox->blockSignals(true);
    m_textSizeSpinBox->setValue(prefs.getValue(KEY_TEXT_SIZE).toInt());
    m_textSizeSpinBox->blockSignals(false);
}

void LegendSettingsPage::saveSettings()
{
    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.setValue(KEY_TEXT_SIZE, m_textSizeSpinBox->value());
}

void LegendSettingsPage::resetToDefaults()
{
    // Confirm before resetting
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        tr("Reset to Defaults"),
        tr("Are you sure you want to reset legend settings to their default values?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (reply != QMessageBox::Yes) {
        return;
    }

    // Reset the spin box to default value (this will trigger saveSettings)
    m_textSizeSpinBox->setValue(DEFAULT_TEXT_SIZE);
}

} // namespace FlySight
