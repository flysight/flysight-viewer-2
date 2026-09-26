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
class WorkingAnimation;

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
    CalculationDemand* calculationDemand = nullptr; // what the plot list's rows and the logbook's column headers and cells present (may be null: all plain)
    WorkingAnimation* workingClock = nullptr;  // the working indicator's one clock (DemandIndicator.h); made to follow the demand layer (followDemand, DemandIndicatorView.h) (may be null: the indicator does not turn)
    QSettings* settings = nullptr;
};

} // namespace FlySight

#endif // APPCONTEXT_H
