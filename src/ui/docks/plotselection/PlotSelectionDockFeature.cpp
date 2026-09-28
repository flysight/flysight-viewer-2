#include "PlotSelectionDockFeature.h"
#include "ui/docks/AppContext.h"
#include "plotmodel.h"
#include <QAbstractItemView>
#include <QSettings>

namespace FlySight {

PlotSelectionDockFeature::PlotSelectionDockFeature(const AppContext& ctx, QObject* parent)
    : DockFeature(parent)
    , m_plotModel(ctx.plotModel)
    , m_settings(ctx.settings)
{
    // Create dock widget with unique name for layout persistence
    m_dock = new KDDockWidgets::QtWidgets::DockWidget(QStringLiteral("Plot Selection"));

    // Create tree view
    m_treeView = new QTreeView(m_dock);
    m_dock->setWidget(m_treeView);

    // Configure tree view
    m_treeView->setModel(ctx.plotModel);
    m_treeView->setHeaderHidden(true);
    m_treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Preserve tree expansion state across model resets
    connect(m_plotModel, &QAbstractItemModel::modelAboutToBeReset,
            this, &PlotSelectionDockFeature::saveExpansionState);
    connect(m_plotModel, &QAbstractItemModel::modelReset,
            this, &PlotSelectionDockFeature::restoreExpansionState);

    // Persist expansion state immediately on user interaction
    connect(m_treeView, &QTreeView::expanded,
            this, &PlotSelectionDockFeature::onExpanded);
    connect(m_treeView, &QTreeView::collapsed,
            this, &PlotSelectionDockFeature::onCollapsed);

    // Restore expansion state for models already populated before dock creation
    restoreExpansionState();
}

void PlotSelectionDockFeature::saveExpansionState()
{
    for (int i = 0; i < m_plotModel->rowCount(); ++i) {
        QModelIndex idx = m_plotModel->index(i, 0);
        QString name = idx.data(Qt::DisplayRole).toString();
        if (m_treeView->isExpanded(idx)) {
            m_expandedCategories.insert(name);
        } else {
            m_expandedCategories.remove(name);
        }
    }
}

void PlotSelectionDockFeature::restoreExpansionState()
{
    // On first load, seed from QSettings
    if (m_expandedCategories.isEmpty() && m_settings) {
        for (int i = 0; i < m_plotModel->rowCount(); ++i) {
            QModelIndex idx = m_plotModel->index(i, 0);
            QString name = idx.data(Qt::DisplayRole).toString();
            if (m_settings->value(QStringLiteral("state/plotExpansion/") + name, false).toBool())
                m_expandedCategories.insert(name);
        }
    }

    for (int i = 0; i < m_plotModel->rowCount(); ++i) {
        QModelIndex idx = m_plotModel->index(i, 0);
        if (m_expandedCategories.contains(idx.data(Qt::DisplayRole).toString())) {
            m_treeView->expand(idx);
        }
    }
}

void PlotSelectionDockFeature::onExpanded(const QModelIndex &index)
{
    if (index.parent().isValid()) return;
    QString name = index.data(Qt::DisplayRole).toString();
    m_expandedCategories.insert(name);
    if (m_settings)
        m_settings->setValue(QStringLiteral("state/plotExpansion/") + name, true);
}

void PlotSelectionDockFeature::onCollapsed(const QModelIndex &index)
{
    if (index.parent().isValid()) return;
    QString name = index.data(Qt::DisplayRole).toString();
    m_expandedCategories.remove(name);
    if (m_settings)
        m_settings->setValue(QStringLiteral("state/plotExpansion/") + name, false);
}

QSet<QString> PlotSelectionDockFeature::expandedCategories() const
{
    return m_expandedCategories;
}

void PlotSelectionDockFeature::setExpandedCategories(const QSet<QString> &categories)
{
    m_expandedCategories = categories;

    for (int i = 0; i < m_plotModel->rowCount(); ++i) {
        QModelIndex idx = m_plotModel->index(i, 0);
        QString name = idx.data(Qt::DisplayRole).toString();
        bool shouldExpand = categories.contains(name);

        if (shouldExpand)
            m_treeView->expand(idx);
        else
            m_treeView->collapse(idx);

        if (m_settings)
            m_settings->setValue(QStringLiteral("state/plotExpansion/") + name, shouldExpand);
    }
}

QString PlotSelectionDockFeature::id() const
{
    return QStringLiteral("Plot Selection");
}

QString PlotSelectionDockFeature::title() const
{
    return QStringLiteral("Plot Selection");
}

KDDockWidgets::QtWidgets::DockWidget* PlotSelectionDockFeature::dock() const
{
    return m_dock;
}

KDDockWidgets::Location PlotSelectionDockFeature::defaultLocation() const
{
    return KDDockWidgets::Location_OnLeft;
}

} // namespace FlySight
