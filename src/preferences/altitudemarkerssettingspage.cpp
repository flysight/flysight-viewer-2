#include "altitudemarkerssettingspage.h"
#include "preferencesmanager.h"
#include "preferencekeys.h"
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QListWidget>
#include <QComboBox>
#include <QPushButton>
#include <QColorDialog>
#include <QMessageBox>
#include <QSettings>
#include <algorithm>

namespace FlySight {

AltitudeMarkersSettingsPage::AltitudeMarkersSettingsPage(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(createUnitsGroup());
    layout->addWidget(createColorGroup());
    layout->addWidget(createAltitudesGroup());
    layout->addStretch();

    loadSettings();
}

QGroupBox* AltitudeMarkersSettingsPage::createUnitsGroup()
{
    QGroupBox *group = new QGroupBox(tr("Units"), this);
    QVBoxLayout *groupLayout = new QVBoxLayout(group);

    m_unitsComboBox = new QComboBox(this);
    m_unitsComboBox->addItem(tr("Imperial (feet)"));
    m_unitsComboBox->addItem(tr("Metric (metres)"));

    groupLayout->addWidget(m_unitsComboBox);

    return group;
}

QGroupBox* AltitudeMarkersSettingsPage::createColorGroup()
{
    QGroupBox *group = new QGroupBox(tr("Marker Colour"), this);
    QHBoxLayout *groupLayout = new QHBoxLayout(group);

    m_colorButton = new QPushButton(this);
    m_colorButton->setFixedSize(60, 24);
    connect(m_colorButton, &QPushButton::clicked, this, &AltitudeMarkersSettingsPage::onColorButtonClicked);

    groupLayout->addWidget(m_colorButton);
    groupLayout->addStretch();

    return group;
}

QGroupBox* AltitudeMarkersSettingsPage::createAltitudesGroup()
{
    QGroupBox *group = new QGroupBox(tr("Altitudes"), this);
    QVBoxLayout *groupLayout = new QVBoxLayout(group);

    m_altitudeList = new QListWidget(this);
    m_altitudeList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

    QWidget *buttonRow = new QWidget(this);
    QHBoxLayout *buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);

    QPushButton *addButton    = new QPushButton(tr("Add"), this);
    QPushButton *removeButton = new QPushButton(tr("Remove"), this);

    connect(addButton,    &QPushButton::clicked, this, &AltitudeMarkersSettingsPage::onAddAltitude);
    connect(removeButton, &QPushButton::clicked, this, &AltitudeMarkersSettingsPage::onRemoveAltitude);

    buttonLayout->addWidget(addButton);
    buttonLayout->addWidget(removeButton);
    buttonLayout->addStretch();

    groupLayout->addWidget(m_altitudeList);
    groupLayout->addWidget(buttonRow);

    return group;
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

void AltitudeMarkersSettingsPage::updateColorButtonStyle(const QColor &color)
{
    m_currentColor = color;

    int brightness = (color.red() * 299 + color.green() * 587 + color.blue() * 114) / 1000;
    QString textColor = (brightness > 128) ? QStringLiteral("black") : QStringLiteral("white");

    QString styleSheet = QString(
        "QPushButton {"
        "  background-color: %1;"
        "  border: 1px solid #888;"
        "  border-radius: 3px;"
        "  color: %2;"
        "}"
        "QPushButton:hover {"
        "  border: 1px solid #555;"
        "}"
        "QPushButton:pressed {"
        "  border: 2px solid #333;"
        "}"
    ).arg(color.name(), textColor);

    m_colorButton->setStyleSheet(styleSheet);
    m_colorButton->setText(color.name().toUpper());
}

void AltitudeMarkersSettingsPage::loadSettings()
{
    PreferencesManager &prefs = PreferencesManager::instance();

    // Units combo box
    QString units = prefs.getValue(PreferenceKeys::AltitudeMarkersUnits).toString();
    if (units == QStringLiteral("Metric")) {
        m_unitsComboBox->setCurrentIndex(1);
    } else {
        m_unitsComboBox->setCurrentIndex(0); // "Imperial" is the default
    }

    // Colour button
    QColor color = prefs.getValue(PreferenceKeys::AltitudeMarkersColor).value<QColor>();
    if (!color.isValid()) {
        color = QColor(0x87, 0xCE, 0xEB); // sky blue fallback
    }
    updateColorButtonStyle(color);

    // Altitude list — read via QSettings array
    m_altitudeList->clear();
    QSettings settings;
    int count = settings.beginReadArray(QStringLiteral("altitudeMarkers"));
    QList<int> altitudes;
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        altitudes.append(settings.value(QStringLiteral("value")).toInt());
    }
    settings.endArray();

    std::sort(altitudes.begin(), altitudes.end());
    for (int val : altitudes) {
        QListWidgetItem *item = new QListWidgetItem(QString::number(val));
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        m_altitudeList->addItem(item);
    }
}

void AltitudeMarkersSettingsPage::saveSettings()
{
    PreferencesManager &prefs = PreferencesManager::instance();

    // Collect and validate altitude values from the list
    QString units = (m_unitsComboBox->currentIndex() == 1)
        ? QStringLiteral("Metric")
        : QStringLiteral("Imperial");

    QList<int> altitudes;
    QSet<int> seen;

    for (int i = 0; i < m_altitudeList->count(); ++i) {
        QString text = m_altitudeList->item(i)->text().trimmed();
        bool ok = false;
        int val = text.toInt(&ok);

        if (!ok || val <= 0) {
            QMessageBox::warning(this, tr("Invalid Altitude"),
                tr("Altitude \"%1\" is not a positive integer and will be ignored.").arg(text));
            continue;
        }
        if (seen.contains(val)) {
            QMessageBox::warning(this, tr("Duplicate Altitude"),
                tr("Altitude %1 appears more than once. The duplicate will be ignored.").arg(val));
            continue;
        }
        seen.insert(val);
        altitudes.append(val);
    }

    std::sort(altitudes.begin(), altitudes.end());

    // Write altitude array FIRST so that any refresh() triggered below reads current data
    QSettings settings;
    settings.beginWriteArray(QStringLiteral("altitudeMarkers"), altitudes.size());
    for (int i = 0; i < altitudes.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("value"), altitudes[i]);
    }
    settings.endArray();

    // Now write scalar prefs — each may trigger preferenceChanged -> refresh(),
    // but the array is already persisted so refresh() reads the correct data.
    prefs.setValue(PreferenceKeys::AltitudeMarkersUnits, units);
    prefs.setValue(PreferenceKeys::AltitudeMarkersColor, m_currentColor);
    prefs.setValue(PreferenceKeys::AltitudeMarkersSize, altitudes.size());

    // Always increment version to guarantee preferenceChanged fires.
    // beginWriteArray above already writes the "size" key to QSettings,
    // so PreferencesManager may not detect a change for AltitudeMarkersSize.
    int version = prefs.getValue(PreferenceKeys::AltitudeMarkersVersion).toInt();
    prefs.setValue(PreferenceKeys::AltitudeMarkersVersion, version + 1);
}

// ---------------------------------------------------------------------------
// Interactive slots
// ---------------------------------------------------------------------------

void AltitudeMarkersSettingsPage::onColorButtonClicked()
{
    QColor newColor = QColorDialog::getColor(m_currentColor, this, tr("Select Marker Colour"));
    if (newColor.isValid() && newColor != m_currentColor) {
        updateColorButtonStyle(newColor);
    }
}

void AltitudeMarkersSettingsPage::onAddAltitude()
{
    int defaultValue;
    if (m_altitudeList->count() == 0) {
        defaultValue = (m_unitsComboBox->currentIndex() == 0) ? 300 : 100;
    } else {
        int maxVal = 0;
        for (int i = 0; i < m_altitudeList->count(); ++i) {
            int val = m_altitudeList->item(i)->text().toInt();
            if (val > maxVal) maxVal = val;
        }
        int step = (m_unitsComboBox->currentIndex() == 0) ? 300 : 100;
        defaultValue = maxVal + step;
    }

    QListWidgetItem *item = new QListWidgetItem(QString::number(defaultValue));
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    m_altitudeList->addItem(item);
    m_altitudeList->setCurrentItem(item);
    m_altitudeList->editItem(item);
}

void AltitudeMarkersSettingsPage::onRemoveAltitude()
{
    QListWidgetItem *item = m_altitudeList->currentItem();
    if (item) {
        delete item;
    }
}

} // namespace FlySight
