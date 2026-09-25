// LogbookView.cpp
#include "LogbookView.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QDebug>
#include <QFontMetrics>
#include <QItemSelectionModel>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QStyle>

#include "attributeregistry.h"
#include "LogbookCellDelegate.h"
#include "LogbookHeaderView.h"

namespace FlySight {

LogbookView::LogbookView(SessionModel *model, CalculationDemand *demand, QWidget *parent)
    : QWidget(parent),
      treeView(new QTreeView(this)),
      model(model),
      m_demand(demand)
{
    QIcon closeIcon = style()->standardIcon(QStyle::SP_TitleBarCloseButton);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setVisible(false);
    m_progressBar->setTextVisible(true);

    m_cancelButton = new QToolButton(this);
    m_cancelButton->setIcon(closeIcon);
    m_cancelButton->setAutoRaise(true);
    m_cancelButton->setVisible(false);
    connect(m_cancelButton, &QToolButton::clicked, this, [this]() {
        emit cancelRequested(m_activeTaskId);
    });

    QHBoxLayout *progressLayout = new QHBoxLayout();
    progressLayout->addWidget(m_progressBar, 1);
    progressLayout->addWidget(m_cancelButton);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(treeView);
    layout->addLayout(progressLayout);
    setLayout(layout);

    setupView();

    treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(treeView, &QTreeView::customContextMenuRequested, this, &LogbookView::onContextMenuRequested);
    connect(treeView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &current, const QModelIndex &/*previous*/) {
        if (current.isValid()) {
            QString sessionId = this->model->rowAt(current.row()).sessionId;
            emit currentSessionChanged(sessionId);
        } else {
            emit currentSessionChanged(QString());
        }
    });

    connect(treeView, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (index.isValid())
            emit focusSessionRequested(index.row());
    });
}

QList<QModelIndex> LogbookView::selectedRows() const {
    return treeView->selectionModel()->selectedRows();
}

void LogbookView::setupView()
{
    // The header before the model: the tree hands it the model and its sort
    // settings, and the lines below configure it
    treeView->setHeader(new LogbookHeaderView(model, m_demand, treeView));
    treeView->setModel(model);
    treeView->setItemDelegate(new LogbookCellDelegate(model, m_demand, treeView));
    treeView->setRootIsDecorated(false);
    treeView->header()->setDefaultSectionSize(100);

    // Ensure header is tall enough for two-line headers (name + unit)
    QFontMetrics fm(treeView->header()->font());
    treeView->header()->setFixedHeight(fm.height() * 2 + 8);
    treeView->setMouseTracking(true); // Enable mouse tracking

    // Set selection mode to ExtendedSelection to allow multiple selections
    treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);

    // Set selection behavior to select entire rows
    treeView->setSelectionBehavior(QAbstractItemView::SelectRows);

    // Edit on F2 or slow second click, not double-click (double-click is used for focus)
    treeView->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);

    // Optimize performance for large models
    treeView->setUniformRowHeights(true);

    // Install event filter to detect hover events on treeView
    treeView->viewport()->installEventFilter(this);

    // Enable sorting on the view
    treeView->setSortingEnabled(true);
}

bool LogbookView::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == treeView->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
            QPoint pos = mouseEvent->pos();
            QModelIndex index = treeView->indexAt(pos);
            if (index.isValid()) {
                QString sessionId = model->rowAt(index.row()).sessionId;
                if (model->hoveredSessionId() != sessionId) {
                    model->setHoveredSessionId(sessionId);
                }
            } else {
                if (!model->hoveredSessionId().isEmpty()) {
                    model->setHoveredSessionId(QString());
                }
            }
        } else if (event->type() == QEvent::Leave) {
            if (!model->hoveredSessionId().isEmpty()) {
                model->setHoveredSessionId(QString());
            }
        }
    }

    // Allow the base class to process other events
    return QWidget::eventFilter(obj, event);
}

void LogbookView::onContextMenuRequested(const QPoint &pos)
{
    QMenu menu(this);

    QAction *showSelectedAction = menu.addAction(tr("Show Selected Tracks"));
    QAction *hideSelectedAction = menu.addAction(tr("Hide Selected Tracks"));
    QAction *hideOthersAction = menu.addAction(tr("Hide Others"));

    // The menu and the input dialog below each run a nested event loop, during
    // which rows can be reordered, added or removed and columns rebuilt. Row
    // and column indices are therefore not kept across them: the selection is
    // captured as session ids and the column as its attribute key, and both are
    // resolved to indices again once the dialogs have closed.
    QStringList selectedSessionIds;
    for (const QModelIndex &idx : treeView->selectionModel()->selectedRows())
        selectedSessionIds.append(model->rowAt(idx.row()).sessionId);

    // --- Editable column actions ---
    QList<QAction*> editActions;
    const auto &reg = AttributeRegistry::instance();
    for (int i = 0; i < model->columnCount(); ++i) {
        const LogbookColumn &col = model->column(i);
        if (col.type != ColumnType::SessionAttribute)
            continue;
        const auto *def = reg.findByKey(col.attributeKey);
        if (!def || !def->editable)
            continue;
        if (editActions.isEmpty())
            menu.addSeparator();
        QAction *action = menu.addAction(tr("Set %1...").arg(logbookColumnLabel(col)));
        action->setData(col.attributeKey);
        action->setProperty("columnLabel", logbookColumnLabel(col));
        editActions.append(action);
    }

    menu.addSeparator();
    QAction *deleteAction = menu.addAction(tr("Delete Selected Tracks"));

    QAction *chosenAction = menu.exec(treeView->viewport()->mapToGlobal(pos));
    if (!chosenAction) {
        return;
    } else if (chosenAction == showSelectedAction) {
        emit showSelectedRequested();
    } else if (chosenAction == hideSelectedAction) {
        emit hideSelectedRequested();
    } else if (chosenAction == hideOthersAction) {
        emit hideOthersRequested();
    } else if (chosenAction == deleteAction) {
        emit deleteRequested();
    } else if (editActions.contains(chosenAction)) {
        const QString attributeKey = chosenAction->data().toString();
        const QString columnLabel = chosenAction->property("columnLabel").toString();
        if (selectedSessionIds.isEmpty())
            return;

        bool ok = false;
        const QString value = QInputDialog::getText(
            this,
            tr("Set %1").arg(columnLabel),
            tr("New value for %n session(s):", "", selectedSessionIds.size()),
            QLineEdit::Normal,
            QString(),
            &ok);

        if (!ok)
            return;

        // Resolve the column and the sessions as they are now. A session that
        // is gone, or a column that was removed, is skipped rather than
        // letting the value land somewhere else.
        int colIndex = -1;
        for (int i = 0; i < model->columnCount(); ++i) {
            const LogbookColumn &col = model->column(i);
            if (col.type == ColumnType::SessionAttribute && col.attributeKey == attributeKey) {
                colIndex = i;
                break;
            }
        }
        if (colIndex < 0)
            return;

        QList<int> rowIndices;
        rowIndices.reserve(selectedSessionIds.size());
        for (const QString &sessionId : selectedSessionIds) {
            const int row = model->getSessionRow(sessionId);
            if (row >= 0)
                rowIndices.append(row);
        }
        model->startBulkEdit(rowIndices, colIndex, value);
    }
}

void LogbookView::selectSessions(const QList<QString> &sessionIds)
{
    QItemSelection selection;

    // Iterate over the session IDs and find corresponding rows
    for (const QString &sessionId : sessionIds) {
        int row = model->getSessionRow(sessionId);
        if (row >= 0) {
            // Select the entire row by selecting from column 0 to the last column
            QModelIndex topLeft = model->index(row, 0);
            QModelIndex bottomRight = model->index(row, model->columnCount() - 1);
            selection.select(topLeft, bottomRight);
        } else {
            qWarning() << "LogbookView::selectSessions: SESSION_ID not found -" << sessionId;
        }
    }

    // Apply the selection
    QItemSelectionModel *selectionModel = treeView->selectionModel();
    if (!selectionModel) {
        qWarning() << "LogbookView::selectSessions: No selection model available.";
        return;
    }

    // Clear existing selection and select the new sessions
    selectionModel->clearSelection();
    selectionModel->select(selection, QItemSelectionModel::Select | QItemSelectionModel::Rows);

    // Optionally, scroll to the first selected session
    if (!sessionIds.isEmpty()) {
        QString firstSessionId = sessionIds.first();
        int firstRow = model->getSessionRow(firstSessionId);
        if (firstRow >= 0) {
            QModelIndex firstIndex = model->index(firstRow, 0);
            treeView->scrollTo(firstIndex, QAbstractItemView::PositionAtCenter);
        }
    }
}

QSize LogbookView::minimumSizeHint() const
{
    // Return a constant to prevent KDDockWidgets from resizing the dock
    // when progress bars are shown or hidden.
    return QSize(0, 0);
}

void LogbookView::onActiveTaskChanged(int id, bool cancellable)
{
    m_activeTaskId = id;
    m_cancelButton->setVisible(cancellable);
    m_progressBar->setVisible(true);
}

void LogbookView::onProgressChanged(int id, int remaining, int total)
{
    if (id != m_activeTaskId)
        return;

    m_progressBar->setRange(0, total);
    m_progressBar->setValue(total - remaining);

    QString label;
    switch (id) {
    case SessionModel::SaveTask:
        label = tr("Saving sessions: %v / %m");
        break;
    case SessionModel::LoadTask:
        label = tr("Loading sessions: %v / %m");
        break;
    case SessionModel::BulkEditTask:
        label = tr("Updating sessions: %v / %m");
        break;
    case SessionModel::ColumnTask:
        label = tr("Computing columns: %v / %m");
        break;
    case SessionModel::ColumnFillTask:
        // The same text on purpose: to the user a column fills in the
        // background the same way whether its values are cheap or requested
        label = tr("Computing columns: %v / %m");
        break;
    default:
        label = tr("Working: %v / %m");
        break;
    }
    m_progressBar->setFormat(label);
}

void LogbookView::onSchedulerIdle()
{
    m_activeTaskId = -1;
    m_progressBar->setVisible(false);
    m_cancelButton->setVisible(false);
}

} // namespace FlySight
