#include "PlotWidget.h"
#include "plotmodel.h"
#include "plotregistry.h"
#include "markermodel.h"
#include "momentmodel.h"
#include "plotviewsettingsmodel.h"
#include "plotrangemodel.h"
#include "preferences/preferencesmanager.h"
#include "preferences/preferencekeys.h"
#include "units/unitconverter.h"

#include <QMenu>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QPixmap>
#include <QBitmap>
#include <QPainter>
#include <QApplication>
#include <QDebug>
#include <QSignalBlocker>
#include <QNativeGestureEvent>

#include "plottool/plottool.h"
#include "plottool/pantool.h"
#include "plottool/zoomtool.h"
#include "plottool/selecttool.h"
#include "plottool/setexittool.h"
#include "plottool/setsynctool.h"
#include "plottool/setgroundtool.h"
#include "plottool/setcoursetool.h"
#include "plottool/measuretool.h"
#include "plotutils.h"
#include "calculations/timecalculations.h"
#include "engine/calculationregistry.h"

namespace {

/**
 * @brief Custom axis ticker that converts SI values to display units.
 *
 * This ticker applies a linear transformation (scale + offset) to tick values
 * to display them in the user's preferred unit system.
 */
class UnitConvertingTicker : public QCPAxisTicker
{
public:
    void setMeasurementType(const QString &type) { m_measurementType = type; }
    void setScale(double scale) { m_scale = scale; }
    void setOffset(double offset) { m_offset = offset; }
    void setPrecision(int precision) { m_precision = precision; }

protected:
    QString getTickLabel(double tick, const QLocale &locale, QChar formatChar, int precision) override
    {
        Q_UNUSED(locale)
        Q_UNUSED(formatChar)

        // Convert SI value to display units
        double displayValue = (tick * m_scale) + m_offset;

        // Use our precision, not the default
        int usePrecision = (m_precision >= 0) ? m_precision : precision;
        return QString::number(displayValue, 'f', usePrecision);
    }

private:
    QString m_measurementType;
    double m_scale = 1.0;
    double m_offset = 0.0;
    int m_precision = -1;
};

constexpr int kLaneHeightPx    = 32;   // height of one marker lane above the plot
constexpr int kMinTopMarginPx  = 10;   // clearance so the topmost y-axis tick label isn't clipped

} // anonymous namespace

namespace FlySight {

// Constructor
PlotWidget::PlotWidget(SessionModel *model,
                       PlotModel *plotModel,
                       MarkerModel *markerModel,
                       PlotViewSettingsModel *viewSettingsModel,
                       MomentModel *momentModel,
                       PlotRangeModel *rangeModel,
                       MeasureModel *measureModel,
                       QWidget *parent)
    : QWidget(parent)
    , customPlot(new QCustomPlot(this))
    , model(model)
    , plotModel(plotModel)
    , markerModel(markerModel)
    , m_viewSettingsModel(viewSettingsModel)
    , m_momentModel(momentModel)
    , m_rangeModel(rangeModel)
    , m_xVariable(SessionKeys::Time)
    , m_referenceMarkerKey(SessionKeys::ExitTime)
    , m_xAxisLabel(tr("Time from exit (s)"))
{
    if (m_viewSettingsModel) {
        m_xVariable = m_viewSettingsModel->xVariable();
        m_referenceMarkerKey = m_viewSettingsModel->referenceMarkerKey();
        m_xAxisLabel = m_viewSettingsModel->xAxisLabel();

        connect(m_viewSettingsModel, &PlotViewSettingsModel::xVariableChanged,
                this, &PlotWidget::onXVariableChanged);
        connect(m_viewSettingsModel, &PlotViewSettingsModel::referenceMarkerKeyChanged,
                this, &PlotWidget::onReferenceMarkerKeyChanged);
    }

    // set up the layout with the custom plot
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(customPlot);
    setLayout(layout);

    // initialize the plot and crosshairs
    setupPlot();

    // create the plot context for tools
    PlotContext ctx;
    ctx.widget = this;
    ctx.plot = customPlot;
    ctx.graphMap = &m_graphInfoMap;
    ctx.model = model;
    ctx.plotModel = plotModel;
    ctx.measureModel = measureModel;

    // instantiate tools for interacting with the plot
    m_panTool = std::make_unique<PanTool>(ctx);
    m_zoomTool = std::make_unique<ZoomTool>(ctx);
    m_measureTool = std::make_unique<MeasureTool>(ctx);
    m_selectTool = std::make_unique<SelectTool>(ctx);
    m_setExitTool = std::make_unique<SetExitTool>(ctx);
    m_setSyncTool = std::make_unique<SetSyncTool>(ctx);
    m_setGroundTool = std::make_unique<SetGroundTool>(ctx);
    m_setCourseTool = std::make_unique<SetCourseTool>(ctx);

    m_currentTool = m_panTool.get();
    m_primaryTool = Tool::Pan;

    // install an event filter to capture mouse events
    customPlot->installEventFilter(this);

    // create crosshair manager
    m_crosshairManager = std::make_unique<CrosshairManager>(
        customPlot,
        model,
        &m_graphInfoMap, // existing QMap<QCPGraph*, GraphInfo>
        this
        );

    // Update MomentModel when traced sessions change (e.g. Shift press/release)
    connect(m_crosshairManager.get(), &CrosshairManager::tracedSessionsChanged,
            this, [this](const QSet<QString> &tracedSessions) {
        if (!m_momentModel || !m_mouseInPlotArea)
            return;

        // Recompute per-session UTC positions from current cursor location
        const QPoint pos = customPlot->mapFromGlobal(QCursor::pos());
        writeMouseMoment(pos, tracedSessions);
    });

    if (m_momentModel) {
        connect(m_momentModel, &MomentModel::momentsChanged,
                this, &PlotWidget::scheduleMomentUpdate);
    }

    // connect signals to slots for updates and interactions
    connect(model, &SessionModel::modelChanged, this, &PlotWidget::schedulePlotRebuild);
    connect(model, &SessionModel::visibilityChanged, this,
            [this](const QSet<QString> &, const QSet<QString> &) { schedulePlotRebuild(); });
    connect(model, &SessionModel::dependencyChanged,
            this, &PlotWidget::onDependencyChanged);

    if (plotModel) {
        connect(plotModel, &QAbstractItemModel::modelReset,
                this, &PlotWidget::updatePlot);

        connect(plotModel, &QAbstractItemModel::dataChanged,
                this,
                [this](const QModelIndex&, const QModelIndex&, const QVector<int>&) {
                    updatePlot();
                });
    }

    connect(customPlot->xAxis, QOverload<const QCPRange &>::of(&QCPAxis::rangeChanged),
            this, &PlotWidget::onXAxisRangeChanged);

    connect(model, &SessionModel::hoveredSessionChanged,
            this, &PlotWidget::onHoveredSessionChanged);

    // Connect to preferences system
    connect(&PreferencesManager::instance(), &PreferencesManager::preferenceChanged,
            this, &PlotWidget::onPreferenceChanged);

    // Connect to unit system changes for reactive updates
    connect(&UnitConverter::instance(), &UnitConverter::systemChanged,
            this, &PlotWidget::updatePlot);

    // Apply initial preferences and theme colors
    applyPlotPreferences();
    applyThemeColors();

    // System theme changes at runtime arrive as ApplicationPaletteChange (see
    // changeEvent).

    // Initialize coalescing timer for dependencyChanged signals
    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(0);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &PlotWidget::updatePlot);

    // Initialize coalescing timer for lightweight marker-only updates
    m_markerUpdateTimer.setSingleShot(true);
    m_markerUpdateTimer.setInterval(0);
    connect(&m_markerUpdateTimer, &QTimer::timeout, this, &PlotWidget::updateMarkersOnly);

    // Initialize coalescing timer for moment-driven updates
    m_momentUpdateTimer.setSingleShot(true);
    m_momentUpdateTimer.setInterval(0);
    connect(&m_momentUpdateTimer, &QTimer::timeout, this, &PlotWidget::onMomentsChanged);

    // update the plot with initial data
    updatePlot();

    // Ensure ticker matches the initial x-axis mode (especially for UTC time)
    updateXAxisTicker();

    // Initialize viewport shift tracking from the reference session
    withReferenceSession([this](const SessionData &ref) {
        m_lastRefSessionId = ref.getAttribute(SessionKeys::SessionId).toString();
        m_lastRefOffset = referenceOffsetForSession(ref).value_or(0.0);
    });
}

PlotWidget::~PlotWidget() = default;

// Public Methods
void PlotWidget::setCurrentTool(Tool tool)
{
    // Close the old tool
    if (m_currentTool)
        m_currentTool->closeTool();

    // Switch to the appropriate tool based on the provided enum
    switch (tool) {
    case Tool::Pan:
        m_currentTool = m_panTool.get();
        break;
    case Tool::Zoom:
        m_currentTool = m_zoomTool.get();
        break;
    case Tool::Measure:
        m_currentTool = m_measureTool.get();
        break;
    case Tool::Select:
        m_currentTool = m_selectTool.get();
        break;
    case Tool::SetExit:
        m_currentTool = m_setExitTool.get();
        break;
    case Tool::SetSync:
        m_currentTool = m_setSyncTool.get();
        break;
    case Tool::SetGround:
        m_currentTool = m_setGroundTool.get();
        break;
    case Tool::SetCourse:
        m_currentTool = m_setCourseTool.get();
        break;
    }

    // Update previous primary tool
    if (m_currentTool->isPrimary()) {
        m_primaryTool = tool;
    }

    // Activate the new tool
    if (m_currentTool)
        m_currentTool->activateTool();

    // Finally, notify
    emit toolChanged(tool);
}

void PlotWidget::revertToPrimaryTool()
{
    setCurrentTool(m_primaryTool);
}

void PlotWidget::zoomToExtent()
{
    // Use all visible sessions, named by id: the sessions themselves are
    // looked up in the model when they are read.
    QStringList visible;
    for (int i = 0; i < model->rowCount(); ++i) {
        const SessionRow &row = model->rowAt(i);
        if (!row.isLoaded() || !row.visible) continue;
        visible.append(row.sessionId);
    }
    zoomToExtent(visible);
}

void PlotWidget::zoomToExtent(const QStringList &sessionIds)
{
    PreferencesManager &prefs = PreferencesManager::instance();
    const QString mode = prefs.getValue(PreferenceKeys::ZoomExtentMode).toString();
    const QString startMarkerKey = prefs.getValue(PreferenceKeys::ZoomExtentStartMarker).toString();
    const QString endMarkerKey = prefs.getValue(PreferenceKeys::ZoomExtentEndMarker).toString();
    const double marginPct = prefs.getValue(PreferenceKeys::ZoomExtentMarginPct).toDouble();

    double minX = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    bool hasData = false;

    // The model's live sessions, not copies: a copied SessionData has no
    // engine cache, so every read of a copy would recompute conversion, time
    // fit and markers and throw them away. The model resolves each id to its
    // row as it is visited and holds a RowStabilityGuard meanwhile, so no
    // session reference outlives one call of the lambda, and anything that
    // loaded, evicted or moved a row during these reads would assert. Ids that
    // are unknown or not loaded are skipped; nothing is loaded for a zoom.
    model->forEachLoadedSession(sessionIds, [&](const SessionData &session) {
        auto offset = referenceOffsetForSession(session);
        if (!offset.has_value())
            return;

        // Try marker range if configured
        if (mode == "markerRange") {
            auto startVal = markerOffsetSeconds(session, startMarkerKey, m_xVariable);
            auto endVal   = markerOffsetSeconds(session, endMarkerKey, m_xVariable);

            if (startVal.has_value() && endVal.has_value()) {
                double aMin = startVal.value() - offset.value();
                double aMax = endVal.value()   - offset.value();
                if (aMin > aMax) std::swap(aMin, aMax);
                minX = std::min(minX, aMin);
                maxX = std::max(maxX, aMax);
                hasData = true;
                return;
            }
            // Fallback to full data extent for this session
        }

        // Full data extent: scan all sensors for x-variable data
        for (const QString &sensorKey : session.sensorKeys()) {
            QVector<double> xData = session.getMeasurement(sensorKey, m_xVariable);
            if (xData.isEmpty())
                continue;

            QVector<double> adjusted = xData;
            if (offset.value() != 0.0) {
                for (double &x : adjusted)
                    x -= offset.value();
            }

            auto [minIt, maxIt] = std::minmax_element(adjusted.begin(), adjusted.end());
            minX = std::min(minX, *minIt);
            maxX = std::max(maxX, *maxIt);
            hasData = true;
        }
    });

    // The guard is gone: setting the range emits, and slots may use the model.
    if (!hasData || minX >= maxX)
        return;

    // Apply symmetric margin
    if (marginPct > 0.0) {
        double range = maxX - minX;
        double margin = range * marginPct / (100.0 - 2.0 * marginPct);
        minX -= margin;
        maxX += margin;
    }

    customPlot->xAxis->setRange(minX, maxX);
}

void PlotWidget::handleSessionsSelected(const QList<QString> &sessionIds)
{
    // emit the sessionsSelected signal with the provided session IDs
    qDebug() << "Selected sessions:" << sessionIds;
    emit sessionsSelected(sessionIds);
}

CrosshairManager* PlotWidget::crosshairManager() const
{
    return m_crosshairManager.get();
}

void PlotWidget::lockFocusToSession(const QString &sessionId)
{
    if (m_crosshairManager)
        m_crosshairManager->setToolFocusLock(sessionId);
}

void PlotWidget::unlockFocus()
{
    if (m_crosshairManager)
        m_crosshairManager->clearToolFocusLock();
}

double PlotWidget::xCoordToUtcSeconds(double xCoord, const QString &sessionId) const
{
    int row = model->getSessionRow(sessionId);
    if (row < 0)
        return 0.0;

    const SessionData &session = model->sessionRef(row);
    auto offset = referenceOffsetForSession(session);
    if (!offset.has_value())
        return 0.0;

    // Plot-to-UTC: add offset back
    return xCoord + offset.value();
}

// Slots
void PlotWidget::updatePlot()
{
    m_pendingRebuildLevel = RebuildLevel::None;

    // Clear existing graphs and axes
    customPlot->clearPlottables();
    m_graphInfoMap.clear();
    m_graphDrawOrder.clear();

    const QList<QCPAxis *> axesToRemove = m_plotValueAxes.values();
    m_plotValueAxes.clear();
    for (auto axis : axesToRemove) {
        customPlot->axisRect()->removeAxis(axis);
    }

    QString hoveredSessionId = model->hoveredSessionId(); // Retrieve hovered session ID

    // Add graphs for each enabled plot
    const QVector<PlotValue> plots = plotModel ? plotModel->enabledPlots() : QVector<PlotValue>{};
    for (const PlotValue &pv : plots) {
        // Retrieve metadata for the graph
        QColor color = pv.defaultColor;

        // Check for user-configured color preference
        QString colorKey = PreferenceKeys::plotColorKey(pv.sensorID, pv.measurementID);
        QVariant colorPref = PreferencesManager::instance().getValue(colorKey);
        if (colorPref.isValid()) {
            QColor prefColor = colorPref.value<QColor>();
            if (prefColor.isValid()) {
                color = prefColor;
            }
        }

        QString sensorID = pv.sensorID;
        QString measurementID = pv.measurementID;
        QString plotName = pv.plotName;
        QString plotUnits = pv.plotUnits;

        QString plotValueID = sensorID + "/" + measurementID;

        // Create a new y-axis if one doesn't exist for this plot value
        if (!m_plotValueAxes.contains(plotValueID)) {
            QCPAxis *newYAxis = customPlot->axisRect()->addAxis(QCPAxis::atLeft);

            // Determine display unit label based on measurementType
            QString displayUnits = plotUnits; // Fallback to static plotUnits
            if (!pv.measurementType.isEmpty()) {
                QString convertedLabel = UnitConverter::instance().getUnitLabel(pv.measurementType);
                if (!convertedLabel.isEmpty()) {
                    displayUnits = convertedLabel;
                }
            }

            if (!displayUnits.isEmpty()) {
                newYAxis->setLabel(plotName + " (" + displayUnits + ")");
            } else {
                newYAxis->setLabel(plotName);
            }
            newYAxis->setLabelColor(color);
            newYAxis->setTickLabelColor(color);
            newYAxis->setBasePen(QPen(color));
            newYAxis->setTickPen(QPen(color));
            newYAxis->setSubTickPen(QPen(color));

            // Configure unit-converting ticker for this axis
            if (!pv.measurementType.isEmpty()) {
                auto ticker = QSharedPointer<UnitConvertingTicker>::create();
                ticker->setMeasurementType(pv.measurementType);

                // Get conversion parameters: scale = convert(1) - convert(0), offset = convert(0)
                double offset = UnitConverter::instance().convert(0.0, pv.measurementType);
                double scale = UnitConverter::instance().convert(1.0, pv.measurementType) - offset;
                ticker->setScale(scale);
                ticker->setOffset(offset);
                ticker->setPrecision(UnitConverter::instance().getPrecision(pv.measurementType));

                newYAxis->setTicker(ticker);
            }

            // Apply theme-appropriate grid colors
            QColor gridFg = QApplication::palette().color(QPalette::Text);
            QColor gridColor = gridFg;
            gridColor.setAlpha(40);
            QColor zeroLineColor = gridFg;
            zeroLineColor.setAlpha(60);
            newYAxis->grid()->setPen(QPen(gridColor, 0, Qt::DotLine));
            newYAxis->grid()->setZeroLinePen(QPen(zeroLineColor, 0, Qt::SolidLine));

            m_plotValueAxes.insert(plotValueID, newYAxis);
        }

        QCPAxis *assignedYAxis = m_plotValueAxes.value(plotValueID);

        // Add graphs for each visible session
        for (int si = 0; si < model->rowCount(); ++si) {
            QVector<double> xData;
            QVector<double> yData;
            QString graphSessionId;
            {
                // The session is read, into plain values, under a row
                // stability guard that ends before the graph is created. Only
                // loaded rows are plotted, so the row is read in place:
                // nothing is loaded here.
                const SessionModel::RowStabilityGuard guard(*model);

                const SessionRow &sr = model->rowAt(si);
                if (!sr.isLoaded() || !sr.visible)
                    continue;

                const SessionData &session = sr.session.value();

                yData = session.getMeasurement(sensorID, measurementID);
                if (yData.isEmpty()) {
                    qWarning() << "No data available for plot:" << plotName << "in session:" << session.getAttribute(SessionKeys::SessionId);
                    continue;
                }

                auto offset = referenceOffsetForSession(session);
                if (!offset.has_value())
                    continue;  // session lacks reference marker value; skip it

                xData = session.getMeasurement(sensorID, m_xVariable);
                if (xData.isEmpty() || xData.size() != yData.size()) {
                    qWarning() << "Time and measurement data size mismatch for session:" << session.getAttribute(SessionKeys::SessionId);
                    continue;
                }

                if (offset.value() != 0.0) {
                    for (double &x : xData)
                        x -= offset.value();
                }

                graphSessionId = session.getAttribute(SessionKeys::SessionId).toString();
            }

            GraphInfo info;
            info.sessionId = graphSessionId;
            info.sensorId = sensorID;
            info.measurementId = measurementID;

            // Determine display unit label based on measurementType (same logic as Y-axis labels)
            QString graphDisplayUnits = plotUnits; // Fallback
            if (!pv.measurementType.isEmpty()) {
                QString convertedLabel = UnitConverter::instance().getUnitLabel(pv.measurementType);
                if (!convertedLabel.isEmpty()) {
                    graphDisplayUnits = convertedLabel;
                }
            }

            if (!graphDisplayUnits.isEmpty()) {
                info.displayName = QString("%1 (%2)").arg(plotName).arg(graphDisplayUnits);
            } else {
                info.displayName = plotName;
            }
            info.defaultPen = QPen(QColor(color), m_lineThickness);

            QCPGraph *graph = customPlot->addGraph(customPlot->xAxis, assignedYAxis);
            graph->setPen(determineGraphPen(info, hoveredSessionId));
            graph->setData(xData, yData);
            graph->setLineStyle(QCPGraph::lsLine);
            graph->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssNone));
            graph->setLayer(determineGraphLayer(info, hoveredSessionId));

            m_graphInfoMap.insert(graph, info);
            m_graphDrawOrder.append(graph);
        }
    }

    updateReferenceMarkers(UpdateMode::Rebuild);

    // Adjust y-axis ranges based on the updated x-axis range
    onXAxisRangeChanged(customPlot->xAxis->range());

    // Update viewport shift tracking for the reference session
    withReferenceSession([this](const SessionData &ref) {
        m_lastRefSessionId = ref.getAttribute(SessionKeys::SessionId).toString();
        m_lastRefOffset = referenceOffsetForSession(ref).value_or(0.0);
    });
}

void PlotWidget::updateMarkersOnly()
{
    // Marker-only update path: do not rebuild graphs
    updateReferenceMarkers(UpdateMode::Rebuild);

    // Rebuild may change axisRect margins (lane count), which changes coordToPixel mapping.
    // Force QCustomPlot to apply the new layout before we position absolute marker items.
    customPlot->replot(QCustomPlot::rpImmediateRefresh);

    updateReferenceMarkers(UpdateMode::Reflow);
    customPlot->replot(QCustomPlot::rpQueuedReplot);
}

void PlotWidget::applyPlotPreferences()
{
    auto &prefs = PreferencesManager::instance();

    // Cache global settings
    m_lineThickness = prefs.getValue(PreferenceKeys::PlotsLineThickness).toDouble();
    m_textSize = prefs.getValue(PreferenceKeys::PlotsTextSize).toInt();
    m_yAxisPadding = prefs.getValue(PreferenceKeys::PlotsYAxisPadding).toDouble();

    // Apply text size to axis labels
    QFont axisFont = customPlot->xAxis->labelFont();
    axisFont.setPointSize(m_textSize);
    customPlot->xAxis->setLabelFont(axisFont);
    customPlot->xAxis->setTickLabelFont(axisFont);

    // Apply to all Y axes
    for (auto it = m_plotValueAxes.constBegin(); it != m_plotValueAxes.constEnd(); ++it) {
        QCPAxis *yAxis = it.value();
        yAxis->setLabelFont(axisFont);
        yAxis->setTickLabelFont(axisFont);
    }
}

void PlotWidget::applyThemeColors()
{
    const QPalette pal = QApplication::palette();

    // Background: Base is white in light mode, dark gray in dark mode
    customPlot->setBackground(QBrush(pal.color(QPalette::Base)));

    // X-axis foreground (the only axis not colored by data line color)
    const QColor fgColor = pal.color(QPalette::Text);
    customPlot->xAxis->setLabelColor(fgColor);
    customPlot->xAxis->setTickLabelColor(fgColor);
    customPlot->xAxis->setBasePen(QPen(fgColor, 0));
    customPlot->xAxis->setTickPen(QPen(fgColor, 0));
    customPlot->xAxis->setSubTickPen(QPen(fgColor, 0));

    // Grid lines: text color at low alpha, adaptive to both modes
    QColor gridColor = fgColor;
    gridColor.setAlpha(40);
    QColor zeroLineColor = fgColor;
    zeroLineColor.setAlpha(60);

    customPlot->xAxis->grid()->setPen(QPen(gridColor, 0, Qt::DotLine));
    customPlot->xAxis->grid()->setZeroLinePen(QPen(zeroLineColor, 0, Qt::SolidLine));

    // Apply grid colors to all existing y-axes
    for (auto it = m_plotValueAxes.constBegin(); it != m_plotValueAxes.constEnd(); ++it) {
        QCPAxis *yAxis = it.value();
        yAxis->grid()->setPen(QPen(gridColor, 0, Qt::DotLine));
        yAxis->grid()->setZeroLinePen(QPen(zeroLineColor, 0, Qt::SolidLine));
    }

    customPlot->replot(QCustomPlot::rpQueuedReplot);
}

void PlotWidget::onPreferenceChanged(const QString &key, const QVariant &value)
{
    // Global plot settings
    if (key == PreferenceKeys::PlotsLineThickness) {
        m_lineThickness = value.toDouble();
        updatePlot(); // Rebuild graphs with new line thickness
        return;
    }

    if (key == PreferenceKeys::PlotsTextSize) {
        m_textSize = value.toInt();
        applyPlotPreferences();
        customPlot->replot();
        return;
    }

    if (key == PreferenceKeys::PlotsYAxisPadding) {
        m_yAxisPadding = value.toDouble();
        onXAxisRangeChanged(customPlot->xAxis->range()); // Recalculate Y ranges
        return;
    }

    // Per-plot color changes
    if (key.startsWith("plots/") && key.endsWith("/color")) {
        updatePlot(); // Rebuild to apply new color
        return;
    }

    // Per-plot Y-axis mode changes
    if (key.startsWith("plots/") && (key.endsWith("/yAxisMode") ||
                                      key.endsWith("/yAxisMin") ||
                                      key.endsWith("/yAxisMax"))) {
        onXAxisRangeChanged(customPlot->xAxis->range()); // Recalculate Y ranges
        return;
    }
}

void PlotWidget::onXAxisRangeChanged(const QCPRange &newRange)
{
    if (m_updatingYAxis) return;

    m_updatingYAxis = true;

    // iterate through all y-axes and adjust their ranges
    for (auto it = m_plotValueAxes.constBegin(); it != m_plotValueAxes.constEnd(); ++it) {
        QCPAxis* yAxis = it.value();
        double yMin = std::numeric_limits<double>::max();
        double yMax = std::numeric_limits<double>::lowest();

        for (int i = 0; i < customPlot->graphCount(); ++i) {
            QCPGraph* graph = customPlot->graph(i);
            if (graph->valueAxis() != yAxis) continue;

            auto itLower = graph->data()->findBegin(newRange.lower, false);
            auto itUpper = graph->data()->findEnd(newRange.upper, false);
            for (auto it = itLower; it != itUpper; ++it) {
                double y = it->value;
                yMin = std::min(yMin, y);
                yMax = std::max(yMax, y);
            }

            double yLower = interpolateY(graph, newRange.lower);
            double yUpper = interpolateY(graph, newRange.upper);
            if (!std::isnan(yLower)) {
                yMin = std::min(yMin, yLower);
                yMax = std::max(yMax, yLower);
            }
            if (!std::isnan(yUpper)) {
                yMin = std::min(yMin, yUpper);
                yMax = std::max(yMax, yUpper);
            }
        }

        if (yMin < yMax) {
            // Extract sensorID/measurementID from axis key
            QString axisKey = it.key(); // Format: "sensorID/measurementID"
            QStringList parts = axisKey.split('/');

            bool useManualRange = false;
            double manualMin = 0.0;
            double manualMax = 100.0;

            if (parts.size() == 2) {
                QString sensorID = parts[0];
                QString measurementID = parts[1];

                // Check Y-axis mode preference
                QString modeKey = PreferenceKeys::plotYAxisModeKey(sensorID, measurementID);
                QString mode = PreferencesManager::instance().getValue(modeKey).toString();

                if (mode.toLower() == "manual") {
                    useManualRange = true;
                    QString minKey = PreferenceKeys::plotYAxisMinKey(sensorID, measurementID);
                    QString maxKey = PreferenceKeys::plotYAxisMaxKey(sensorID, measurementID);
                    manualMin = PreferencesManager::instance().getValue(minKey).toDouble();
                    manualMax = PreferencesManager::instance().getValue(maxKey).toDouble();
                }
            }

            if (useManualRange && manualMax > manualMin) {
                yAxis->setRange(manualMin, manualMax);
            } else {
                // Auto mode: use data range with padding
                double padding = (yMax - yMin) * m_yAxisPadding;
                padding = (padding == 0) ? 1.0 : padding;
                yAxis->setRange(yMin - padding, yMax + padding);
            }
        }
    }

    customPlot->replot();

    // Position markers and crosshairs AFTER replot so coord/pixel conversion uses
    // correct axis rect geometry. Y-axis range changes (above) affect label widths,
    // which affect axisRect layout.
    updateReferenceMarkers(UpdateMode::Reflow);
    if (m_crosshairManager)
        m_crosshairManager->updateIfOverPlotArea();
    customPlot->replot(QCustomPlot::rpQueuedReplot);

    if (m_referenceMarkerKey.isEmpty() && m_xVariable == SessionKeys::Time)
        updateXAxisTicker();      // update format when span changes

    // Broadcast range to interested listeners (e.g., map)
    if (m_rangeModel) {
        m_rangeModel->setRange(m_xVariable, m_referenceMarkerKey, newRange.lower, newRange.upper);
    }

    m_updatingYAxis = false;
}

void PlotWidget::onXVariableChanged(const QString &newXVariable)
{
    if (m_xVariable == newXVariable)
        return;

    m_xVariable = newXVariable;
    m_xAxisLabel = m_viewSettingsModel ? m_viewSettingsModel->xAxisLabel() : m_xAxisLabel;
    customPlot->xAxis->setLabel(m_xAxisLabel);
    updateXAxisTicker();
    updatePlot();
}

void PlotWidget::onReferenceMarkerKeyChanged(const QString &oldKey, const QString &newKey)
{
    if (m_referenceMarkerKey == newKey)
        return;

    // Compute per-session viewport transformations and take the union.
    // Each session may have a different offset for the old and new reference
    // markers, so a single global delta would only be correct for one session.
    QCPRange oldRange = customPlot->xAxis->range();
    double newLower = std::numeric_limits<double>::max();
    double newUpper = std::numeric_limits<double>::lowest();
    bool hasValid = false;

    for (int si = 0; si < model->rowCount(); ++si) {
        const SessionRow &sr = model->rowAt(si);
        if (!sr.isLoaded() || !sr.visible)
            continue;

        const SessionData &session = sr.session.value();
        auto oldOffset = markerOffsetSeconds(session, oldKey, m_xVariable);
        auto newOffset = markerOffsetSeconds(session, newKey, m_xVariable);
        if (!oldOffset.has_value() || !newOffset.has_value())
            continue;

        double delta = newOffset.value() - oldOffset.value();
        newLower = std::min(newLower, oldRange.lower - delta);
        newUpper = std::max(newUpper, oldRange.upper - delta);
        hasValid = true;
    }

    QCPRange newRange = hasValid ? QCPRange(newLower, newUpper) : oldRange;

    // Update state
    m_referenceMarkerKey = newKey;
    m_xAxisLabel = m_viewSettingsModel ? m_viewSettingsModel->xAxisLabel() : m_xAxisLabel;

    customPlot->xAxis->setLabel(m_xAxisLabel);

    {
        QSignalBlocker blocker(customPlot->xAxis);
        customPlot->xAxis->setRange(newRange);
    }

    updateXAxisTicker();
    updatePlot();
    customPlot->replot();
    updateXAxisTicker();

    // Broadcast range with new axis configuration
    if (m_rangeModel) {
        m_rangeModel->setRange(m_xVariable, m_referenceMarkerKey, newRange.lower, newRange.upper);
    }

    // Re-apply external cursors under the new axis mode
    scheduleMomentUpdate();
}

void PlotWidget::onHoveredSessionChanged(const QString &sessionId)
{
    // Update graph appearance based on the hovered session.
    // Iterate in plot-model draw order (not QMap pointer order) so that
    // setLayer() calls re-establish the correct visual stacking.
    for (QCPGraph *graph : m_graphDrawOrder) {
        auto it = m_graphInfoMap.constFind(graph);
        if (it == m_graphInfoMap.constEnd())
            continue;
        const GraphInfo &info = it.value();

        graph->setLayer(determineGraphLayer(info, sessionId));
        graph->setPen(determineGraphPen(info, sessionId));
    }

    customPlot->replot();
}

void PlotWidget::applyPinchZoom(double factor, const QPointF &centerPos)
{
    if (factor <= 0.0 || qFuzzyCompare(factor, 1.0))
        return;
    factor = qBound(0.1, factor, 10.0);

    const double centerCoord = customPlot->xAxis->pixelToCoord(centerPos.x());
    customPlot->xAxis->scaleRange(factor, centerCoord);
    customPlot->replot(QCustomPlot::rpQueuedReplot);
}

// Protected Methods
void PlotWidget::changeEvent(QEvent *event)
{
    // Qt delivers this to every widget when the application palette changes,
    // for example when the system switches between light and dark.
    if (event->type() == QEvent::ApplicationPaletteChange)
        applyThemeColors();
    QWidget::changeEvent(event);
}

bool PlotWidget::eventFilter(QObject *obj, QEvent *event)
{
    // Step 5: Keep reference markers aligned when plot geometry changes (resize/layout, margins).
    // Spec trigger: QEvent::Resize on customPlot -> updateReferenceMarkers(Reflow)
    if (obj == customPlot && event->type() == QEvent::Resize) {
        // Defer to the next event loop turn so QCustomPlot can apply its new geometry first.
        QTimer::singleShot(0, this, [this]() {
            updateReferenceMarkers(UpdateMode::Reflow);
            customPlot->replot(QCustomPlot::rpQueuedReplot);
        });
    }

    if (obj == customPlot && m_currentTool) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            auto me = static_cast<QMouseEvent*>(event);

            // Handle active bubble drag before anything else
            if (m_dragBubble) {
                return handleBubbleDrag(me);
            }

            const bool wasInPlotArea = m_mouseInPlotArea;

            const bool inPlotArea =
                customPlot->axisRect()->rect().contains(me->pos());
            m_mouseInPlotArea = inPlotArea;

            // forward to crosshairManager if desired:
            if (m_crosshairManager) {
                m_crosshairManager->handleMouseMove(me->pos());
            }

            // Write mouse position to MomentModel (UTC seconds)
            {
                const QSet<QString> tracedSessions =
                    m_crosshairManager ? m_crosshairManager->getTracedSessionIds()
                                       : QSet<QString>{};
                writeMouseMoment(me->pos(), tracedSessions);
            }

            // If the mouse just left the plot area, force a re-evaluation of moments
            // so external (e.g., video) cursors re-appear immediately.
            if (wasInPlotArea && !inPlotArea) {
                scheduleMomentUpdate();
            }

            // also forward to the current tool
            return m_currentTool->mouseMoveEvent(me);
        }
        case QEvent::MouseButtonPress: {
            auto me = static_cast<QMouseEvent*>(event);
            QCPItemText *hitBubble = hitTestMarkerBubble(me->pos());
            if (hitBubble) {
                if (me->button() == Qt::RightButton) {
                    showBubbleContextMenu(hitBubble, me->globalPosition().toPoint());
                    return true;  // Always consume right-clicks on bubbles
                }
                return handleBubblePress(hitBubble, me);
            }
            return m_currentTool->mousePressEvent(me);
        }
        case QEvent::MouseButtonRelease: {
            auto me = static_cast<QMouseEvent*>(event);
            if (m_dragBubble) {
                return handleBubbleRelease(me);
            }
            return m_currentTool->mouseReleaseEvent(me);
        }
        case QEvent::MouseButtonDblClick: {
            auto me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                QCPItemText *hitBubble = hitTestMarkerBubble(me->pos());
                if (hitBubble) {
                    handleBubbleDoubleClick(hitBubble);
                    return true;
                }
            }
            break;
        }
        case QEvent::Leave:
            m_mouseInPlotArea = false;

            if (m_crosshairManager) {
                m_crosshairManager->handleMouseLeave();
            }

            // Mark mouse moment inactive and clear targets on leave
            if (m_momentModel) {
                m_momentModel->setMomentPosition(
                    QStringLiteral("mouse"), 0.0, {}, false);
            }

            // Force re-evaluation so external (e.g., video) cursors come back immediately.
            scheduleMomentUpdate();

            m_currentTool->leaveEvent(event);
            return false;
        case QEvent::NativeGesture: {
            auto *nge = static_cast<QNativeGestureEvent *>(event);
            if (nge->gestureType() == Qt::ZoomNativeGesture) {
                const double factor = 1.0 / (1.0 + nge->value());
                applyPinchZoom(factor, nge->position());
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// Initialization

void PlotWidget::setupPlot()
{
    // configure basic plot settings
    customPlot->xAxis->setLabel(m_xAxisLabel);
    customPlot->yAxis->setVisible(false);

    // enable interactions for range dragging and zooming
    customPlot->setInteraction(QCP::iRangeDrag, true);
    customPlot->setInteraction(QCP::iRangeZoom, true);

    // restrict range interactions to horizontal only
    customPlot->axisRect()->setRangeDrag(Qt::Horizontal);
    customPlot->axisRect()->setRangeZoom(Qt::Horizontal);

    // Reserve top margin for the reference marker lane
    customPlot->axisRect()->setMinimumMargins(QMargins(0, kLaneHeightPx, 0, 0));

    // create a dedicated layer for highlighted graphs
    customPlot->addLayer("highlighted", customPlot->layer("main"), QCustomPlot::limAbove);

    // create dedicated layers for reference marker items (drawn above all plot content)
    // arrows below bubbles so that pointer lines never cross over bubble labels
    QCPLayer *aboveLayer = customPlot->layer("overlay");
    if (!aboveLayer)
        aboveLayer = customPlot->layer("highlighted");
    customPlot->addLayer("markerArrows", aboveLayer, QCustomPlot::limAbove);
    customPlot->addLayer("markerBubbles", customPlot->layer("markerArrows"), QCustomPlot::limAbove);
}

void PlotWidget::updateXAxisTicker()
{
    if (m_referenceMarkerKey.isEmpty() && m_xVariable == SessionKeys::Time) {
        // Absolute UTC time → human-readable
        auto dtTicker = QSharedPointer<QCPAxisTickerDateTime>::create();
        dtTicker->setDateTimeSpec(Qt::UTC);

        // Choose format based on current visible span (seconds)
        double span = customPlot->xAxis->range().size();
        if (span < 30)                   // < 30 s
            dtTicker->setDateTimeFormat("HH:mm:ss.z");
        else if (span < 3600)            // < 1 h
            dtTicker->setDateTimeFormat("HH:mm:ss");
        else if (span < 86400)           // < 1 day
            dtTicker->setDateTimeFormat("HH:mm");
        else                             // ≥ 1 day
            dtTicker->setDateTimeFormat("yyyy‑MM‑dd\nHH:mm");

        customPlot->xAxis->setTicker(dtTicker);
    } else {
        // Relative seconds → default numeric ticker
        customPlot->xAxis->setTicker(QSharedPointer<QCPAxisTicker>::create());
    }
}

// Utility Methods
double PlotWidget::interpolateY(const QCPGraph* graph, double x)
{
    // find the closest data points for interpolation
    auto itLower = graph->data()->findBegin(x, false);
    if (itLower == graph->data()->constBegin() || itLower == graph->data()->constEnd()) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    auto itPrev = itLower;
    --itPrev;

    double x1 = itPrev->key;
    double y1 = itPrev->value;
    double x2 = itLower->key;
    double y2 = itLower->value;

    if (x2 == x1) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    return y1 + (y2 - y1) * (x - x1) / (x2 - x1);
}

QPen PlotWidget::determineGraphPen(const GraphInfo &info, const QString &hoveredSessionId) const
{
    // Always use the default pen
    return info.defaultPen;
}

QString PlotWidget::determineGraphLayer(const GraphInfo &info, const QString &hoveredSessionId) const
{
    // Always use the default layer
    return "highlighted";
}

// View management
bool PlotWidget::withReferenceSession(const std::function<void(const SessionData &)> &fn) const
{
    // The session is handed to fn under a row stability guard and is never
    // returned, so it cannot be held past the read.
    const SessionModel::RowStabilityGuard guard(*model);

    // 1. hovered?
    QString hovered = model->hoveredSessionId();
    if (!hovered.isEmpty()) {
        for (int i = 0; i < model->rowCount(); ++i) {
            const SessionRow &row = model->rowAt(i);
            if (row.isLoaded() && row.sessionId == hovered) {
                fn(row.session.value());
                return true;
            }
        }
    }
    // 2. first visible
    for (int i = 0; i < model->rowCount(); ++i) {
        const SessionRow &row = model->rowAt(i);
        if (row.isLoaded() && row.visible) {
            fn(row.session.value());
            return true;
        }
    }

    return false;             // should not happen
}

std::optional<double> PlotWidget::referenceOffsetForSession(const SessionData &session) const
{
    return markerOffsetSeconds(session, m_referenceMarkerKey, m_xVariable);
}

void PlotWidget::updateReferenceMarkers(UpdateMode mode)
{
    auto clearLaneItems = [this](QVector<QPointer<QCPAbstractItem>> &laneItems) {
        for (auto &item : laneItems) {
            if (!item)
                continue;

            // Keep marker-bubble metadata in sync with item lifetime.
            if (QCPItemText *bubble = qobject_cast<QCPItemText *>(item.data())) {
                m_markerBubbleMeta.remove(bubble);
            }

            customPlot->removeItem(item);
        }
        laneItems.clear();
    };

    auto clearAllItems = [this, &clearLaneItems]() {
        for (auto &laneItems : m_markerItemsByLane) {
            clearLaneItems(laneItems);
        }
        m_markerItemsByLane.clear();
        m_markerBubbleMeta.clear();
    };

    if (mode == UpdateMode::Rebuild) {
        clearAllItems();
    }

    // A BubbleMoment unifies the data needed for bubble lane rendering.
    // Built from MomentModel when available, MarkerModel as fallback.
    struct BubbleMoment {
        QString attributeKey;
        QString shortLabel;
        QColor color;
        bool editable = false;
        QVector<MarkerMeasurement> measurements;
    };

    QVector<BubbleMoment> bubbleMoments;

    if (m_momentModel) {
        // Build measurement lookup from MarkerRegistry (needed for annotation computation)
        QHash<QString, QVector<MarkerMeasurement>> measurementsByKey;
        const QVector<MarkerDefinition> allDefs = MarkerRegistry::instance()->allMarkers();
        for (const auto &def : allDefs) {
            measurementsByKey.insert(def.attributeKey, def.measurements);
        }

        const QVector<MomentModel::Moment> moments = m_momentModel->enabledMoments();
        for (const auto &moment : moments) {
            if (moment.traits.plotBubble != PlotBubble::Bubble)
                continue;

            BubbleMoment bm;
            bm.attributeKey = moment.traits.attributeKey;
            bm.shortLabel = moment.traits.shortLabel;
            bm.color = moment.traits.color;
            bm.editable = (moment.traits.interaction == Interaction::Drag);
            bm.measurements = measurementsByKey.value(moment.traits.attributeKey);
            bubbleMoments.append(bm);
        }
    } else {
        // Fallback: read from MarkerModel
        const QVector<MarkerDefinition> enabledDefs =
            markerModel ? markerModel->enabledMarkers() : QVector<MarkerDefinition>{};
        for (const auto &def : enabledDefs) {
            BubbleMoment bm;
            bm.attributeKey = def.attributeKey;
            bm.shortLabel = def.shortLabel;
            bm.color = def.color;
            bm.editable = def.editable;
            bm.measurements = def.measurements;
            bubbleMoments.append(bm);
        }
    }

    // One lane per enabled bubble moment; always keep a small minimum so
    // the topmost y-axis tick label is not clipped when no lanes are present.
    const int topMargin = qMax(kLaneHeightPx * static_cast<int>(bubbleMoments.size()),
                               kMinTopMarginPx);
    customPlot->axisRect()->setMinimumMargins(QMargins(0, topMargin, 0, 0));

    if (bubbleMoments.isEmpty()) {
        if (!m_markerItemsByLane.isEmpty() || !m_markerBubbleMeta.isEmpty())
            clearAllItems();
        return;
    }

    // Ensure lane storage matches the enabled bubble moment count
    if (m_markerItemsByLane.size() != bubbleMoments.size()) {
        clearAllItems();
        m_markerItemsByLane.resize(bubbleMoments.size());
    } else if (m_markerItemsByLane.isEmpty()) {
        m_markerItemsByLane.resize(bubbleMoments.size());
    }

    // Only draw markers that are currently within the visible x range
    const QCPRange xRange = customPlot->xAxis->range();
    const QRect axisRectPx = customPlot->axisRect()->rect();

    auto tryAttributeUtcSeconds =
        [](const SessionData &s, const QString &attributeKey, double *outUtcSeconds) -> bool {
            if (!outUtcSeconds)
                return false;

            QVariant v = s.getAttribute(attributeKey);
            if (!v.canConvert<double>())
                return false;

            *outUtcSeconds = v.toDouble();
            return true;
        };

    // Compute annotation string for a single-session marker bubble
    auto computeAnnotation = [this](const BubbleMoment &bm,
                                    const QString &sessionId) -> QString
    {
        if (bm.measurements.isEmpty())
            return QString();

        const auto &primaryMk = bm.measurements.first();
        QString valueKey = SessionData::interpolationKey(
            bm.attributeKey, primaryMk.sensor,
            primaryMk.timeVector, primaryMk.dataVector);

        int row = model->getSessionRow(sessionId);
        if (row < 0)
            return QString();

        SessionData &session = model->sessionRef(row);
        QVariant val = session.getAttribute(valueKey);
        if (!val.isValid() || !val.canConvert<double>())
            return QString();

        double siValue = val.toDouble();

        // Look up measurement type from PlotRegistry
        QString measurementType;
        const QVector<PlotValue> &allPlots = PlotRegistry::instance().allPlots();
        for (const PlotValue &pv : allPlots) {
            if (pv.sensorID == primaryMk.sensor && pv.measurementID == primaryMk.dataVector) {
                measurementType = pv.measurementType;
                break;
            }
        }

        return UnitConverter::instance().format(siValue, measurementType);
    };

    const int clusterThresholdPx = 20;

    // Style (must match existing EXIT marker visuals)
    const int pointerHeightPx = 8;
    const int pointerBaseWidthPx = 10;
    const int bubbleGapPx = 0;

    for (int laneIndex = 0; laneIndex < bubbleMoments.size(); ++laneIndex) {
        const BubbleMoment &def = bubbleMoments[laneIndex];

        struct DrawableInstance {
            double xPixel = 0.0;
            double markerUtcSeconds = 0.0;
            QString sessionId;
        };

        QVector<DrawableInstance> drawable;
        drawable.reserve(model->rowCount());

        for (int si = 0; si < model->rowCount(); ++si) {
            const SessionRow &sr = model->rowAt(si);
            if (!sr.isLoaded() || !sr.visible)
                continue;

            const SessionData &s = sr.session.value();

            double markerUtcSeconds = 0.0;
            if (!tryAttributeUtcSeconds(s, def.attributeKey, &markerUtcSeconds))
                continue;

            auto optX = plotAxisXFromUtc(markerUtcSeconds, m_xVariable, m_referenceMarkerKey, s);
            if (!optX.has_value())
                continue;

            double xCoord = *optX;

            if (xCoord < xRange.lower || xCoord > xRange.upper)
                continue;

            DrawableInstance di;
            di.xPixel = customPlot->xAxis->coordToPixel(xCoord);
            di.markerUtcSeconds = markerUtcSeconds;
            di.sessionId = sr.sessionId;
            drawable.append(di);
        }

        QVector<QPointer<QCPAbstractItem>> &laneItems = m_markerItemsByLane[laneIndex];

        if (drawable.isEmpty()) {
            if (!laneItems.isEmpty())
                clearLaneItems(laneItems);
            continue;
        }

        std::sort(drawable.begin(), drawable.end(),
                  [](const DrawableInstance &a, const DrawableInstance &b) { return a.xPixel < b.xPixel; });

        struct Cluster {
            double anchorXPixel = 0.0;
            QString label;

            double sumXPixel = 0.0;
            double lastXPixel = 0.0;
            int count = 0;

            double markerUtcSeconds = 0.0; // Only valid when count == 1
            QString sessionId;             // Only valid when count == 1
        };

        QVector<Cluster> clusters;
        clusters.reserve(drawable.size());

        Cluster current;
        current.sumXPixel = drawable.first().xPixel;
        current.lastXPixel = drawable.first().xPixel;
        current.count = 1;
        current.markerUtcSeconds = drawable.first().markerUtcSeconds;
        current.sessionId = drawable.first().sessionId;

        for (int i = 1; i < drawable.size(); ++i) {
            const double xPix = drawable[i].xPixel;
            const double utcSeconds = drawable[i].markerUtcSeconds;

            if (qAbs(xPix - current.lastXPixel) <= clusterThresholdPx) {
                current.sumXPixel += xPix;
                current.lastXPixel = xPix;
                current.count += 1;

                // Only valid for single-marker clusters.
                if (current.count != 1) {
                    current.markerUtcSeconds = 0.0;
                    current.sessionId.clear();
                }

                continue;
            }

            // finalize cluster
            current.anchorXPixel = current.sumXPixel / current.count;
            if (current.count == 1) {
                QString ann = computeAnnotation(def, current.sessionId);
                current.label = ann.isEmpty()
                    ? def.shortLabel
                    : QStringLiteral("%1: %2").arg(def.shortLabel, ann);
            } else {
                current.label = QStringLiteral("%1 \u00D7%2").arg(def.shortLabel).arg(current.count);
            }
            clusters.append(current);

            // start new cluster
            current = Cluster{};
            current.sumXPixel = xPix;
            current.lastXPixel = xPix;
            current.count = 1;
            current.markerUtcSeconds = utcSeconds;
            current.sessionId = drawable[i].sessionId;
        }

        // finalize last cluster
        current.anchorXPixel = current.sumXPixel / current.count;
        if (current.count == 1) {
            QString ann = computeAnnotation(def, current.sessionId);
            current.label = ann.isEmpty()
                ? def.shortLabel
                : QStringLiteral("%1: %2").arg(def.shortLabel, ann);
        } else {
            current.label = QStringLiteral("%1 \u00D7%2").arg(def.shortLabel).arg(current.count);
        }
        clusters.append(current);

        if (clusters.isEmpty()) {
            if (!laneItems.isEmpty())
                clearLaneItems(laneItems);
            continue;
        }

        // Rebuild lane items if cluster count changed (or items are stale)
        const int requiredItemCount = clusters.size() * 2;
        bool needsRebuild = (laneItems.size() != requiredItemCount);
        if (!needsRebuild) {
            for (auto &item : laneItems) {
                if (!item) {
                    needsRebuild = true;
                    break;
                }
            }
        }
        if (needsRebuild) {
            clearLaneItems(laneItems);
        }

        // Geometry per lane (lane 0 is closest to plot)
        const int laneBottomY = axisRectPx.top() - laneIndex * kLaneHeightPx;
        const int bubbleBottomY = laneBottomY - pointerHeightPx - bubbleGapPx;

        // Lazily create marker items (one pointer + one bubble per cluster)
        if (laneItems.isEmpty()) {
            const QColor markerColor = def.color;

            for (int i = 0; i < clusters.size(); ++i) {
                // Pointer triangle (implemented via a flat arrow head)
                QCPItemLine *pointer = new QCPItemLine(customPlot);
                pointer->setLayer("markerArrows");
                pointer->setClipToAxisRect(false);
                pointer->setSelectable(false);

                QPen pointerPen(markerColor);
                pointerPen.setWidthF(0);
                pointer->setPen(pointerPen);
                pointer->setHead(QCPLineEnding(QCPLineEnding::esFlatArrow, pointerBaseWidthPx, pointerHeightPx));
                pointer->setTail(QCPLineEnding(QCPLineEnding::esNone));

                pointer->start->setType(QCPItemPosition::ptAbsolute);
                pointer->end->setType(QCPItemPosition::ptAbsolute);

                // Label bubble
                QCPItemText *bubble = new QCPItemText(customPlot);
                bubble->setLayer("markerBubbles");
                bubble->setClipToAxisRect(false);
                bubble->setSelectable(false);

                bubble->position->setType(QCPItemPosition::ptAbsolute);
                bubble->setPositionAlignment(Qt::AlignHCenter | Qt::AlignBottom);
                bubble->setTextAlignment(Qt::AlignCenter);

                bubble->setText(def.shortLabel);
                bubble->setPadding(QMargins(6, 2, 6, 2));
                bubble->setBrush(QBrush(markerColor));
                bubble->setPen(QPen(markerColor));
                bubble->setColor(Qt::white);

                QFont f = bubble->font();
                f.setBold(true);
                bubble->setFont(f);

                laneItems << pointer << bubble;
            }
        }

        // Update item positions and labels (for panning/zooming and re-clustering)
        for (int i = 0; i < clusters.size(); ++i) {
            const double xPixel = clusters[i].anchorXPixel;

            QCPItemLine *pointer = qobject_cast<QCPItemLine *>(laneItems[2 * i].data());
            QCPItemText *bubble = qobject_cast<QCPItemText *>(laneItems[2 * i + 1].data());

            if (pointer) {
                pointer->start->setCoords(xPixel, laneBottomY - pointerHeightPx);
                pointer->end->setCoords(xPixel, axisRectPx.top());
            }
            if (bubble) {
                bubble->setText(clusters[i].label);
                bubble->position->setCoords(xPixel, bubbleBottomY);

                MarkerBubbleMeta meta;
                meta.count        = clusters[i].count;
                meta.utcSeconds   = (clusters[i].count == 1) ? clusters[i].markerUtcSeconds : 0.0;
                meta.attributeKey = def.attributeKey;
                meta.sessionId    = (clusters[i].count == 1) ? clusters[i].sessionId : QString();
                meta.editable     = def.editable;
                meta.measurements = def.measurements;
                m_markerBubbleMeta.insert(bubble, meta);
            }
        }
    }
}

QCPRange PlotWidget::keyRangeOf(const SessionData& s,
                                const QString& sensor,
                                const QString& meas) const
{
    QVector<double> v =
        const_cast<SessionData&>(s).getMeasurement(sensor, meas);
    if (v.isEmpty()) return QCPRange(0,0);

    auto [minIt,maxIt] = std::minmax_element(v.begin(), v.end());
    return QCPRange(*minIt, *maxIt);
}

QCPItemText* PlotWidget::hitTestMarkerBubble(const QPoint &pos) const
{
    for (auto it = m_markerBubbleMeta.constBegin(); it != m_markerBubbleMeta.constEnd(); ++it) {
        QCPItemText *bubble = it.key();
        if (!bubble)
            continue;

        // Compute the bubble's visual bounding rect in pixel coordinates,
        // replicating the logic from QCPItemText::selectTest/draw.
        QPointF anchor = bubble->position->pixelPosition();
        QFontMetrics fm(bubble->font());
        QRect textRect = fm.boundingRect(0, 0, 0, 0,
            Qt::TextDontClip | bubble->textAlignment(), bubble->text());
        QMargins pad = bubble->padding();
        QRect boxRect = textRect.adjusted(-pad.left(), -pad.top(), pad.right(), pad.bottom());

        // Apply position alignment (matches QCPItemText::getTextDrawPoint)
        QPointF topLeft = anchor;
        Qt::Alignment align = bubble->positionAlignment();
        if (align.testFlag(Qt::AlignHCenter))
            topLeft.rx() -= boxRect.width() / 2.0;
        else if (align.testFlag(Qt::AlignRight))
            topLeft.rx() -= boxRect.width();
        if (align.testFlag(Qt::AlignVCenter))
            topLeft.ry() -= boxRect.height() / 2.0;
        else if (align.testFlag(Qt::AlignBottom))
            topLeft.ry() -= boxRect.height();
        boxRect.moveTopLeft(topLeft.toPoint());

        if (boxRect.contains(pos))
            return bubble;
    }
    return nullptr;
}

bool PlotWidget::handleBubblePress(QCPItemText *bubble, QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return false;

    auto it = m_markerBubbleMeta.constFind(bubble);
    if (it == m_markerBubbleMeta.constEnd())
        return false;

    const MarkerBubbleMeta &meta = it.value();
    if (!meta.editable || meta.count != 1)
        return false;

    // Store drag state
    m_dragBubble = bubble;
    m_dragSessionId = meta.sessionId;
    m_dragAttributeKey = meta.attributeKey;

    // Record offset between click point and bubble anchor so the marker
    // tracks from the grab point rather than snapping its center to the cursor.
    double clickXCoord = customPlot->xAxis->pixelToCoord(event->pos().x());
    double bubbleXCoord = customPlot->xAxis->pixelToCoord(bubble->position->pixelPosition().x());
    m_dragXCoordOffset = clickXCoord - bubbleXCoord;

    customPlot->setCursor(Qt::ClosedHandCursor);
    return true;
}

bool PlotWidget::handleBubbleDrag(QMouseEvent *event)
{
    double xCoord = customPlot->xAxis->pixelToCoord(event->pos().x()) - m_dragXCoordOffset;
    double utcSec = xCoordToUtcSeconds(xCoord, m_dragSessionId);
    model->updateAttribute(m_dragSessionId, m_dragAttributeKey, utcSec);
    return true;
}

bool PlotWidget::handleBubbleRelease(QMouseEvent *event)
{
    // Perform final position update
    double xCoord = customPlot->xAxis->pixelToCoord(event->pos().x()) - m_dragXCoordOffset;
    double utcSec = xCoordToUtcSeconds(xCoord, m_dragSessionId);
    model->updateAttribute(m_dragSessionId, m_dragAttributeKey, utcSec);

    // Clear drag state
    m_dragBubble = nullptr;
    m_dragSessionId.clear();
    m_dragAttributeKey.clear();
    m_dragXCoordOffset = 0.0;

    customPlot->unsetCursor();
    return true;
}

void PlotWidget::showBubbleContextMenu(QCPItemText *bubble, const QPoint &globalPos)
{
    auto it = m_markerBubbleMeta.find(bubble);
    if (it == m_markerBubbleMeta.end())
        return;
    const MarkerBubbleMeta &meta = it.value();

    // Guard: editable, single-session, has calculation, has stored override
    if (!meta.editable)
        return;
    if (meta.count != 1)
        return;
    if (!CalculationRegistry::instance().hasCandidateFor(DependencyKey::attribute(meta.attributeKey)))
        return;

    int row = model->getSessionRow(meta.sessionId);
    if (row < 0)
        return;
    if (!model->sessionRef(row).hasAttribute(meta.attributeKey))
        return;

    // menu.exec() runs a nested event loop, during which the marker bubbles can
    // be rebuilt (clearing m_markerBubbleMeta) and sessions loaded or evicted.
    // Keep copies of what is needed afterwards; hold no reference across it.
    const QString sessionId = meta.sessionId;
    const QString attributeKey = meta.attributeKey;

    // Show menu
    QMenu menu(this);
    QAction *resetAction = menu.addAction(tr("Reset to default"));
    QAction *chosenAction = menu.exec(globalPos);
    if (chosenAction == resetAction) {
        model->removeAttribute(sessionId, attributeKey);
    }
}

void PlotWidget::handleBubbleDoubleClick(QCPItemText *bubble)
{
    auto it = m_markerBubbleMeta.constFind(bubble);
    if (it == m_markerBubbleMeta.constEnd())
        return;

    const MarkerBubbleMeta &meta = it.value();
    if (m_viewSettingsModel)
        m_viewSettingsModel->setReferenceMarkerKey(meta.attributeKey);
}

void PlotWidget::schedulePlotRebuild()
{
    m_pendingRebuildLevel = RebuildLevel::Full;
    m_rebuildTimer.start();
}

void PlotWidget::scheduleMarkerUpdate()
{
    if (m_pendingRebuildLevel == RebuildLevel::Full)
        return;
    m_markerUpdateTimer.start();
}

void PlotWidget::scheduleMomentUpdate()
{
    if (m_pendingRebuildLevel == RebuildLevel::Full)
        return;
    m_momentUpdateTimer.start();
}

// ─────────────────────────────── Moment-driven rendering

void PlotWidget::onMomentsChanged()
{
    if (!m_momentModel)
        return;

    updateCrosshairFromMoments();
    updateMarkersOnly();  // reads from MomentModel per Task 3.3
    updateMomentVLines();
}

void PlotWidget::updateCrosshairFromMoments()
{
    if (!m_momentModel || !m_crosshairManager)
        return;

    // While the mouse is inside the plot area, PlotWidget owns cursor visuals.
    if (m_mouseInPlotArea)
        return;

    const QVector<MomentModel::Moment> moments = m_momentModel->enabledMoments();

    // Choose effective moment using precedence rules:
    // 1) Mouse moment that is active with non-empty targets
    // 2) First active non-mouse moment
    // 3) None (clear crosshair)
    const MomentModel::Moment *effective = nullptr;

    for (const auto &m : moments) {
        if (m.id == QStringLiteral("mouse")) {
            if (m.active && !m.targetSessions.isEmpty()) {
                effective = &m;
                break;  // Mouse moment has highest priority
            }
            continue;
        }
        if (m.active && !effective) {
            effective = &m;
            // Don't break -- mouse moment could still appear later in the list.
            // But since mouse priority is highest, once found we stop.
        }
    }

    // If we found a non-mouse but no active mouse, effective is the non-mouse.
    // If we found an active mouse, effective is the mouse.
    // If nothing found, clear.

    if (!effective || !effective->active) {
        m_crosshairManager->clearExternalCursor();
        return;
    }

    // Convert moment UTC position to plot-axis coordinate for a given session.
    auto xPlotForMoment = [&](const MomentModel::Moment &moment,
                              const SessionData &s,
                              double *outXPlot) -> bool {
        if (!outXPlot)
            return false;

        if (moment.traits.positionSource == PositionSource::Attribute) {
            // Attribute-sourced: read position from session attribute
            QVariant v = s.getAttribute(moment.traits.attributeKey);
            if (!v.canConvert<double>())
                return false;
            double utcSec = v.toDouble();
            auto opt = plotAxisXFromUtc(utcSec, m_xVariable, m_referenceMarkerKey, s);
            if (!opt.has_value())
                return false;
            *outXPlot = *opt;
            return true;
        }

        // MouseInput or External: check per-session positions first
        double utcSec = moment.positionUtc;
        if (!moment.sessionPositions.isEmpty()) {
            const QString sid = s.getAttribute(SessionKeys::SessionId).toString();
            auto psIt = moment.sessionPositions.constFind(sid);
            if (psIt != moment.sessionPositions.constEnd())
                utcSec = psIt.value();
        }
        auto opt = plotAxisXFromUtc(utcSec, m_xVariable, m_referenceMarkerKey, s);
        if (!opt.has_value())
            return false;
        *outXPlot = *opt;
        return true;
    };

    // Sessions are looked up by id and read, into plain plot coordinates, under
    // a row stability guard; each guarded block ends before the crosshair
    // manager is called.

    // Case 1: Mouse moment with exactly one explicit target session.
    if (effective->id == QStringLiteral("mouse")) {
        if (effective->targetSessions.size() != 1) {
            m_crosshairManager->clearExternalCursor();
            return;
        }

        const QString sessionId = *effective->targetSessions.constBegin();
        double xPlot = 0.0;
        bool haveX = false;
        {
            const SessionModel::RowStabilityGuard guard(*model);
            const SessionData *sessionPtr = model->loadedSession(sessionId);
            haveX = sessionPtr && xPlotForMoment(*effective, *sessionPtr, &xPlot);
        }
        if (!haveX) {
            m_crosshairManager->clearExternalCursor();
            return;
        }

        m_crosshairManager->setExternalCursor(sessionId, xPlot);
        return;
    }

    // Case 2: Non-mouse moment: multi-session external cursor.
    QHash<QString, double> xBySession;

    {
        const SessionModel::RowStabilityGuard guard(*model);

        if (!effective->targetSessions.isEmpty()) {
            // Explicit targets
            for (const QString &sid : effective->targetSessions) {
                const SessionData *s = model->loadedSession(sid);
                if (!s || !s->isVisible())
                    continue;

                double xPlot = 0.0;
                if (!xPlotForMoment(*effective, *s, &xPlot))
                    continue;

                xBySession.insert(sid, xPlot);
            }
        } else if (effective->traits.positionSource == PositionSource::Attribute) {
            // Attribute-sourced without explicit targets: all visible sessions
            for (int si = 0; si < model->rowCount(); ++si) {
                const SessionRow &sr = model->rowAt(si);
                if (!sr.isLoaded() || !sr.visible)
                    continue;

                const QString &sid = sr.sessionId;
                if (sid.isEmpty())
                    continue;

                const SessionData &s = sr.session.value();
                double xPlot = 0.0;
                if (!xPlotForMoment(*effective, s, &xPlot))
                    continue;

                xBySession.insert(sid, xPlot);
            }
        } else {
            // Non-attribute without explicit targets: all visible sessions that overlap
            for (int si = 0; si < model->rowCount(); ++si) {
                const SessionRow &sr = model->rowAt(si);
                if (!sr.isLoaded() || !sr.visible)
                    continue;

                const QString &sid = sr.sessionId;
                if (sid.isEmpty())
                    continue;

                const SessionData &s = sr.session.value();
                double xPlot = 0.0;
                if (!xPlotForMoment(*effective, s, &xPlot))
                    continue;

                // Check if any graph for this session has data at this x position
                bool overlaps = false;
                for (auto it = m_graphInfoMap.cbegin(); it != m_graphInfoMap.cend(); ++it) {
                    if (it.value().sessionId != sid)
                        continue;
                    QCPGraph *g = it.key();
                    if (!g || !g->visible())
                        continue;
                    const double yPlot = PlotWidget::interpolateY(g, xPlot);
                    if (!std::isnan(yPlot)) {
                        overlaps = true;
                        break;
                    }
                }
                if (!overlaps)
                    continue;

                xBySession.insert(sid, xPlot);
            }
        }
    }

    if (xBySession.isEmpty()) {
        m_crosshairManager->clearExternalCursor();
        return;
    }

    m_crosshairManager->setExternalCursorMulti(xBySession, true);
}

void PlotWidget::writeMouseMoment(const QPoint &pixelPos, const QSet<QString> &tracedSessions)
{
    if (!m_momentModel)
        return;

    const bool inPlotArea = customPlot->axisRect()->rect().contains(pixelPos);

    if (inPlotArea && !tracedSessions.isEmpty()) {
        const double xCoord = customPlot->xAxis->pixelToCoord(pixelPos.x());

        // Convert plot-axis coordinate to per-session UTC seconds.
        // Each session may have a different reference marker offset,
        // so the same plot x-coordinate maps to different UTC times.
        double fallbackUtc = 0.0;
        bool anyConverted = false;
        QHash<QString, double> sessionUtcMap;

        {
            // Traced sessions have graphs, so they are normally loaded. One that
            // is not (hidden and evicted before the plot was rebuilt) is
            // skipped rather than loaded from disk inside a mouse move. They
            // are read in place under a row stability guard (no load, no LRU
            // use), which ends before the moment model is updated and emits.
            const SessionModel::RowStabilityGuard guard(*model);

            for (const QString &sid : tracedSessions) {
                const SessionData *sessPtr = model->loadedSession(sid);
                if (!sessPtr)
                    continue;
                const SessionData &sess = *sessPtr;
                const auto offset = referenceOffsetForSession(sess);
                if (!offset.has_value())
                    continue;

                const double rawX = xCoord + offset.value();

                if (m_xVariable == QLatin1String(SessionKeys::SystemTime)) {
                    auto utcOpt = Calculations::systemTimeToUtc(sess, rawX);
                    if (utcOpt.has_value()) {
                        sessionUtcMap.insert(sid, *utcOpt);
                        if (!anyConverted) {
                            fallbackUtc = *utcOpt;
                            anyConverted = true;
                        }
                    }
                } else {
                    sessionUtcMap.insert(sid, rawX);
                    if (!anyConverted) {
                        fallbackUtc = rawX;
                        anyConverted = true;
                    }
                }
            }
        }

        if (anyConverted) {
            m_momentModel->setMomentPosition(
                QStringLiteral("mouse"),
                fallbackUtc,
                sessionUtcMap,
                tracedSessions,
                true);
        } else {
            m_momentModel->setMomentPosition(
                QStringLiteral("mouse"), 0.0, {}, false);
        }
    } else {
        m_momentModel->setMomentPosition(
            QStringLiteral("mouse"), 0.0, tracedSessions, false);
    }
}

void PlotWidget::updateMomentVLines()
{
    if (!m_momentModel)
        return;

    const QVector<MomentModel::Moment> moments = m_momentModel->enabledMoments();

    // Collect moment IDs that should have lines
    QSet<QString> activeMomentIds;

    for (const auto &moment : moments) {
        // Skip MouseInput moments (handled by CrosshairManager)
        if (moment.traits.positionSource == PositionSource::MouseInput)
            continue;

        // Skip DashedLineOnDrag (deferred to Phase 5)
        if (moment.traits.plotPresentation == PlotPresentation::DashedLineOnDrag)
            continue;

        // Only VerticalLine and DashedLine are handled
        if (moment.traits.plotPresentation != PlotPresentation::VerticalLine &&
            moment.traits.plotPresentation != PlotPresentation::DashedLine)
            continue;

        // Only draw if the moment is active
        if (!moment.active)
            continue;

        activeMomentIds.insert(moment.id);

        // Compute the x-axis position. For attribute-sourced moments, we need
        // to pick a representative session (use the first visible session that
        // has the attribute). For MouseInput/External, use positionUtc directly.
        double xPlot = 0.0;
        bool havePosition = false;

        if (moment.traits.positionSource == PositionSource::Attribute) {
            // Use the first visible session that has the attribute
            for (int si = 0; si < model->rowCount(); ++si) {
                const SessionRow &sr = model->rowAt(si);
                if (!sr.isLoaded() || !sr.visible)
                    continue;

                const SessionData &s = sr.session.value();
                QVariant v = s.getAttribute(moment.traits.attributeKey);
                if (!v.canConvert<double>())
                    continue;
                double utcSec = v.toDouble();
                auto opt = plotAxisXFromUtc(utcSec, m_xVariable, m_referenceMarkerKey, s);
                if (!opt.has_value())
                    continue;
                xPlot = *opt;
                havePosition = true;
                break;
            }
        } else {
            // External source
            withReferenceSession([&](const SessionData &refSession) {
                auto opt = plotAxisXFromUtc(moment.positionUtc, m_xVariable,
                                            m_referenceMarkerKey, refSession);
                if (opt.has_value()) {
                    xPlot = *opt;
                    havePosition = true;
                }
            });
        }

        if (!havePosition)
            continue;

        // Get or create the QCPItemLine
        QCPItemLine *line = m_momentVLines.value(moment.id, nullptr);
        if (!line) {
            line = new QCPItemLine(customPlot);
            line->setClipToAxisRect(true);
            line->setSelectable(false);
            m_momentVLines.insert(moment.id, line);
        }

        // Configure line style based on presentation trait
        QPen pen(moment.traits.color.isValid() ? moment.traits.color : QColor(128, 128, 128));
        pen.setWidthF(1.0);
        if (moment.traits.plotPresentation == PlotPresentation::DashedLine) {
            pen.setStyle(Qt::DashLine);
        } else {
            pen.setStyle(Qt::SolidLine);
        }
        line->setPen(pen);

        // Position the line vertically across the plot area
        line->start->setTypeX(QCPItemPosition::ptPlotCoords);
        line->start->setTypeY(QCPItemPosition::ptAxisRectRatio);
        line->start->setCoords(xPlot, 0.0);

        line->end->setTypeX(QCPItemPosition::ptPlotCoords);
        line->end->setTypeY(QCPItemPosition::ptAxisRectRatio);
        line->end->setCoords(xPlot, 1.0);

        line->setVisible(true);
    }

    // Remove lines for moments no longer present or no longer active
    QMutableHashIterator<QString, QCPItemLine*> it(m_momentVLines);
    while (it.hasNext()) {
        it.next();
        if (!activeMomentIds.contains(it.key())) {
            if (it.value()) {
                customPlot->removeItem(it.value());
            }
            it.remove();
        }
    }

    customPlot->replot(QCustomPlot::rpQueuedReplot);
}

void PlotWidget::onDependencyChanged(const QString &sessionId, const DependencyKey &key)
{
    if (key.type == DependencyKey::Type::Attribute) {
        // Check if the changed attribute is the current reference marker key
        if (!m_referenceMarkerKey.isEmpty() && key.attributeKey == m_referenceMarkerKey) {
            // Viewport shift for the reference session.
            // Only apply the delta when the changed session is the same one
            // whose offset we cached, to avoid stale/mismatched deltas.
            // The new offset is read as a plain value; the range model is
            // updated (and emits) only after the session read has ended.
            std::optional<double> newRefOffset;
            withReferenceSession([&](const SessionData &ref) {
                QString refId = ref.getAttribute(SessionKeys::SessionId).toString();
                if (refId == sessionId && sessionId == m_lastRefSessionId)
                    newRefOffset = referenceOffsetForSession(ref).value_or(0.0);
            });
            if (newRefOffset.has_value()) {
                double newOffset = newRefOffset.value();
                double delta = newOffset - m_lastRefOffset;

                if (delta != 0.0) {
                    QCPRange oldRange = customPlot->xAxis->range();
                    QCPRange newRange(oldRange.lower - delta, oldRange.upper - delta);

                    {
                        QSignalBlocker blocker(customPlot->xAxis);
                        customPlot->xAxis->setRange(newRange);
                    }

                    if (m_rangeModel) {
                        m_rangeModel->setRange(m_xVariable, m_referenceMarkerKey, newRange.lower, newRange.upper);
                    }

                    m_lastRefOffset = newOffset;
                }
            }
            // Reference marker change always requires a full rebuild
            schedulePlotRebuild();
            return;
        }
        // Check if the changed attribute is a marker attribute (for marker display updates)
        const QVector<MarkerDefinition> enabledDefs =
            markerModel ? markerModel->enabledMarkers() : QVector<MarkerDefinition>{};
        for (const MarkerDefinition &def : enabledDefs) {
            if (key.attributeKey == def.attributeKey) {
                scheduleMarkerUpdate();
                return;
            }
        }
        // Other attributes: ignore
        return;
    }

    if (key.type == DependencyKey::Type::Measurement) {
        const QString sensor = key.measurementKey.first;
        const QString measurement = key.measurementKey.second;

        // Check if this measurement is currently displayed
        for (auto it = m_graphInfoMap.constBegin(); it != m_graphInfoMap.constEnd(); ++it) {
            const GraphInfo &info = it.value();
            if (info.sensorId == sensor && info.measurementId == measurement) {
                schedulePlotRebuild();
                return;
            }
        }

        // Check if the invalidated measurement matches the x-axis variable
        if (measurement == m_xVariable) {
            schedulePlotRebuild();
            return;
        }

        // Measurement not displayed: ignore
        return;
    }
}

} // namespace FlySight
