#ifndef APPCONTEXT_H
#define APPCONTEXT_H

class QSettings;

namespace FlySight {

class SessionModel;
class PlotModel;
class MarkerModel;
class MomentModel;
class PlotRangeModel;
class PlotViewSettingsModel;
class MeasureModel;
class JobQueue;
class CalculationDemand;

/**
 * Bundles all shared services that dock features may need.
 * This is a simple struct, not a service locator.
 * All pointers are non-owning; lifetime is managed by MainWindow.
 */
struct AppContext {
    SessionModel* sessionModel = nullptr;
    PlotModel* plotModel = nullptr;
    MarkerModel* markerModel = nullptr;
    MomentModel* momentModel = nullptr;
    PlotRangeModel* rangeModel = nullptr;
    PlotViewSettingsModel* plotViewSettings = nullptr;
    MeasureModel* measureModel = nullptr;
    JobQueue* jobQueue = nullptr;             // background calculation jobs; JobQueue::model() is what a jobs view would show
    CalculationDemand* calculationDemand = nullptr; // plot-list row state (may be null: rows are then plain)
    QSettings* settings = nullptr;
};

} // namespace FlySight

#endif // APPCONTEXT_H
