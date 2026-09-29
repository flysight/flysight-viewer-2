#include <QLabel>
#include <QFormLayout>
#include <QColorDialog>
#include <QHeaderView>
#include <QMessageBox>
#include "plotssettingspage.h"
#include "preferencesmanager.h"
#include "../plotregistry.h"
#include "../plotutils.h"

namespace FlySight {

PlotsSettingsPage::PlotsSettingsPage(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    layout->addWidget(createGlobalSettingsGroup());
    layout->addWidget(createPerPlotSettingsGroup(), 1); // Give tree widget stretch
    layout->addWidget(createResetSection());

    // Load current settings
    loadGlobalSettings();
    loadPerPlotSettings();

    // Connect global settings signals
    connect(lineThicknessSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &PlotsSettingsPage::saveGlobalSettings);
    connect(textSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &PlotsSettingsPage::saveGlobalSettings);
    connect(crosshairColorButton, &QPushButton::clicked,
            this, &PlotsSettingsPage::chooseCrosshairColor);
    connect(crosshairThicknessSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &PlotsSettingsPage::saveGlobalSettings);
    connect(yAxisPaddingSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &PlotsSettingsPage::saveGlobalSettings);
}

QGroupBox* PlotsSettingsPage::createGlobalSettingsGroup()
{
    QGroupBox *group = new QGroupBox(tr("Plot appearance"), this);
    QFormLayout *formLayout = new QFormLayout(group);

    // Line thickness
    lineThicknessSpinBox = new QDoubleSpinBox(this);
    lineThicknessSpinBox->setRange(0.5, 10.0);
    lineThicknessSpinBox->setSingleStep(0.5);
    lineThicknessSpinBox->setSuffix(tr(" px"));
    lineThicknessSpinBox->setDecimals(1);
    lineThicknessSpinBox->setToolTip(tr("Thickness of plot lines (0.5-10 pixels)"));
    formLayout->addRow(tr("Line thickness:"), lineThicknessSpinBox);

    // Plot text size
    textSizeSpinBox = new QSpinBox(this);
    textSizeSpinBox->setRange(6, 24);
    textSizeSpinBox->setSuffix(tr(" pt"));
    textSizeSpinBox->setToolTip(tr("Font size for axis labels and values (6-24 points)"));
    formLayout->addRow(tr("Plot text size:"), textSizeSpinBox);

    // Crosshair color
    crosshairColorButton = new QPushButton(this);
    crosshairColorButton->setFixedSize(80, 24);
    crosshairColorButton->setCursor(Qt::PointingHandCursor);
    crosshairColorButton->setToolTip(tr("Click to choose crosshair color"));
    formLayout->addRow(tr("Crosshair color:"), crosshairColorButton);

    // Crosshair thickness
    crosshairThicknessSpinBox = new QDoubleSpinBox(this);
    crosshairThicknessSpinBox->setRange(0.5, 5.0);
    crosshairThicknessSpinBox->setSingleStep(0.5);
    crosshairThicknessSpinBox->setSuffix(tr(" px"));
    crosshairThicknessSpinBox->setDecimals(1);
    crosshairThicknessSpinBox->setToolTip(tr("Thickness of crosshair lines (0.5-5 pixels)"));
    formLayout->addRow(tr("Crosshair thickness:"), crosshairThicknessSpinBox);

    // Y-axis padding (displayed as percentage, stored as decimal)
    yAxisPaddingSpinBox = new QSpinBox(this);
    yAxisPaddingSpinBox->setRange(1, 50);
    yAxisPaddingSpinBox->setSuffix(tr("%"));
    yAxisPaddingSpinBox->setToolTip(tr("Extra space above and below plot data (1-50%)"));
    formLayout->addRow(tr("Y-axis padding:"), yAxisPaddingSpinBox);

    return group;
}

QGroupBox* PlotsSettingsPage::createPerPlotSettingsGroup()
{
    QGroupBox *group = new QGroupBox(tr("Per-plot settings"), this);
    QVBoxLayout *layout = new QVBoxLayout(group);

    plotsTreeWidget = new QTreeWidget(this);
    plotsTreeWidget->setHeaderLabels({
        tr("Plot"),
        tr("Color"),
        tr("Y-Axis Mode"),
        tr("Y-Axis Min"),
        tr("Y-Axis Max")
    });
    plotsTreeWidget->setRootIsDecorated(true);
    plotsTreeWidget->setAlternatingRowColors(true);

    // Set column widths
    plotsTreeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    plotsTreeWidget->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    plotsTreeWidget->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    plotsTreeWidget->header()->setSectionResizeMode(3, QHeaderView::Fixed);
    plotsTreeWidget->header()->setSectionResizeMode(4, QHeaderView::Fixed);
    plotsTreeWidget->setColumnWidth(1, 80);
    plotsTreeWidget->setColumnWidth(2, 100);
    plotsTreeWidget->setColumnWidth(3, 100);
    plotsTreeWidget->setColumnWidth(4, 100);

    // Build tree from PlotRegistry
    const QVector<PlotValue> plots = PlotRegistry::instance().allPlots();

    // Group plots by category
    QMap<QString, QTreeWidgetItem*> categoryItems;

    for (const PlotValue &pv : plots) {
        // Get or create category item
        QTreeWidgetItem *categoryItem = categoryItems.value(pv.category, nullptr);
        if (!categoryItem) {
            categoryItem = new QTreeWidgetItem(plotsTreeWidget);
            categoryItem->setText(0, pv.category);
            categoryItem->setFlags(categoryItem->flags() & ~Qt::ItemIsSelectable);
            categoryItem->setExpanded(true);
            QFont boldFont = categoryItem->font(0);
            boldFont.setBold(true);
            categoryItem->setFont(0, boldFont);
            categoryItems[pv.category] = categoryItem;
        }

        // Create plot item
        QTreeWidgetItem *plotItem = new QTreeWidgetItem(categoryItem);
        QString plotKey = getPlotKey(pv.sensorID, pv.measurementID);

        // Column 0: Plot name (read-only, just text)
        QString displayName = pv.plotName;
        if (!pv.plotUnits.isEmpty()) {
            displayName += QString(" (%1)").arg(pv.plotUnits);
        }
        plotItem->setText(0, displayName);
        plotItem->setData(0, Qt::UserRole, plotKey); // Store key for later lookup

        // Column 1: Color button
        QPushButton *colorButton = new QPushButton(this);
        colorButton->setFixedSize(60, 20);
        colorButton->setCursor(Qt::PointingHandCursor);
        colorButton->setProperty("plotKey", plotKey);
        plotsTreeWidget->setItemWidget(plotItem, 1, colorButton);
        plotColorButtons[plotKey] = colorButton;
        connect(colorButton, &QPushButton::clicked,
                this, &PlotsSettingsPage::onPlotColorButtonClicked);

        // Column 2: Y-axis mode combo
        QComboBox *modeCombo = new QComboBox(this);
        modeCombo->addItem(tr("Auto"));
        modeCombo->addItem(tr("Manual"));
        modeCombo->setProperty("plotKey", plotKey);
        plotsTreeWidget->setItemWidget(plotItem, 2, modeCombo);
        plotYAxisModeComboBoxes[plotKey] = modeCombo;
        connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, plotKey](int index) {
                    onYAxisModeChanged(plotKey, index);
                });

        // Column 3: Y-axis min
        QDoubleSpinBox *minSpinBox = new QDoubleSpinBox(this);
        minSpinBox->setRange(-1e9, 1e9);
        minSpinBox->setDecimals(2);
        minSpinBox->setProperty("plotKey", plotKey);
        plotsTreeWidget->setItemWidget(plotItem, 3, minSpinBox);
        plotYAxisMinSpinBoxes[plotKey] = minSpinBox;
        connect(minSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, plotKey](double value) {
                    onYAxisMinChanged(plotKey, value);
                });

        // Column 4: Y-axis max
        QDoubleSpinBox *maxSpinBox = new QDoubleSpinBox(this);
        maxSpinBox->setRange(-1e9, 1e9);
        maxSpinBox->setDecimals(2);
        maxSpinBox->setProperty("plotKey", plotKey);
        plotsTreeWidget->setItemWidget(plotItem, 4, maxSpinBox);
        plotYAxisMaxSpinBoxes[plotKey] = maxSpinBox;
        connect(maxSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, plotKey](double value) {
                    onYAxisMaxChanged(plotKey, value);
                });
    }

    layout->addWidget(plotsTreeWidget);
    return group;
}

QWidget* PlotsSettingsPage::createResetSection()
{
    QWidget *container = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 10, 0, 0);

    layout->addStretch();

    QPushButton *resetButton = new QPushButton(tr("Reset All to Defaults"), this);
    connect(resetButton, &QPushButton::clicked, this, &PlotsSettingsPage::resetAllToDefaults);
    layout->addWidget(resetButton);

    return container;
}

void PlotsSettingsPage::updateColorButtonStyle(QPushButton *button, const QColor &color)
{
    QString styleSheet = QString(
        "QPushButton {"
        "  background-color: %1;"
        "  border: 1px solid #888888;"
        "  border-radius: 3px;"
        "}"
        "QPushButton:hover {"
        "  border: 1px solid #555555;"
        "}"
    ).arg(color.name());
    button->setStyleSheet(styleSheet);
    button->setToolTip(color.name());
}

void PlotsSettingsPage::loadGlobalSettings()
{
    PreferencesManager &prefs = PreferencesManager::instance();

    // Block signals during load to prevent triggering saveGlobalSettings
    lineThicknessSpinBox->blockSignals(true);
    textSizeSpinBox->blockSignals(true);
    crosshairThicknessSpinBox->blockSignals(true);
    yAxisPaddingSpinBox->blockSignals(true);

    lineThicknessSpinBox->setValue(prefs.getValue("plots/lineThickness").toDouble());
    textSizeSpinBox->setValue(prefs.getValue("plots/textSize").toInt());

    currentCrosshairColor = QColor(prefs.getValue("plots/crosshairColor").toString());
    updateColorButtonStyle(crosshairColorButton, currentCrosshairColor);

    crosshairThicknessSpinBox->setValue(prefs.getValue("plots/crosshairThickness").toDouble());

    // Y-axis padding: stored as decimal (0.01-0.50), displayed as percentage (1-50)
    double paddingDecimal = prefs.getValue("plots/yAxisPadding").toDouble();
    yAxisPaddingSpinBox->setValue(static_cast<int>(paddingDecimal * 100));

    // Re-enable signals
    lineThicknessSpinBox->blockSignals(false);
    textSizeSpinBox->blockSignals(false);
    crosshairThicknessSpinBox->blockSignals(false);
    yAxisPaddingSpinBox->blockSignals(false);
}

void PlotsSettingsPage::loadPerPlotSettings()
{
    PreferencesManager &prefs = PreferencesManager::instance();
    const QVector<PlotValue> plots = PlotRegistry::instance().allPlots();

    for (const PlotValue &pv : plots) {
        QString plotKey = getPlotKey(pv.sensorID, pv.measurementID);
        QString prefKeyBase = QString("plots/%1/%2").arg(pv.sensorID, pv.measurementID);

        // Color: the one the plot is drawn in (plotutils.h)
        if (plotColorButtons.contains(plotKey))
            updateColorButtonStyle(plotColorButtons[plotKey], plotColor(pv));

        // Y-axis mode - block signals during loading
        if (plotYAxisModeComboBoxes.contains(plotKey)) {
            QComboBox *combo = plotYAxisModeComboBoxes[plotKey];
            combo->blockSignals(true);
            QString mode = prefs.getValue(prefKeyBase + "/yAxisMode").toString();
            int index = (mode == "Manual") ? 1 : 0;
            combo->setCurrentIndex(index);
            combo->blockSignals(false);
            updateYAxisWidgetsEnabled(plotKey, index == 1);
        }

        // Y-axis min/max - block signals during loading
        if (plotYAxisMinSpinBoxes.contains(plotKey)) {
            QDoubleSpinBox *spinBox = plotYAxisMinSpinBoxes[plotKey];
            spinBox->blockSignals(true);
            double minVal = prefs.getValue(prefKeyBase + "/yAxisMin").toDouble();
            spinBox->setValue(minVal);
            spinBox->blockSignals(false);
        }
        if (plotYAxisMaxSpinBoxes.contains(plotKey)) {
            QDoubleSpinBox *spinBox = plotYAxisMaxSpinBoxes[plotKey];
            spinBox->blockSignals(true);
            double maxVal = prefs.getValue(prefKeyBase + "/yAxisMax").toDouble();
            spinBox->setValue(maxVal);
            spinBox->blockSignals(false);
        }
    }
}

QString PlotsSettingsPage::getPlotKey(const QString &sensorID, const QString &measurementID) const
{
    return QString("%1/%2").arg(sensorID, measurementID);
}

void PlotsSettingsPage::saveGlobalSettings()
{
    PreferencesManager &prefs = PreferencesManager::instance();

    prefs.setValue("plots/lineThickness", lineThicknessSpinBox->value());
    prefs.setValue("plots/textSize", textSizeSpinBox->value());
    prefs.setValue("plots/crosshairColor", currentCrosshairColor.name());
    prefs.setValue("plots/crosshairThickness", crosshairThicknessSpinBox->value());

    // Convert percentage to decimal for storage
    double paddingDecimal = yAxisPaddingSpinBox->value() / 100.0;
    prefs.setValue("plots/yAxisPadding", paddingDecimal);
}

void PlotsSettingsPage::chooseCrosshairColor()
{
    QColor color = QColorDialog::getColor(currentCrosshairColor, this, tr("Choose Crosshair Color"));
    if (color.isValid()) {
        currentCrosshairColor = color;
        updateColorButtonStyle(crosshairColorButton, currentCrosshairColor);
        saveGlobalSettings();
    }
}

void PlotsSettingsPage::onPlotColorButtonClicked()
{
    QPushButton *button = qobject_cast<QPushButton*>(sender());
    if (!button) return;

    QString plotKey = button->property("plotKey").toString();
    QStringList parts = plotKey.split('/');
    if (parts.size() != 2) return;

    QString sensorID = parts[0];
    QString measurementID = parts[1];
    QString prefKey = QString("plots/%1/%2/color").arg(sensorID, measurementID);

    PreferencesManager &prefs = PreferencesManager::instance();
    QColor currentColor(prefs.getValue(prefKey).toString());

    // Find default color from registry for dialog title
    QString plotName = plotKey;
    const QVector<PlotValue> plots = PlotRegistry::instance().allPlots();
    for (const PlotValue &pv : plots) {
        if (pv.sensorID == sensorID && pv.measurementID == measurementID) {
            plotName = pv.plotName;
            if (!currentColor.isValid()) {
                currentColor = pv.defaultColor;
            }
            break;
        }
    }

    QColor newColor = QColorDialog::getColor(currentColor, this,
                                              tr("Choose Color for %1").arg(plotName));
    if (newColor.isValid()) {
        prefs.setValue(prefKey, newColor.name());
        updateColorButtonStyle(button, newColor);
    }
}

void PlotsSettingsPage::onYAxisModeChanged(const QString &plotKey, int index)
{
    QStringList parts = plotKey.split('/');
    if (parts.size() != 2) return;

    QString sensorID = parts[0];
    QString measurementID = parts[1];
    QString prefKey = QString("plots/%1/%2/yAxisMode").arg(sensorID, measurementID);

    PreferencesManager &prefs = PreferencesManager::instance();
    prefs.setValue(prefKey, (index == 1) ? "Manual" : "Auto");

    updateYAxisWidgetsEnabled(plotKey, index == 1);
}

void PlotsSettingsPage::onYAxisMinChanged(const QString &plotKey, double value)
{
    QStringList parts = plotKey.split('/');
    if (parts.size() != 2) return;

    QString sensorID = parts[0];
    QString measurementID = parts[1];
    QString prefKey = QString("plots/%1/%2/yAxisMin").arg(sensorID, measurementID);

    PreferencesManager::instance().setValue(prefKey, value);
}

void PlotsSettingsPage::onYAxisMaxChanged(const QString &plotKey, double value)
{
    QStringList parts = plotKey.split('/');
    if (parts.size() != 2) return;

    QString sensorID = parts[0];
    QString measurementID = parts[1];
    QString prefKey = QString("plots/%1/%2/yAxisMax").arg(sensorID, measurementID);

    PreferencesManager::instance().setValue(prefKey, value);
}

void PlotsSettingsPage::updateYAxisWidgetsEnabled(const QString &plotKey, bool manual)
{
    if (plotYAxisMinSpinBoxes.contains(plotKey)) {
        plotYAxisMinSpinBoxes[plotKey]->setEnabled(manual);
    }
    if (plotYAxisMaxSpinBoxes.contains(plotKey)) {
        plotYAxisMaxSpinBoxes[plotKey]->setEnabled(manual);
    }
}

void PlotsSettingsPage::resetAllToDefaults()
{
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        tr("Reset to Defaults"),
        tr("Are you sure you want to reset all plot settings to their default values?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (reply != QMessageBox::Yes) {
        return;
    }

    PreferencesManager &prefs = PreferencesManager::instance();

    // Reset global settings to defaults
    prefs.setValue("plots/lineThickness", prefs.getDefaultValue("plots/lineThickness"));
    prefs.setValue("plots/textSize", prefs.getDefaultValue("plots/textSize"));
    prefs.setValue("plots/crosshairColor", prefs.getDefaultValue("plots/crosshairColor"));
    prefs.setValue("plots/crosshairThickness", prefs.getDefaultValue("plots/crosshairThickness"));
    prefs.setValue("plots/yAxisPadding", prefs.getDefaultValue("plots/yAxisPadding"));

    // Reset per-plot settings to defaults
    const QVector<PlotValue> plots = PlotRegistry::instance().allPlots();
    for (const PlotValue &pv : plots) {
        QString prefKeyBase = QString("plots/%1/%2").arg(pv.sensorID, pv.measurementID);

        prefs.setValue(prefKeyBase + "/color", pv.defaultColor.name());
        prefs.setValue(prefKeyBase + "/yAxisMode", "Auto");
        prefs.setValue(prefKeyBase + "/yAxisMin", 0.0);
        prefs.setValue(prefKeyBase + "/yAxisMax", 100.0);
    }

    // Reload UI
    loadGlobalSettings();
    loadPerPlotSettings();
}

} // namespace FlySight
