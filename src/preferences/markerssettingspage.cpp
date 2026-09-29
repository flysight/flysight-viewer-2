#include <QLabel>
#include <QHeaderView>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QMessageBox>
#include "markerssettingspage.h"
#include "preferencesmanager.h"
#include "preferencekeys.h"
#include "../markerregistry.h"

namespace FlySight {

MarkersSettingsPage::MarkersSettingsPage(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    layout->addWidget(createMarkerColorsGroup(), 1); // the tree takes the stretch
    layout->addWidget(createResetSection());
}

QGroupBox* MarkersSettingsPage::createMarkerColorsGroup()
{
    QGroupBox *group = new QGroupBox(tr("Marker colors"), this);
    QVBoxLayout *groupLayout = new QVBoxLayout(group);

    m_markerTree = new QTreeWidget(this);
    m_markerTree->setColumnCount(2);
    m_markerTree->setHeaderLabels(QStringList() << tr("Marker") << tr("Color"));
    m_markerTree->header()->setStretchLastSection(false);
    m_markerTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_markerTree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_markerTree->header()->resizeSection(1, 80);
    m_markerTree->setRootIsDecorated(true);
    m_markerTree->setSelectionMode(QAbstractItemView::NoSelection);
    m_markerTree->setFocusPolicy(Qt::NoFocus);

    populateMarkerTree();

    // Expand all categories by default
    m_markerTree->expandAll();

    groupLayout->addWidget(m_markerTree);

    return group;
}

QWidget* MarkersSettingsPage::createResetSection()
{
    QWidget *resetWidget = new QWidget(this);
    QHBoxLayout *resetLayout = new QHBoxLayout(resetWidget);
    resetLayout->setContentsMargins(0, 0, 0, 0);

    m_resetButton = new QPushButton(tr("Reset All to Defaults"), this);
    connect(m_resetButton, &QPushButton::clicked, this, &MarkersSettingsPage::resetAllToDefaults);

    resetLayout->addStretch();
    resetLayout->addWidget(m_resetButton);

    return resetWidget;
}

void MarkersSettingsPage::populateMarkerTree()
{
    m_markerTree->clear();
    m_colorButtons.clear();
    m_defaultColors.clear();

    // Get all markers from registry
    QVector<MarkerDefinition> markers = MarkerRegistry::instance()->allMarkers();

    // Group markers by category, skipping any whose colour is managed by their own subsystem
    QMap<QString, QVector<MarkerDefinition>> categorizedMarkers;
    for (const MarkerDefinition &marker : markers) {
        if (marker.groupId.isEmpty()) {
            categorizedMarkers[marker.category].append(marker);
        }
    }

    // Create tree structure
    for (auto it = categorizedMarkers.constBegin(); it != categorizedMarkers.constEnd(); ++it) {
        const QString &category = it.key();
        const QVector<MarkerDefinition> &categoryMarkers = it.value();

        // Create category item (top-level, non-editable, non-selectable)
        QTreeWidgetItem *categoryItem = new QTreeWidgetItem(m_markerTree);
        categoryItem->setText(0, category);
        categoryItem->setFlags(Qt::ItemIsEnabled); // Not selectable, not editable

        // Make category text bold
        QFont boldFont = categoryItem->font(0);
        boldFont.setBold(true);
        categoryItem->setFont(0, boldFont);

        // Add markers under this category
        for (const MarkerDefinition &marker : categoryMarkers) {
            QTreeWidgetItem *markerItem = new QTreeWidgetItem(categoryItem);
            markerItem->setText(0, marker.displayName);
            markerItem->setFlags(Qt::ItemIsEnabled); // Not selectable, not editable

            // Store the default color for reset functionality
            m_defaultColors[marker.attributeKey] = marker.color;

            // Load current color from preferences (or use default)
            QColor currentColor = loadMarkerColor(marker.attributeKey, marker.color);

            // Create color button
            QPushButton *colorButton = new QPushButton(this);
            colorButton->setFixedSize(60, 24);
            colorButton->setProperty("attributeKey", marker.attributeKey);
            updateColorButtonStyle(colorButton, currentColor);

            connect(colorButton, &QPushButton::clicked, this, &MarkersSettingsPage::onColorButtonClicked);

            // Store reference to color button
            m_colorButtons[marker.attributeKey] = colorButton;

            // Set the button as the item widget for column 1
            m_markerTree->setItemWidget(markerItem, 1, colorButton);
        }
    }
}

void MarkersSettingsPage::onColorButtonClicked()
{
    QPushButton *button = qobject_cast<QPushButton*>(sender());
    if (!button) {
        return;
    }

    QString attributeKey = button->property("attributeKey").toString();
    if (attributeKey.isEmpty()) {
        return;
    }

    // Get current color from the button's stored property
    QColor currentColor = button->property("currentColor").value<QColor>();
    if (!currentColor.isValid()) {
        currentColor = m_defaultColors.value(attributeKey, Qt::white);
    }

    // Open color dialog
    QColor newColor = QColorDialog::getColor(currentColor, this, tr("Select Marker Color"));

    if (newColor.isValid() && newColor != currentColor) {
        // Update button appearance
        updateColorButtonStyle(button, newColor);

        // Save immediately
        saveMarkerColor(attributeKey, newColor);
    }
}

void MarkersSettingsPage::resetAllToDefaults()
{
    // Confirm before resetting
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        tr("Reset to Defaults"),
        tr("Are you sure you want to reset all marker colors to their default values?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (reply != QMessageBox::Yes) {
        return;
    }

    // Only markers with an empty groupId are tracked in m_defaultColors
    // (see populateMarkerTree()), so this loop implicitly scopes to non-grouped markers.
    for (auto it = m_defaultColors.constBegin(); it != m_defaultColors.constEnd(); ++it) {
        const QString &attributeKey = it.key();
        const QColor &defaultColor = it.value();

        // Update the button appearance
        QPushButton *button = m_colorButtons.value(attributeKey);
        if (button) {
            updateColorButtonStyle(button, defaultColor);
        }

        // Save the default color
        saveMarkerColor(attributeKey, defaultColor);
    }
}

void MarkersSettingsPage::updateColorButtonStyle(QPushButton *button, const QColor &color)
{
    // Store current color as a property for later retrieval
    button->setProperty("currentColor", color);

    // Calculate contrasting text color (white or black)
    int brightness = (color.red() * 299 + color.green() * 587 + color.blue() * 114) / 1000;
    QString textColor = (brightness > 128) ? "black" : "white";

    // Set button style to show the color
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

    button->setStyleSheet(styleSheet);
    button->setText(color.name().toUpper());
}

void MarkersSettingsPage::saveMarkerColor(const QString &attributeKey, const QColor &color)
{
    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.setValue(PreferenceKeys::markerColorKey(attributeKey), color);
}

QColor MarkersSettingsPage::loadMarkerColor(const QString &attributeKey, const QColor &defaultColor)
{
    PreferencesManager &prefs = PreferencesManager::instance();
    QString key = PreferenceKeys::markerColorKey(attributeKey);

    // Register preference if not already registered
    if (!prefs.getValue(key).isValid()) {
        prefs.registerPreference(key, defaultColor);
    }

    return prefs.getValue(key).value<QColor>();
}

} // namespace FlySight
