#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include "importsettingspage.h"
#include "preferencekeys.h"
#include "preferencepagestyle.h"
#include "preferencesmanager.h"
#include "../fusion/orientation.h"

namespace FlySight {

ImportSettingsPage::ImportSettingsPage(QWidget *parent)
    : QWidget(parent) {
    QVBoxLayout *layout = new QVBoxLayout(this);

    layout->addWidget(createGroundReferenceGroup());
    layout->addWidget(createOrientationGroup());
    layout->addWidget(createDescentPauseGroup());
    layout->addWidget(createTrackVisibilityGroup());
    layout->addStretch();

    connect(automaticRadioButton, &QRadioButton::toggled, this, &ImportSettingsPage::saveSettings);
    connect(fixedRadioButton, &QRadioButton::toggled, this, &ImportSettingsPage::saveSettings);
    connect(fixedElevationLineEdit, &QLineEdit::textChanged, this, &ImportSettingsPage::saveSettings);
    connect(descentPauseSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ImportSettingsPage::saveSettings);
    connect(hideOthersCheckBox, &QCheckBox::toggled, this, &ImportSettingsPage::saveSettings);
    connect(orientationComboBox, &QComboBox::currentIndexChanged, this, &ImportSettingsPage::saveSettings);
}

// The orientation stored into each newly imported recording: which of the
// device's axes point forward and up. The 24 orientations of the one
// orientation type, labelled as the logbook's Orientation column labels
// them; the current preference is selected, or the default when the stored
// token is none of them.
QGroupBox* ImportSettingsPage::createOrientationGroup() {
    QGroupBox *group = new QGroupBox(tr("Orientation"), this);
    QFormLayout *form = new QFormLayout(group);

    orientationComboBox = new QComboBox(this);
    for (const Fusion::Orientation &orientation : Fusion::Orientation::all())
        orientationComboBox->addItem(orientation.label(), orientation.token());
    form->addRow(tr("Device orientation:"), orientationComboBox);

    const QString stored = PreferencesManager::instance().getValue(PreferenceKeys::ImportOrientation).toString();
    int index = orientationComboBox->findData(stored);
    if (index < 0)
        index = orientationComboBox->findData(Fusion::Orientation::defaultOrientation().token());
    orientationComboBox->setCurrentIndex(index);

    return group;
}

// Two modes; the elevation belongs to the fixed one, so its row sits under
// that button and is enabled only while that button is chosen.
QGroupBox* ImportSettingsPage::createGroundReferenceGroup() {
    QGroupBox *group = new QGroupBox(tr("Ground reference"), this);
    QFormLayout *form = new QFormLayout(group);

    automaticRadioButton = new QRadioButton(tr("Automatic"), this);
    fixedRadioButton = new QRadioButton(tr("Fixed"), this);

    // A line edit has no suffix, so its unit follows it as a label
    QWidget *elevation = new QWidget(this);
    QHBoxLayout *elevationLayout = new QHBoxLayout(elevation);
    elevationLayout->setContentsMargins(0, 0, 0, 0);
    fixedElevationLineEdit = new QLineEdit(this);
    elevationLayout->addWidget(fixedElevationLineEdit);
    elevationLayout->addWidget(new QLabel(tr("m"), this));

    form->addRow(automaticRadioButton);
    form->addRow(fixedRadioButton);
    form->addRow(PreferencePage::subordinateLabel(tr("Elevation:"), this), elevation);

    // Initialize settings
    PreferencesManager &prefs = PreferencesManager::instance();
    QString groundRefMode = prefs.getValue(PreferenceKeys::ImportGroundReferenceMode).toString();
    double elevationValue = prefs.getValue(PreferenceKeys::ImportFixedElevation).toDouble();

    if (groundRefMode == "Automatic") {
        automaticRadioButton->setChecked(true);
    } else {
        fixedRadioButton->setChecked(true);
    }
    fixedElevationLineEdit->setText(QString::number(elevationValue));

    elevation->setEnabled(fixedRadioButton->isChecked());
    connect(fixedRadioButton, &QRadioButton::toggled, elevation, &QWidget::setEnabled);

    return group;
}

QGroupBox* ImportSettingsPage::createDescentPauseGroup() {
    QGroupBox *group = new QGroupBox(tr("Descent detection"), this);
    QFormLayout *form = new QFormLayout(group);

    descentPauseSpinBox = new QDoubleSpinBox(this);
    descentPauseSpinBox->setRange(1.0, 300.0);
    descentPauseSpinBox->setSingleStep(1.0);
    descentPauseSpinBox->setDecimals(1);
    descentPauseSpinBox->setSuffix(tr(" s"));
    form->addRow(tr("Pause timeout:"), descentPauseSpinBox);

    // Initialize from preferences
    PreferencesManager &prefs = PreferencesManager::instance();
    descentPauseSpinBox->setValue(prefs.getValue(PreferenceKeys::ImportDescentPauseSeconds).toDouble());

    return group;
}

QGroupBox* ImportSettingsPage::createTrackVisibilityGroup() {
    QGroupBox *group = new QGroupBox(tr("Track visibility"), this);
    QFormLayout *form = new QFormLayout(group);

    hideOthersCheckBox = new QCheckBox(tr("Hide other tracks on import"), this);
    form->addRow(hideOthersCheckBox);

    // Initialize from preferences
    PreferencesManager &prefs = PreferencesManager::instance();
    hideOthersCheckBox->setChecked(prefs.getValue(PreferenceKeys::ImportHideOthersOnImport).toBool());

    return group;
}

void ImportSettingsPage::saveSettings() {
    PreferencesManager &prefs = PreferencesManager::instance();

    if (automaticRadioButton->isChecked()) {
        prefs.setValue(PreferenceKeys::ImportGroundReferenceMode, "Automatic");
    } else {
        prefs.setValue(PreferenceKeys::ImportGroundReferenceMode, "Fixed");
    }
    prefs.setValue(PreferenceKeys::ImportFixedElevation, fixedElevationLineEdit->text().toDouble());
    prefs.setValue(PreferenceKeys::ImportDescentPauseSeconds, descentPauseSpinBox->value());
    prefs.setValue(PreferenceKeys::ImportHideOthersOnImport, hideOthersCheckBox->isChecked());
    prefs.setValue(PreferenceKeys::ImportOrientation, orientationComboBox->currentData().toString());
}

} // namespace FlySight
