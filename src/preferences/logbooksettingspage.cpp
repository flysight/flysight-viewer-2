#include "logbooksettingspage.h"
#include "addcolumndialog.h"
#include "preferencekeys.h"
#include "preferencesmanager.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace FlySight {

LogbookSettingsPage::LogbookSettingsPage(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(createColumnsGroup());
    layout->addWidget(createCacheGroup());
    layout->addStretch();

    loadSettings();
}

QGroupBox* LogbookSettingsPage::createColumnsGroup()
{
    QGroupBox *group = new QGroupBox(tr("Columns"), this);
    QVBoxLayout *groupLayout = new QVBoxLayout(group);

    m_columnList = new QListWidget(this);
    m_columnList->setEditTriggers(QAbstractItemView::DoubleClicked
                                  | QAbstractItemView::EditKeyPressed);
    m_columnList->setDragDropMode(QAbstractItemView::NoDragDrop);

    connect(m_columnList, &QListWidget::currentRowChanged,
            this, &LogbookSettingsPage::updateButtonStates);
    connect(m_columnList, &QListWidget::itemChanged,
            this, &LogbookSettingsPage::onItemChanged);

    QWidget *buttonRow = new QWidget(this);
    QHBoxLayout *buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);

    m_addButton    = new QPushButton(tr("Add..."), this);
    m_removeButton = new QPushButton(tr("Remove"), this);
    m_moveUpButton = new QPushButton(tr("Move Up"), this);
    m_moveDownButton = new QPushButton(tr("Move Down"), this);

    connect(m_addButton,    &QPushButton::clicked, this, &LogbookSettingsPage::onAddColumn);
    connect(m_removeButton, &QPushButton::clicked, this, &LogbookSettingsPage::onRemoveColumn);
    connect(m_moveUpButton, &QPushButton::clicked, this, &LogbookSettingsPage::onMoveUp);
    connect(m_moveDownButton, &QPushButton::clicked, this, &LogbookSettingsPage::onMoveDown);

    buttonLayout->addWidget(m_addButton);
    buttonLayout->addWidget(m_removeButton);
    buttonLayout->addWidget(m_moveUpButton);
    buttonLayout->addWidget(m_moveDownButton);
    buttonLayout->addStretch();

    groupLayout->addWidget(m_columnList);
    groupLayout->addWidget(buttonRow);

    return group;
}

QGroupBox* LogbookSettingsPage::createCacheGroup()
{
    QGroupBox *group = new QGroupBox(tr("Cache"), this);
    QHBoxLayout *groupLayout = new QHBoxLayout(group);

    QLabel *label = new QLabel(tr("Maximum cached sessions:"), this);
    m_cacheSizeSpinBox = new QSpinBox(this);
    m_cacheSizeSpinBox->setRange(0, 1000);

    groupLayout->addWidget(label);
    groupLayout->addWidget(m_cacheSizeSpinBox);
    groupLayout->addStretch();

    return group;
}

void LogbookSettingsPage::loadSettings()
{
    populateList();

    int cacheSize = PreferencesManager::instance()
                        .getValue(PreferenceKeys::LogbookCacheSize).toInt();
    m_cacheSizeSpinBox->setValue(cacheSize);
}

void LogbookSettingsPage::populateList()
{
    m_columnList->blockSignals(true);
    m_columnList->clear();

    m_columns = LogbookColumnStore::instance().columns();

    for (const LogbookColumn &col : m_columns) {
        QListWidgetItem *item = new QListWidgetItem(logbookColumnLabel(col));
        item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled
                       | Qt::ItemIsSelectable | Qt::ItemIsEditable
                       | Qt::ItemIsDragEnabled);
        item->setCheckState(col.enabled ? Qt::Checked : Qt::Unchecked);
        m_columnList->addItem(item);
    }

    m_columnList->blockSignals(false);
    updateButtonStates();
}

void LogbookSettingsPage::updateButtonStates()
{
    int row = m_columnList->currentRow();
    int count = m_columnList->count();

    m_removeButton->setEnabled(row >= 0);
    m_moveUpButton->setEnabled(row > 0);
    m_moveDownButton->setEnabled(row >= 0 && row < count - 1);
}

void LogbookSettingsPage::onAddColumn()
{
    AddColumnDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        LogbookColumn col = dlg.result();

        // A column that is already in the list is not added a second time:
        // the existing entry is selected and switched on instead.
        const QString key = logbookColumnDefinitionKey(col);
        for (int row = 0; row < m_columns.size(); ++row) {
            if (logbookColumnDefinitionKey(m_columns[row]) != key)
                continue;
            m_columnList->setCurrentRow(row);
            m_columnList->item(row)->setCheckState(Qt::Checked);    // onItemChanged updates m_columns
            m_columnList->scrollToItem(m_columnList->item(row));
            return;
        }

        m_columns.append(col);

        m_columnList->blockSignals(true);
        QListWidgetItem *item = new QListWidgetItem(logbookColumnLabel(col));
        item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled
                       | Qt::ItemIsSelectable | Qt::ItemIsEditable
                       | Qt::ItemIsDragEnabled);
        item->setCheckState(col.enabled ? Qt::Checked : Qt::Unchecked);
        m_columnList->addItem(item);
        m_columnList->blockSignals(false);

        m_columnList->setCurrentItem(item);
        updateButtonStates();
    }
}

void LogbookSettingsPage::onRemoveColumn()
{
    int row = m_columnList->currentRow();
    if (row < 0)
        return;

    m_columns.remove(row);
    delete m_columnList->takeItem(row);
    updateButtonStates();
}

void LogbookSettingsPage::onMoveUp()
{
    int row = m_columnList->currentRow();
    if (row <= 0)
        return;

    // Swap in m_columns
    m_columns.swapItemsAt(row, row - 1);

    // Swap in list widget
    m_columnList->blockSignals(true);
    QListWidgetItem *item = m_columnList->takeItem(row);
    m_columnList->insertItem(row - 1, item);
    m_columnList->blockSignals(false);

    m_columnList->setCurrentRow(row - 1);
    updateButtonStates();
}

void LogbookSettingsPage::onMoveDown()
{
    int row = m_columnList->currentRow();
    if (row < 0 || row >= m_columnList->count() - 1)
        return;

    // Swap in m_columns
    m_columns.swapItemsAt(row, row + 1);

    // Swap in list widget
    m_columnList->blockSignals(true);
    QListWidgetItem *item = m_columnList->takeItem(row);
    m_columnList->insertItem(row + 1, item);
    m_columnList->blockSignals(false);

    m_columnList->setCurrentRow(row + 1);
    updateButtonStates();
}

void LogbookSettingsPage::onItemChanged(QListWidgetItem *item)
{
    int row = m_columnList->row(item);
    if (row < 0 || row >= m_columns.size())
        return;

    // Update enabled state from checkbox
    m_columns[row].enabled = (item->checkState() == Qt::Checked);

    // Update customLabel from text
    QString text = item->text();
    QString autoName = logbookColumnDisplayName(m_columns[row]);
    if (text == autoName) {
        m_columns[row].customLabel.clear();
    } else {
        m_columns[row].customLabel = text;
    }
}

void LogbookSettingsPage::saveSettings()
{
    LogbookColumnStore::instance().setColumns(m_columns);
    PreferencesManager::instance().setValue(
        PreferenceKeys::LogbookCacheSize, m_cacheSizeSpinBox->value());
}

} // namespace FlySight
