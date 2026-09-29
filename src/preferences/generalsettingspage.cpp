#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include "generalsettingspage.h"
#include "preferencesmanager.h"
#include "units/unitconverter.h"

namespace FlySight {

GeneralSettingsPage::GeneralSettingsPage(QWidget *parent)
    : QWidget(parent) {
    QVBoxLayout *layout = new QVBoxLayout(this);

    layout->addWidget(createUnitsGroup());
    layout->addWidget(createLogbookFolderGroup());
    layout->addStretch();

    connect(unitsComboBox, &QComboBox::currentTextChanged, this, &GeneralSettingsPage::saveSettings);
    connect(browseButton, &QPushButton::clicked, this, &GeneralSettingsPage::browseLogbookFolder);
}

QGroupBox* GeneralSettingsPage::createUnitsGroup() {
    QGroupBox *unitsGroup = new QGroupBox(tr("Units"), this);
    QFormLayout *form = new QFormLayout(unitsGroup);

    unitsComboBox = new QComboBox(this);
    unitsComboBox->addItems(UnitConverter::instance().availableSystems());
    form->addRow(tr("Unit system:"), unitsComboBox);

    // Initialize from current unit system (reflects changes via keyboard shortcut)
    QString units = UnitConverter::instance().currentSystem();
    unitsComboBox->setCurrentText(units);

    return unitsGroup;
}

QGroupBox* GeneralSettingsPage::createLogbookFolderGroup() {
    QGroupBox *logbookGroup = new QGroupBox(tr("Logbook"), this);
    QFormLayout *form = new QFormLayout(logbookGroup);

    // The folder and the button that chooses it are one field
    QWidget *folderField = new QWidget(this);
    QHBoxLayout *folderLayout = new QHBoxLayout(folderField);
    folderLayout->setContentsMargins(0, 0, 0, 0);

    logbookFolderLineEdit = new QLineEdit(this);
    logbookFolderLineEdit->setReadOnly(true);

    browseButton = new QPushButton(this);
    browseButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));

    folderLayout->addWidget(logbookFolderLineEdit);
    folderLayout->addWidget(browseButton);
    form->addRow(tr("Folder:"), folderField);

    // Initialize from settings
    QString folder = PreferencesManager::instance().getValue("general/logbookFolder").toString();
    logbookFolderLineEdit->setText(folder);

    return logbookGroup;
}

void GeneralSettingsPage::saveSettings() {
    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.setValue("general/units", unitsComboBox->currentText());
    prefs.setValue("general/logbookFolder", logbookFolderLineEdit->text());

    // Notify UnitConverter of the change
    UnitConverter::instance().setSystem(unitsComboBox->currentText());
}

void GeneralSettingsPage::browseLogbookFolder() {
    QString folder = QFileDialog::getExistingDirectory(this, tr("Select Logbook Folder"));
    if (!folder.isEmpty()) {
        logbookFolderLineEdit->setText(folder);
        saveSettings();
    }
}

} // namespace FlySight
