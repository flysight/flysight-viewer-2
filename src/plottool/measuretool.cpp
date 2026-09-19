#include "measuretool.h"
#include "../measuremodel.h"
#include "../plotmodel.h"
#include "../sessiondata.h"
#include "../crosshairmanager.h"
#include "../units/unitconverter.h"
#include "../plotutils.h"
#include "../plotregistry.h"
#include "../calculations/timecalculations.h"

#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace FlySight {
namespace {

// Convert plot-axis X to UTC seconds for header display.
// When xVariable is _system_time, the reconstructed absolute value is in
// system-time space and must be converted back to UTC via systemTimeToUtc.
double plotAxisXToUtcSeconds(double plotAxisX,
                             const QString &referenceMarkerKey,
                             const QString &xVariable,
                             const SessionData &session)
{
    const auto optOffset = markerOffsetSeconds(session, referenceMarkerKey, xVariable);
    if (!optOffset.has_value())
        return kNaN;

    const double rawValue = plotAxisX + *optOffset;

    if (xVariable == SessionKeys::SystemTime) {
        auto utc = Calculations::systemTimeToUtc(session, rawValue);
        return utc.has_value() ? *utc : kNaN;
    }
    return rawValue;
}

} // anonymous namespace

// What the measure model is given, as plain values.
struct MeasureTool::Measurement
{
    bool valid = false;         // false: the measure model is cleared
    bool multiTrack = false;
    QString sessionDesc;
    QString utcText;
    QString coordsText;
    QVector<MeasureModel::Row> rows;
};

// -----------------------------------------------------------------------

MeasureTool::MeasureTool(const PlotWidget::PlotContext &ctx)
    : m_widget(ctx.widget)
    , m_plot(ctx.plot)
    , m_graphMap(ctx.graphMap)
    , m_model(ctx.model)
    , m_plotModel(ctx.plotModel)
    , m_measureModel(ctx.measureModel)
    , m_rect(new QCPItemRect(ctx.plot))
    , m_lineLeft(new QCPItemLine(ctx.plot))
    , m_lineRight(new QCPItemLine(ctx.plot))
{
    m_rect->setVisible(false);
    m_rect->setClipToAxisRect(true);
    m_rect->setPen(Qt::NoPen);
    m_rect->setBrush(QColor(0, 120, 215, 40));

    m_lineLeft->setVisible(false);
    m_lineLeft->setClipToAxisRect(true);
    m_lineRight->setVisible(false);
    m_lineRight->setClipToAxisRect(true);
    applyLinePenFromPreferences();
}

bool MeasureTool::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return false;

    const bool shiftHeld = event->modifiers().testFlag(Qt::ShiftModifier);

    if (shiftHeld) {
        // Multi-track mode: measure across all visible tracks.
        m_multiTrack = true;
        m_lockedSessionId.clear();
    } else {
        // Single-track mode: lock to the focused (hovered) session.
        m_multiTrack = false;
        m_lockedSessionId = m_model->hoveredSessionId();
        if (m_lockedSessionId.isEmpty())
            return false;
        m_widget->lockFocusToSession(m_lockedSessionId);
    }

    m_measuring  = true;
    m_startPixel = event->pos();
    m_startX     = m_plot->xAxis->pixelToCoord(event->pos().x());

    // Show the overlay.
    double yLow  = m_plot->yAxis->range().lower;
    double yHigh = m_plot->yAxis->range().upper;
    m_rect->topLeft->setCoords(m_startX, yHigh);
    m_rect->bottomRight->setCoords(m_startX, yLow);
    m_rect->setVisible(true);

    applyLinePenFromPreferences();
    m_lineLeft->start->setCoords(m_startX, yLow);
    m_lineLeft->end->setCoords(m_startX, yHigh);
    m_lineLeft->setVisible(true);
    m_lineRight->start->setCoords(m_startX, yLow);
    m_lineRight->end->setCoords(m_startX, yHigh);
    m_lineRight->setVisible(true);

    m_plot->replot(QCustomPlot::rpQueuedReplot);

    return true;
}

bool MeasureTool::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_measuring)
        return false;

    updateMeasurement(event->pos());
    return true;
}

bool MeasureTool::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_measuring || event->button() != Qt::LeftButton)
        return false;

    m_measuring = false;
    m_multiTrack = false;
    m_rect->setVisible(false);
    m_lineLeft->setVisible(false);
    m_lineRight->setVisible(false);
    m_plot->replot(QCustomPlot::rpQueuedReplot);

    m_widget->unlockFocus();

    if (m_measureModel)
        m_measureModel->clear();

    return true;
}

bool MeasureTool::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    // If dragging and mouse leaves the widget, keep the measurement active —
    // it will update when the mouse re-enters or on release.
    return false;
}

void MeasureTool::closeTool()
{
    if (m_measuring) {
        m_measuring = false;
        m_multiTrack = false;
        m_rect->setVisible(false);
        m_lineLeft->setVisible(false);
        m_lineRight->setVisible(false);
        m_plot->replot(QCustomPlot::rpQueuedReplot);
        m_widget->unlockFocus();
    }
    if (m_measureModel)
        m_measureModel->clear();
}

// -----------------------------------------------------------------------

void MeasureTool::updateMeasurement(const QPoint &currentPixel)
{
    if (!m_measureModel || !m_plotModel)
        return;

    const double currentX = m_plot->xAxis->pixelToCoord(currentPixel.x());

    // Update the visual overlay.
    {
        double xLeft  = qMin(m_startX, currentX);
        double xRight = qMax(m_startX, currentX);
        double yLow   = m_plot->yAxis->range().lower;
        double yHigh  = m_plot->yAxis->range().upper;

        m_rect->topLeft->setCoords(xLeft, yHigh);
        m_rect->bottomRight->setCoords(xRight, yLow);
        m_rect->setVisible(true);

        applyLinePenFromPreferences();
        m_lineLeft->start->setCoords(xLeft, yLow);
        m_lineLeft->end->setCoords(xLeft, yHigh);
        m_lineLeft->setVisible(true);
        m_lineRight->start->setCoords(xRight, yLow);
        m_lineRight->end->setCoords(xRight, yHigh);
        m_lineRight->setVisible(true);

        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }

    const QString xVariable = m_widget->xVariable();
    const QString referenceMarkerKey = m_widget->referenceMarkerKey();
    const QVector<PlotValue> enabledPlots = m_plotModel->enabledPlots();

    if (enabledPlots.isEmpty()) {
        m_measureModel->clear();
        return;
    }

    // The sessions are read, into plain values, while a row stability guard is
    // held; the guard ends before the measure model is updated and emits.
    Measurement result;
    {
        const SessionModel::RowStabilityGuard guard(*m_model);
        result = measure(currentX, xVariable, referenceMarkerKey, enabledPlots);
    }

    if (!result.valid) {
        m_measureModel->clear();
        return;
    }

    m_measureModel->setData(result.sessionDesc, result.utcText, result.coordsText,
                            result.rows, result.multiTrack);
}

// Reads sessions through pointers into the model: the caller holds a
// SessionModel::RowStabilityGuard for the whole call.
MeasureTool::Measurement MeasureTool::measure(double currentX,
                                              const QString &xVariable,
                                              const QString &referenceMarkerKey,
                                              const QVector<PlotValue> &enabledPlots) const
{
    const double xLo = qMin(m_startX, currentX);
    const double xHi = qMax(m_startX, currentX);

    // Helper: compute the reference offset for a given session
    auto offsetForSession = [&referenceMarkerKey, &xVariable](const SessionData &s) -> double {
        return markerOffsetSeconds(s, referenceMarkerKey, xVariable).value_or(0.0);
    };

    if (m_multiTrack) {
        // ---- Multi-track: min/avg/max across all visible sessions ----

        // Collect visible session IDs from the graph map.
        QSet<QString> visibleSessionIds;
        for (auto it = m_graphMap->begin(); it != m_graphMap->end(); ++it) {
            QCPGraph *g = it.key();
            if (g && g->visible())
                visibleSessionIds.insert(it.value().sessionId);
        }

        // Build session lookup.
        QHash<QString, const SessionData *> sessionById;
        for (int si = 0; si < m_model->rowCount(); ++si) {
            const SessionRow &sr = m_model->rowAt(si);
            if (!sr.isLoaded()) continue;
            if (visibleSessionIds.contains(sr.sessionId))
                sessionById.insert(sr.sessionId, &sr.session.value());
        }

        if (sessionById.isEmpty()) {
            return Measurement();
        }

        QVector<MeasureModel::Row> rows;
        rows.reserve(enabledPlots.size() + 1);
        bool hasData = false;

        // Prepend independent variable rows (e.g., time).
        for (const PlotValue &ipv : PlotRegistry::instance().independentPlots()) {
            if (ipv.measurementID != xVariable) continue;
            MeasureModel::Row row;
            row.name  = seriesDisplayName(ipv);
            row.color = ipv.defaultColor;
            rows.push_back(row);
        }

        for (const PlotValue &pv : enabledPlots) {
            MeasureModel::Row row;
            row.name  = seriesDisplayName(pv);
            row.color = pv.defaultColor;

            // Collect samples from ALL visible sessions.
            QVector<double> samples;

            for (auto it = sessionById.constBegin(); it != sessionById.constEnd(); ++it) {
                const SessionData *session = it.value();
                const double offset = offsetForSession(*session);

                // Convert plot-space bounds to raw data space
                const double rawLo = xLo + offset;
                const double rawHi = xHi + offset;

                const QVector<double> xData = session->getMeasurement(pv.sensorID, xVariable);
                const QVector<double> yData = session->getMeasurement(pv.sensorID, pv.measurementID);

                // Interpolated endpoints
                const double yAtLo = interpolateAtX(xData, yData, rawLo);
                const double yAtHi = interpolateAtX(xData, yData, rawHi);
                if (!std::isnan(yAtLo)) samples.append(yAtLo);
                if (!std::isnan(yAtHi)) samples.append(yAtHi);

                // Interior data points
                if (!xData.isEmpty() && xData.size() == yData.size()) {
                    auto itBegin = std::lower_bound(xData.cbegin(), xData.cend(), rawLo);
                    auto itEnd   = std::upper_bound(xData.cbegin(), xData.cend(), rawHi);
                    for (auto jt = itBegin; jt != itEnd; ++jt) {
                        int i = static_cast<int>(std::distance(xData.cbegin(), jt));
                        double y = yData[i];
                        if (!std::isnan(y))
                            samples.append(y);
                    }
                }
            }

            if (samples.isEmpty()) {
                row.minValue = QStringLiteral("--");
                row.avgValue = QStringLiteral("--");
                row.maxValue = QStringLiteral("--");
            } else {
                hasData = true;
                double minVal = *std::min_element(samples.begin(), samples.end());
                double maxVal = *std::max_element(samples.begin(), samples.end());
                double avgVal = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();

                row.minValue = formatValue(minVal, pv.measurementID, pv.measurementType);
                row.avgValue = formatValue(avgVal, pv.measurementID, pv.measurementType);
                row.maxValue = formatValue(maxVal, pv.measurementID, pv.measurementType);
            }

            rows.push_back(row);
        }

        if (!hasData) {
            return Measurement();
        }

        Measurement result;
        result.valid = true;
        result.multiTrack = true;
        result.rows = rows;
        return result;
    }

    // ---- Single-track: locked session ----

    // Find the locked session.
    const SessionData *session = nullptr;
    int lockedRow = m_model->getSessionRow(m_lockedSessionId);
    if (lockedRow >= 0) {
        const SessionRow &sr = m_model->rowAt(lockedRow);
        if (sr.isLoaded())
            session = &sr.session.value();
    }
    if (!session) {
        return Measurement();
    }

    // Compute offset for raw data space conversion
    const double offset = offsetForSession(*session);
    const double rawStartX  = m_startX + offset;
    const double rawCurrentX = currentX + offset;
    const double rawLo = qMin(rawStartX, rawCurrentX);
    const double rawHi = qMax(rawStartX, rawCurrentX);

    // Build rows for each enabled plot value.
    QVector<MeasureModel::Row> rows;
    rows.reserve(enabledPlots.size() + 1);
    bool hasData = false;

    // Prepend independent variable rows (e.g., time).
    for (const PlotValue &ipv : PlotRegistry::instance().independentPlots()) {
        if (ipv.measurementID != xVariable) continue;
        MeasureModel::Row row;
        row.name  = seriesDisplayName(ipv);
        row.color = ipv.defaultColor;
        row.deltaValue = formatValue(currentX - m_startX, ipv.measurementID, ipv.measurementType);
        row.finalValue = formatXAxisValue(currentX, xVariable, referenceMarkerKey);
        hasData = true;
        rows.push_back(row);
    }

    for (const PlotValue &pv : enabledPlots) {
        MeasureModel::Row row;
        row.name  = seriesDisplayName(pv);
        row.color = pv.defaultColor;

        const QVector<double> xData = session->getMeasurement(pv.sensorID, xVariable);
        const QVector<double> yData = session->getMeasurement(pv.sensorID, pv.measurementID);

        const double initialVal = interpolateAtX(xData, yData, rawStartX);
        const double finalVal   = interpolateAtX(xData, yData, rawCurrentX);

        if (std::isnan(initialVal) && std::isnan(finalVal)) {
            row.deltaValue = QStringLiteral("--");
            row.finalValue = QStringLiteral("--");
            row.minValue   = QStringLiteral("--");
            row.avgValue   = QStringLiteral("--");
            row.maxValue   = QStringLiteral("--");
            rows.push_back(row);
            continue;
        }

        hasData = true;

        // Delta = final - initial
        if (!std::isnan(initialVal) && !std::isnan(finalVal))
            row.deltaValue = formatValue(finalVal - initialVal, pv.measurementID, pv.measurementType);
        else
            row.deltaValue = QStringLiteral("--");

        row.finalValue = formatValue(finalVal, pv.measurementID, pv.measurementType);

        // Compute min / avg / max across the range [rawLo, rawHi].
        // Collect: interpolated endpoints + all interior data points.
        QVector<double> samples;

        // Interpolated endpoints
        const double yAtLo = interpolateAtX(xData, yData, rawLo);
        const double yAtHi = interpolateAtX(xData, yData, rawHi);
        if (!std::isnan(yAtLo)) samples.append(yAtLo);
        if (!std::isnan(yAtHi)) samples.append(yAtHi);

        // Interior data points
        if (!xData.isEmpty() && xData.size() == yData.size()) {
            auto itBegin = std::lower_bound(xData.cbegin(), xData.cend(), rawLo);
            auto itEnd   = std::upper_bound(xData.cbegin(), xData.cend(), rawHi);
            for (auto it = itBegin; it != itEnd; ++it) {
                int i = static_cast<int>(std::distance(xData.cbegin(), it));
                double y = yData[i];
                if (!std::isnan(y))
                    samples.append(y);
            }
        }

        if (samples.isEmpty()) {
            row.minValue = QStringLiteral("--");
            row.avgValue = QStringLiteral("--");
            row.maxValue = QStringLiteral("--");
        } else {
            double minVal = *std::min_element(samples.begin(), samples.end());
            double maxVal = *std::max_element(samples.begin(), samples.end());
            double avgVal = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();

            row.minValue = formatValue(minVal, pv.measurementID, pv.measurementType);
            row.avgValue = formatValue(avgVal, pv.measurementID, pv.measurementType);
            row.maxValue = formatValue(maxVal, pv.measurementID, pv.measurementType);
        }

        rows.push_back(row);
    }

    if (!hasData) {
        return Measurement();
    }

    // Header: session description + UTC + coordinates at the *current* cursor position.
    QString sessionDesc = session->getAttribute(SessionKeys::Description).toString();
    if (sessionDesc.isEmpty())
        sessionDesc = m_lockedSessionId;

    QString utcText;
    QString coordsText;

    const double utcSecs = plotAxisXToUtcSeconds(currentX, referenceMarkerKey, xVariable, *session);
    if (!std::isnan(utcSecs)) {
        utcText = QStringLiteral("%1 UTC")
                      .arg(QDateTime::fromMSecsSinceEpoch(
                               qint64(utcSecs * 1000.0), Qt::UTC)
                               .toString(QStringLiteral("yy-MM-dd HH:mm:ss.zzz")));

        const double lat = interpolateSessionMeasurement(
            *session, QStringLiteral("GNSS"), SessionKeys::Time, QStringLiteral("lat"), utcSecs);
        const double lon = interpolateSessionMeasurement(
            *session, QStringLiteral("GNSS"), SessionKeys::Time, QStringLiteral("lon"), utcSecs);
        const double alt = interpolateSessionMeasurement(
            *session, QStringLiteral("GNSS"), SessionKeys::Time, QStringLiteral("hMSL"), utcSecs);

        if (!std::isnan(lat) && !std::isnan(lon) && !std::isnan(alt)) {
            double displayAlt = UnitConverter::instance().convert(alt, QStringLiteral("altitude"));
            QString altUnit   = UnitConverter::instance().getUnitLabel(QStringLiteral("altitude"));
            int altPrecision  = UnitConverter::instance().getPrecision(QStringLiteral("altitude"));
            if (altPrecision < 0) altPrecision = 1;

            coordsText = QStringLiteral("(%1 deg, %2 deg, %3 %4)")
                             .arg(lat, 0, 'f', 7)
                             .arg(lon, 0, 'f', 7)
                             .arg(displayAlt, 0, 'f', altPrecision)
                             .arg(altUnit);
        }
    }

    Measurement result;
    result.valid = true;
    result.sessionDesc = sessionDesc;
    result.utcText = utcText;
    result.coordsText = coordsText;
    result.rows = rows;
    return result;
}

void MeasureTool::applyLinePenFromPreferences()
{
    auto &prefs = PreferencesManager::instance();
    QColor color = prefs.getValue(PreferenceKeys::PlotsCrosshairColor).value<QColor>();
    if (!color.isValid())
        color = Qt::gray;
    double thickness = prefs.getValue(PreferenceKeys::PlotsCrosshairThickness).toDouble();
    if (thickness <= 0)
        thickness = 1.0;

    QPen pen(color, thickness, Qt::DashLine);
    m_lineLeft->setPen(pen);
    m_lineRight->setPen(pen);
}

} // namespace FlySight
