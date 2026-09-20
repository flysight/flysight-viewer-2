#include "LegendPresenter.h"

#include "sessionmodel.h"
#include "plotmodel.h"
#include "momentmodel.h"
#include "plotviewsettingsmodel.h"
#include "measuremodel.h"
#include "LegendWidget.h"
#include "sessiondata.h"
#include "units/unitconverter.h"
#include "plotutils.h"
#include "plotregistry.h"
#include "calculations/timecalculations.h"

#include <QHash>
#include <QDateTime>
#include <QTimeZone>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

namespace FlySight {
namespace {

bool sessionOverlapsAtX(const SessionData &session,
                        const QVector<PlotValue> &enabledPlots,
                        const QString &xVariable,
                        const QString &referenceMarkerKey,
                        double plotAxisX)
{
    // Compute offset: plot-to-raw conversion is rawX = plotAxisX + offset
    const double offset = markerOffsetSeconds(session, referenceMarkerKey, xVariable).value_or(0.0);
    const double rawX = plotAxisX + offset;

    for (const PlotValue &pv : enabledPlots) {
        const QVector<double> xData = session.getMeasurement(pv.sensorID, xVariable);
        const QVector<double> yData = session.getMeasurement(pv.sensorID, pv.measurementID);

        if (xData.isEmpty() || yData.isEmpty() || xData.size() != yData.size()) {
            continue;
        }

        auto minmax = std::minmax_element(xData.cbegin(), xData.cend());
        if (minmax.first == xData.cend() || minmax.second == xData.cend()) {
            continue;
        }

        if (rawX >= *minmax.first && rawX <= *minmax.second) {
            return true;
        }
    }

    return false;
}

// What the legend shows for a moment, as plain values.
struct LegendContent
{
    bool show = false;          // false: the legend is cleared
    LegendWidget::Mode mode = LegendWidget::PointDataMode;
    bool hasHeader = false;
    QString sessionDesc;
    QString utcText;
    QString coordsText;
    QVector<LegendWidget::Row> rows;
};

LegendContent legendContentForMoment(const SessionModel &sessionModel,
                                     const MomentModel::Moment &moment,
                                     const QVector<PlotValue> &enabledPlots,
                                     const QString &xVariable,
                                     const QString &referenceMarkerKey);

} // namespace

LegendPresenter::LegendPresenter(SessionModel *sessionModel,
                                 PlotModel *plotModel,
                                 MomentModel *momentModel,
                                 PlotViewSettingsModel *plotViewSettings,
                                 MeasureModel *measureModel,
                                 LegendWidget *legendWidget,
                                 QObject *parent)
    : QObject(parent)
    , m_sessionModel(sessionModel)
    , m_plotModel(plotModel)
    , m_momentModel(momentModel)
    , m_plotViewSettings(plotViewSettings)
    , m_measureModel(measureModel)
    , m_legendWidget(legendWidget)
{
    m_updateTimer.setSingleShot(true);
    m_updateTimer.setInterval(0);

    connect(&m_updateTimer, &QTimer::timeout,
            this, &LegendPresenter::recompute);

    if (m_sessionModel) {
        connect(m_sessionModel, &SessionModel::modelChanged,
                this, &LegendPresenter::scheduleUpdate);
        connect(m_sessionModel, &SessionModel::visibilityChanged, this,
                [this](const QSet<QString> &, const QSet<QString> &) { scheduleUpdate(); });
    }

    if (m_plotModel) {
        connect(m_plotModel, &QAbstractItemModel::modelReset,
                this, &LegendPresenter::scheduleUpdate);
        connect(m_plotModel, &QAbstractItemModel::dataChanged,
                this, &LegendPresenter::scheduleUpdate);
    }

    if (m_momentModel) {
        connect(m_momentModel, &MomentModel::momentsChanged,
                this, &LegendPresenter::scheduleUpdate);
    }

    if (m_plotViewSettings) {
        connect(m_plotViewSettings, &PlotViewSettingsModel::xVariableChanged,
                this, &LegendPresenter::scheduleUpdate);
        connect(m_plotViewSettings, &PlotViewSettingsModel::referenceMarkerKeyChanged,
                this, &LegendPresenter::scheduleUpdate);
    }

    if (m_measureModel) {
        connect(m_measureModel, &MeasureModel::dataChanged,
                this, &LegendPresenter::scheduleUpdate);
    }

    // Connect to unit system changes for reactive updates
    connect(&UnitConverter::instance(), &UnitConverter::systemChanged,
            this, &LegendPresenter::scheduleUpdate);

    if (m_legendWidget) {
        m_legendWidget->clear();
    }

    scheduleUpdate();
}

void LegendPresenter::scheduleUpdate()
{
    if (m_updateTimer.isActive())
        return;

    m_updateTimer.start();
}

void LegendPresenter::recompute()
{
    if (!m_legendWidget) {
        return;
    }

    // If the measure tool is actively dragging, it overrides the legend display.
    if (m_measureModel && m_measureModel->isActive()) {
        const auto &mRows = m_measureModel->rows();

        QVector<LegendWidget::Row> widgetRows;
        widgetRows.reserve(mRows.size());
        for (const auto &mr : mRows) {
            LegendWidget::Row wr;
            wr.name       = mr.name;
            wr.color      = mr.color;
            wr.deltaValue = mr.deltaValue;
            wr.value      = mr.finalValue;
            wr.minValue   = mr.minValue;
            wr.avgValue   = mr.avgValue;
            wr.maxValue   = mr.maxValue;
            widgetRows.push_back(wr);
        }

        if (m_measureModel->isMultiTrack()) {
            // Multi-track: show only Min/Avg/Max, no header.
            m_legendWidget->setMode(LegendWidget::RangeStatsMode);
            m_legendWidget->setRows(widgetRows);
        } else {
            // Single-track: full measure layout with header.
            m_legendWidget->setMode(LegendWidget::MeasureMode);
            m_legendWidget->setHeader(m_measureModel->sessionDesc(),
                                      m_measureModel->utcText(),
                                      m_measureModel->coordsText());
            m_legendWidget->setRows(widgetRows);
        }
        return;
    }

    if (!m_sessionModel || !m_plotModel || !m_momentModel || !m_plotViewSettings) {
        m_legendWidget->clear();
        return;
    }

    const QString xVariable = m_plotViewSettings->xVariable();
    const QString referenceMarkerKey = m_plotViewSettings->referenceMarkerKey();

    const QVector<PlotValue> enabledPlots = m_plotModel->enabledPlots();
    if (enabledPlots.isEmpty()) {
        m_legendWidget->clear();
        return;
    }

    const MomentModel::Moment moment = chooseEffectiveMoment(m_momentModel);
    if (moment.id.isEmpty() || !moment.active) {
        m_legendWidget->clear();
        return;
    }

    // The sessions are read, into plain values, while a row stability guard is
    // held; the guard ends before the legend widget is updated.
    LegendContent content;
    {
        const SessionModel::RowStabilityGuard guard(*m_sessionModel);
        content = legendContentForMoment(*m_sessionModel, moment, enabledPlots,
                                         xVariable, referenceMarkerKey);
    }

    if (!content.show) {
        m_legendWidget->clear();
        return;
    }

    m_legendWidget->setMode(content.mode);
    if (content.hasHeader)
        m_legendWidget->setHeader(content.sessionDesc, content.utcText, content.coordsText);
    m_legendWidget->setRows(content.rows);
}

namespace {

// Reads sessions through pointers into the model: the caller holds a
// SessionModel::RowStabilityGuard for the whole call.
LegendContent legendContentForMoment(const SessionModel &sessionModel,
                                     const MomentModel::Moment &moment,
                                     const QVector<PlotValue> &enabledPlots,
                                     const QString &xVariable,
                                     const QString &referenceMarkerKey)
{
    // Build a fast id -> session lookup.
    QHash<QString, const SessionData *> sessionById;
    sessionById.reserve(sessionModel.rowCount());
    for (int si = 0; si < sessionModel.rowCount(); ++si) {
        const SessionRow &sr = sessionModel.rowAt(si);
        if (!sr.isLoaded()) continue;
        if (!sr.sessionId.isEmpty()) {
            sessionById.insert(sr.sessionId, &sr.session.value());
        }
    }

    // Resolve target sessions.
    QSet<QString> targets;
    const bool explicitTargets = !moment.targetSessions.isEmpty();

    if (explicitTargets) {
        for (const QString &sid : moment.targetSessions) {
            auto it = sessionById.constFind(sid);
            if (it == sessionById.constEnd())
                continue;
            if (!it.value()->isVisible())
                continue;
            targets.insert(sid);
        }
    } else {
        // Auto-visible-overlap: derive from visible sessions that overlap at the moment's position.
        for (int si = 0; si < sessionModel.rowCount(); ++si) {
            const SessionRow &sr = sessionModel.rowAt(si);
            if (!sr.isLoaded() || !sr.visible)
                continue;

            const QString &sid = sr.sessionId;
            if (sid.isEmpty())
                continue;

            const SessionData &s = sr.session.value();
            const auto utcOpt = utcSecondsForMoment(moment, s);
            if (!utcOpt.has_value())
                continue;

            const auto xOpt = plotAxisXFromUtc(*utcOpt, xVariable, referenceMarkerKey, s);
            if (!xOpt.has_value())
                continue;

            if (sessionOverlapsAtX(s, enabledPlots, xVariable, referenceMarkerKey, *xOpt)) {
                targets.insert(sid);
            }
        }
    }

    if (targets.isEmpty()) {
        return LegendContent();
    }

    // Helper: compute the reference offset for a given session
    auto offsetForSession = [&referenceMarkerKey, &xVariable](const SessionData &s) -> double {
        return markerOffsetSeconds(s, referenceMarkerKey, xVariable).value_or(0.0);
    };

    const bool pointMode = (targets.size() == 1);

    QVector<LegendWidget::Row> rows;
    rows.reserve(enabledPlots.size() + 1);

    bool hasData = false;

    if (pointMode) {
        const QString targetSessionId = *targets.constBegin();
        const SessionData *session = sessionById.value(targetSessionId, nullptr);
        if (!session) {
            return LegendContent();
        }

        const auto utcOpt = utcSecondsForMoment(moment, *session);
        if (!utcOpt.has_value()) {
            return LegendContent();
        }
        const double utcSecs = *utcOpt;

        const auto xOpt = plotAxisXFromUtc(utcSecs, xVariable, referenceMarkerKey, *session);
        if (!xOpt.has_value()) {
            return LegendContent();
        }
        const double plotX = *xOpt;

        // Convert plot coordinate to raw data space for interpolation
        const double offset = offsetForSession(*session);
        const double rawX = plotX + offset;

        // Prepend independent variable rows (e.g., time).
        for (const PlotValue &ipv : PlotRegistry::instance().independentPlots()) {
            if (ipv.measurementID != xVariable) continue;
            LegendWidget::Row row;
            row.name  = seriesDisplayName(ipv);
            row.color = ipv.defaultColor;
            row.value = formatXAxisValue(plotX, xVariable, referenceMarkerKey);
            rows.push_back(row);
            hasData = true;
        }

        // Rows
        for (const PlotValue &pv : enabledPlots) {
            LegendWidget::Row row;
            row.name = seriesDisplayName(pv);
            row.color = pv.defaultColor;

            const double v = interpolateSessionMeasurement(*session, pv.sensorID, xVariable, pv.measurementID, rawX);
            if (!std::isnan(v)) {
                hasData = true;
            }

            row.value = formatValue(v, pv.measurementID, pv.measurementType);
            rows.push_back(row);
        }

        if (!hasData) {
            return LegendContent();
        }

        // Header
        QString sessionDesc = session->getAttribute(SessionKeys::Description).toString();
        if (sessionDesc.isEmpty())
            sessionDesc = targetSessionId;

        QString utcText;
        QString coordsText;

        // UTC text comes directly from the moment's UTC position (no conversion needed)
        utcText = QStringLiteral("%1 UTC")
                      .arg(QDateTime::fromMSecsSinceEpoch(
                               qint64(utcSecs * 1000.0),
                               QTimeZone::UTC)
                               .toString(QStringLiteral("yy-MM-dd HH:mm:ss.zzz")));

        const double lat = interpolateSessionMeasurement(*session, QStringLiteral("GNSS"), SessionKeys::Time, QStringLiteral("lat"), utcSecs);
        const double lon = interpolateSessionMeasurement(*session, QStringLiteral("GNSS"), SessionKeys::Time, QStringLiteral("lon"), utcSecs);
        const double alt = interpolateSessionMeasurement(*session, QStringLiteral("GNSS"), SessionKeys::Time, QStringLiteral("hMSL"), utcSecs);

        if (!std::isnan(lat) && !std::isnan(lon) && !std::isnan(alt)) {
            // Convert altitude to display units
            double displayAlt = UnitConverter::instance().convert(alt, QStringLiteral("altitude"));
            QString altUnit = UnitConverter::instance().getUnitLabel(QStringLiteral("altitude"));
            int altPrecision = UnitConverter::instance().getPrecision(QStringLiteral("altitude"));
            if (altPrecision < 0) altPrecision = 1; // Fallback

            coordsText = QStringLiteral("(%1 deg, %2 deg, %3 %4)")
                             .arg(lat, 0, 'f', 7)
                             .arg(lon, 0, 'f', 7)
                             .arg(displayAlt, 0, 'f', altPrecision)
                             .arg(altUnit);
        }

        LegendContent content;
        content.show = true;
        content.mode = LegendWidget::PointDataMode;
        content.hasHeader = true;
        content.sessionDesc = sessionDesc;
        content.utcText = utcText;
        content.coordsText = coordsText;
        content.rows = rows;
        return content;
    }

    // Prepend independent variable rows (e.g., time).
    for (const PlotValue &ipv : PlotRegistry::instance().independentPlots()) {
        if (ipv.measurementID != xVariable) continue;
        LegendWidget::Row row;
        row.name  = seriesDisplayName(ipv);
        row.color = ipv.defaultColor;
        rows.push_back(row);
    }

    // RangeStatsMode
    for (const PlotValue &pv : enabledPlots) {
        LegendWidget::Row row;
        row.name = seriesDisplayName(pv);
        row.color = pv.defaultColor;

        QVector<double> valuesAtCursor;

        for (const QString &sid : targets) {
            const SessionData *session = sessionById.value(sid, nullptr);
            if (!session)
                continue;

            const auto utcOpt = utcSecondsForMoment(moment, *session);
            if (!utcOpt.has_value())
                continue;

            const auto xOpt = plotAxisXFromUtc(*utcOpt, xVariable, referenceMarkerKey, *session);
            if (!xOpt.has_value())
                continue;

            // Convert plot coordinate to raw data space for interpolation
            const double offset = offsetForSession(*session);
            const double rawX = *xOpt + offset;

            const double v = interpolateSessionMeasurement(*session, pv.sensorID, xVariable, pv.measurementID, rawX);
            if (!std::isnan(v)) {
                valuesAtCursor.append(v);
            }
        }

        if (!valuesAtCursor.isEmpty()) {
            hasData = true;

            const double minVal = *std::min_element(valuesAtCursor.begin(), valuesAtCursor.end());
            const double maxVal = *std::max_element(valuesAtCursor.begin(), valuesAtCursor.end());
            const double avgVal = std::accumulate(valuesAtCursor.begin(), valuesAtCursor.end(), 0.0)
                                    / valuesAtCursor.size();

            row.minValue = formatValue(minVal, pv.measurementID, pv.measurementType);
            row.avgValue = formatValue(avgVal, pv.measurementID, pv.measurementType);
            row.maxValue = formatValue(maxVal, pv.measurementID, pv.measurementType);
        } else {
            row.minValue = QStringLiteral("--");
            row.avgValue = QStringLiteral("--");
            row.maxValue = QStringLiteral("--");
        }

        rows.push_back(row);
    }

    if (!hasData) {
        return LegendContent();
    }

    LegendContent content;
    content.show = true;
    content.mode = LegendWidget::RangeStatsMode;
    content.rows = rows;
    return content;
}

} // namespace

} // namespace FlySight
