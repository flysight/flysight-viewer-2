#include <QCheckBox>
#include <QLabel>
#include <QSpacerItem>
#include "importsettingspage.h"
#include "preferencekeys.h"
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
    QHBoxLayout *groupLayout = new QHBoxLayout(group);

    QLabel *label = new QLabel(tr("Device orientation"), this);
    orientationComboBox = new QComboBox(this);
    for (const Fusion::Orientation &orientation : Fusion::Orientation::all())
        orientationComboBox->addItem(orientation.label(), orientation.token());

    groupLayout->addWidget(label);
    groupLayout->addWidget(orientationComboBox);

    const QString stored = PreferencesManager::instance().getValue(PreferenceKeys::ImportOrientation).toString();
    int index = orientationComboBox->findData(stored);
    if (index < 0)
        index = orientationComboBox->findData(Fusion::Orientation::defaultOrientation().token());
    orientationComboBox->setCurrentIndex(index);

    return group;
}

QGroupBox* ImportSettingsPage::createGroundReferenceGroup() {
    QGroupBox *groundReferenceGroup = new QGroupBox(tr("Ground reference"), this);
    QVBoxLayout *groupLayout = new QVBoxLayout(groundReferenceGroup);

    automaticRadioButton = new QRadioButton(tr("Automatic"), this);
    fixedRadioButton = new QRadioButton(tr("Fixed"), this);

    QHBoxLayout *fixedLayout = new QHBoxLayout();
    fixedElevationLineEdit = new QLineEdit(this);
    QLabel *metersLabel = new QLabel(tr("m"), this);

    fixedLayout->addWidget(fixedRadioButton);
    fixedLayout->addWidget(fixedElevationLineEdit);
    fixedLayout->addWidget(metersLabel);

    groupLayout->addWidget(automaticRadioButton);
    groupLayout->addLayout(fixedLayout);

    // Initialize settings
    PreferencesManager &prefs = PreferencesManager::instance();
    QString groundRefMode = prefs.getValue(PreferenceKeys::ImportGroundReferenceMode).toString();
    double elevation = prefs.getValue(PreferenceKeys::ImportFixedElevation).toDouble();

    if (groundRefMode == "Automatic") {
        automaticRadioButton->setChecked(true);
    } else {
        fixedRadioButton->setChecked(true);
    }
    fixedElevationLineEdit->setText(QString::number(elevation));

    return groundReferenceGroup;
}

QGroupBox* ImportSettingsPage::createDescentPauseGroup() {
    QGroupBox *descentPauseGroup = new QGroupBox(tr("Descent detection"), this);
    QHBoxLayout *groupLayout = new QHBoxLayout(descentPauseGroup);

    QLabel *label = new QLabel(tr("Descent pause timeout"), this);
    descentPauseSpinBox = new QDoubleSpinBox(this);
    descentPauseSpinBox->setRange(1.0, 300.0);
    descentPauseSpinBox->setSingleStep(1.0);
    descentPauseSpinBox->setDecimals(1);
    QLabel *unitLabel = new QLabel(tr("s"), this);

    groupLayout->addWidget(label);
    groupLayout->addWidget(descentPauseSpinBox);
    groupLayout->addWidget(unitLabel);

    // Initialize from preferences
    PreferencesManager &prefs = PreferencesManager::instance();
    descentPauseSpinBox->setValue(prefs.getValue(PreferenceKeys::ImportDescentPauseSeconds).toDouble());

    return descentPauseGroup;
}

QGroupBox* ImportSettingsPage::createTrackVisibilityGroup() {
    QGroupBox *group = new QGroupBox(tr("Track visibility"), this);
    QVBoxLayout *groupLayout = new QVBoxLayout(group);

    hideOthersCheckBox = new QCheckBox(tr("Hide other tracks on import"), this);
    groupLayout->addWidget(hideOthersCheckBox);

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
