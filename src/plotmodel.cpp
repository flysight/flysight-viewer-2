#include "plotmodel.h"

#include <QSet>
#include <QVariant>

namespace FlySight {

PlotModel::PlotModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

void PlotModel::setSettings(QSettings *settings)
{
    m_settings = settings;
}

QString PlotModel::makePlotId(const QString& sensorId, const QString& measurementId)
{
    return sensorId + "/" + measurementId;
}

QString PlotModel::makePlotId(const PlotValue& pv)
{
    return makePlotId(pv.sensorID, pv.measurementID);
}

QString PlotModel::settingsKey(const QString& plotId)
{
    return QStringLiteral("state/plots/") + plotId;
}

PlotModel::Node* PlotModel::nodeFromIndex(const QModelIndex& index) const
{
    if (!index.isValid())
        return nullptr;
    return static_cast<Node*>(index.internalPointer());
}

PlotModel::CategoryNode* PlotModel::categoryFromIndex(const QModelIndex& index) const
{
    Node* n = nodeFromIndex(index);
    if (!n || n->type != Node::Type::Category)
        return nullptr;
    return static_cast<CategoryNode*>(n);
}

PlotModel::PlotNode* PlotModel::plotFromIndex(const QModelIndex& index) const
{
    Node* n = nodeFromIndex(index);
    if (!n || n->type != Node::Type::Plot)
        return nullptr;
    return static_cast<PlotNode*>(n);
}

QModelIndex PlotModel::indexForPlot(const PlotNode* node) const
{
    if (!node || node->plotRow < 0 || node->categoryRow < 0)
        return {};
    return createIndex(node->plotRow, 0, const_cast<PlotNode*>(node));
}

void PlotModel::setPlots(const QVector<PlotValue>& plots)
{
    // Preserve enabled state by plot ID across resets
    QHash<QString, bool> enabledById;
    enabledById.reserve(m_plotsById.size());
    for (auto it = m_plotsById.constBegin(); it != m_plotsById.constEnd(); ++it) {
        if (it.value()) {
            enabledById.insert(it.key(), it.value()->enabled);
        }
    }

    beginResetModel();

    m_categories.clear();
    m_plotsById.clear();

    QHash<QString, CategoryNode*> categoryByName;
    categoryByName.reserve(16);

    for (const PlotValue& pv : plots) {
        CategoryNode* category = nullptr;

        auto catIt = categoryByName.find(pv.category);
        if (catIt == categoryByName.end()) {
            auto catNode = std::make_unique<CategoryNode>();
            catNode->name = pv.category;
            catNode->row = static_cast<int>(m_categories.size());
            category = catNode.get();

            m_categories.push_back(std::move(catNode));
            categoryByName.insert(pv.category, category);
        } else {
            category = catIt.value();
        }

        const QString id = makePlotId(pv);

        auto plotNode = std::make_unique<PlotNode>();
        plotNode->value = pv;
        plotNode->enabled = enabledById.contains(id)
            ? enabledById.value(id)
            : (m_settings ? m_settings->value(settingsKey(id), false).toBool() : false);

        plotNode->category = category;
        plotNode->categoryRow = category->row;
        plotNode->plotRow = static_cast<int>(category->plots.size());

        PlotNode* plotPtr = plotNode.get();
        category->plots.push_back(std::move(plotNode));

        m_plotsById.insert(id, plotPtr);
    }

    endResetModel();
}

QVector<PlotValue> PlotModel::enabledPlots() const
{
    QVector<PlotValue> out;
    for (const auto& cat : m_categories) {
        for (const auto& plot : cat->plots) {
            if (plot->enabled) {
                out.push_back(plot->value);
            }
        }
    }
    return out;
}

bool PlotModel::togglePlot(const QString& sensorId, const QString& measurementId)
{
    const QString id = makePlotId(sensorId, measurementId);
    PlotNode* node = m_plotsById.value(id, nullptr);
    if (!node) {
        return false;
    }

    const bool newEnabled = !node->enabled;
    setPlotEnabled(sensorId, measurementId, newEnabled);
    return newEnabled;
}

void PlotModel::setPlotEnabled(const QString& sensorId, const QString& measurementId, bool enabled)
{
    const QString id = makePlotId(sensorId, measurementId);
    PlotNode* node = m_plotsById.value(id, nullptr);
    if (!node) {
        return;
    }

    if (node->enabled == enabled) {
        return;
    }

    node->enabled = enabled;

    if (m_settings)
        m_settings->setValue(settingsKey(id), enabled);

    const QModelIndex idx = indexForPlot(node);
    if (idx.isValid()) {
        emit dataChanged(idx, idx, {Qt::CheckStateRole});
    }
}

void PlotModel::setEnabledPlotIds(const QStringList& plotIds)
{
    // The walk is over the model's own rows, so an id the model does not
    // have is never looked up, let alone reported. The rows are copied
    // first because each change emits dataChanged, and a watcher must not
    // be able to pull the rows out from under the walk.
    const QSet<QString> listed(plotIds.cbegin(), plotIds.cend());
    QVector<PlotValue> rows;
    for (const auto& cat : m_categories) {
        for (const auto& plot : cat->plots)
            rows.push_back(plot->value);
    }
    for (const PlotValue& pv : rows)
        setPlotEnabled(pv.sensorID, pv.measurementID, listed.contains(makePlotId(pv)));
}

bool PlotModel::isPlotEnabled(const QString& sensorId, const QString& measurementId) const
{
    const QString id = makePlotId(sensorId, measurementId);
    PlotNode* node = m_plotsById.value(id, nullptr);
    return node ? node->enabled : false;
}

QModelIndex PlotModel::index(int row, int column, const QModelIndex& parent) const
{
    if (column != 0 || row < 0) {
        return {};
    }

    if (!parent.isValid()) {
        if (row >= static_cast<int>(m_categories.size())) {
            return {};
        }
        return createIndex(row, column, m_categories[static_cast<std::size_t>(row)].get());
    }

    // Only a two-level hierarchy: parent must be a category
    CategoryNode* category = categoryFromIndex(parent);
    if (!category) {
        return {};
    }

    if (row >= static_cast<int>(category->plots.size())) {
        return {};
    }

    return createIndex(row, column, category->plots[static_cast<std::size_t>(row)].get());
}

QModelIndex PlotModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) {
        return {};
    }

    Node* n = nodeFromIndex(child);
    if (!n) {
        return {};
    }

    if (n->type == Node::Type::Category) {
        return {};
    }

    PlotNode* plot = static_cast<PlotNode*>(n);
    if (!plot->category) {
        return {};
    }

    return createIndex(plot->categoryRow, 0, plot->category);
}

int PlotModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid()) {
        return static_cast<int>(m_categories.size());
    }

    Node* n = nodeFromIndex(parent);
    if (!n) {
        return 0;
    }

    if (n->type == Node::Type::Category) {
        CategoryNode* category = static_cast<CategoryNode*>(n);
        return static_cast<int>(category->plots.size());
    }

    return 0;
}

int PlotModel::columnCount(const QModelIndex& /*parent*/) const
{
    return 1;
}

QVariant PlotModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return {};
    }

    Node* n = nodeFromIndex(index);
    if (!n) {
        return {};
    }

    if (n->type == Node::Type::Category) {
        CategoryNode* category = static_cast<CategoryNode*>(n);
        if (role == Qt::DisplayRole) {
            return category->name;
        }
        return {};
    }

    PlotNode* plot = static_cast<PlotNode*>(n);

    switch (role) {
    case Qt::DisplayRole:
        return plot->value.plotName;
    case Qt::CheckStateRole:
        return plot->enabled ? Qt::Checked : Qt::Unchecked;
    case DefaultColorRole:
        return plot->value.defaultColor;
    case SensorIDRole:
        return plot->value.sensorID;
    case MeasurementIDRole:
        return plot->value.measurementID;
    case PlotUnitsRole:
        return plot->value.plotUnits;
    case PlotValueIdRole:
        return makePlotId(plot->value);
    case CategoryRole:
        return plot->value.category;
    case MeasurementTypeRole:
        return plot->value.measurementType;
    default:
        return {};
    }
}

bool PlotModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid()) {
        return false;
    }

    PlotNode* plot = plotFromIndex(index);
    if (!plot) {
        return false;
    }

    if (role != Qt::CheckStateRole) {
        return false;
    }

    bool enabled = false;
    if (value.canConvert<int>()) {
        enabled = (static_cast<Qt::CheckState>(value.toInt()) == Qt::Checked);
    } else if (value.canConvert<bool>()) {
        enabled = value.toBool();
    } else {
        return false;
    }

    if (plot->enabled == enabled) {
        return true;
    }

    plot->enabled = enabled;

    if (m_settings)
        m_settings->setValue(settingsKey(makePlotId(plot->value)), enabled);

    emit dataChanged(index, index, {Qt::CheckStateRole});
    return true;
}

Qt::ItemFlags PlotModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }

    Node* n = nodeFromIndex(index);
    if (!n) {
        return Qt::NoItemFlags;
    }

    if (n->type == Node::Type::Category) {
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
}

QVariant PlotModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && section == 0 && role == Qt::DisplayRole) {
        return tr("Plots");
    }
    return {};
}

QHash<int, QByteArray> PlotModel::roleNames() const
{
    QHash<int, QByteArray> roles = QAbstractItemModel::roleNames();
    roles[Qt::DisplayRole] = "display";
    roles[Qt::CheckStateRole] = "checkState";
    roles[DefaultColorRole] = "defaultColor";
    roles[SensorIDRole] = "sensorId";
    roles[MeasurementIDRole] = "measurementId";
    roles[PlotUnitsRole] = "plotUnits";
    roles[PlotValueIdRole] = "plotValueId";
    roles[CategoryRole] = "category";
    roles[MeasurementTypeRole] = "measurementType";
    return roles;
}

} // namespace FlySight
