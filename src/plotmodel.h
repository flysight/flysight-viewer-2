#ifndef PLOTMODEL_H
#define PLOTMODEL_H

#include <QAbstractItemModel>
#include <QHash>
#include <QSettings>
#include <QStringList>
#include <QVector>
#include <memory>
#include <vector>
#include <cstddef>

#include "plotregistry.h"

namespace FlySight {

class PlotModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum PlotRoles {
        DefaultColorRole = Qt::UserRole + 1,
        SensorIDRole,
        MeasurementIDRole,
        PlotUnitsRole,

        // Recommended additions (spec v1.1)
        PlotValueIdRole,   // sensorID + "/" + measurementID
        CategoryRole,
        MeasurementTypeRole  // Returns measurementType for unit conversion
    };

    explicit PlotModel(QObject *parent = nullptr);

    void setSettings(QSettings *settings);

    // Public API (minimum)
    void setPlots(const QVector<PlotValue>& plots);
    QVector<PlotValue> enabledPlots() const;

    bool togglePlot(const QString& sensorId, const QString& measurementId);
    void setPlotEnabled(const QString& sensorId, const QString& measurementId, bool enabled);
    bool isPlotEnabled(const QString& sensorId, const QString& measurementId) const;

    // Applies a profile's list of enabled plots. Every plot of the model is
    // enabled exactly when its id (sensorID + "/" + measurementID, the
    // PlotValueIdRole form a profile stores) is in plotIds, and disabled
    // otherwise. Each change goes through setPlotEnabled(), so the settings
    // key and dataChanged follow as for a click, and whoever watches the
    // model sees a profile exactly as a click. An id for which the model has
    // no plot is ignored: no message, no failure, no row added, because a
    // profile may name plots the application no longer has, or those of a
    // plug-in that is not loaded, and applies without them. The list is
    // only read; nothing rewrites the profile it came from.
    void setEnabledPlotIds(const QStringList& plotIds);

    // QAbstractItemModel
    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

protected:
    QHash<int, QByteArray> roleNames() const override;

private:
    struct Node {
        enum class Type { Category, Plot };
        explicit Node(Type t) : type(t) {}
        Type type;
    };

    struct CategoryNode;

    struct PlotNode : Node {
        PlotNode() : Node(Type::Plot) {}
        PlotValue value;
        bool enabled = false;

        CategoryNode* category = nullptr;
        int categoryRow = -1;
        int plotRow = -1;
    };

    struct CategoryNode : Node {
        CategoryNode() : Node(Type::Category) {}
        QString name;
        std::vector<std::unique_ptr<PlotNode>> plots;
        int row = -1;
    };

    static QString makePlotId(const QString& sensorId, const QString& measurementId);
    static QString makePlotId(const PlotValue& pv);
    static QString settingsKey(const QString& plotId);

    Node* nodeFromIndex(const QModelIndex& index) const;
    CategoryNode* categoryFromIndex(const QModelIndex& index) const;
    PlotNode* plotFromIndex(const QModelIndex& index) const;

    QModelIndex indexForPlot(const PlotNode* node) const;

    QSettings *m_settings = nullptr;
    std::vector<std::unique_ptr<CategoryNode>> m_categories;
    QHash<QString, PlotNode*> m_plotsById;
};

} // namespace FlySight

#endif // PLOTMODEL_H
