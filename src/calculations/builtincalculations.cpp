#include "builtincalculations.h"
#include "attributecalculations.h"
#include "gnsscalculations.h"
#include "imucalculations.h"
#include "magcalculations.h"
#include "timecalculations.h"
#include "simplificationcalculations.h"
#include "wspcalculations.h"
#include "spcalculations.h"
#include "interpolationcalculations.h"
#include "../conversion/sourceconversion.h"

namespace FlySight {

void registerBuiltInCalculations(CalculationRegistry &registry)
{
    // The conversion layer (source -> effective values) first. Source
    // conversions are kept in their own ordered list, so their position among
    // the built-ins does not affect any candidate order; it only keeps
    // registeredIds() readable.
    Calculations::registerSourceConversions(registry);

    // The order is fixed: it decides which of several candidates for one output
    // is tried first (only _START_TIME / _DURATION have several). Interpolation
    // comes last, after every calculation with an explicit output name.
    Calculations::registerAttributeCalculations(registry);
    Calculations::registerGnssCalculations(registry);
    Calculations::registerImuCalculations(registry);
    Calculations::registerMagCalculations(registry);
    Calculations::registerTimeCalculations(registry);
    Calculations::registerSimplificationCalculations(registry);
    Calculations::registerWspCalculations(registry);
    Calculations::registerSpCalculations(registry);
    Calculations::registerInterpolationFamily(registry);
}

void registerBuiltInCalculationMetadata()
{
    Calculations::registerWspMetadata();
    Calculations::registerSpMetadata();
}

} // namespace FlySight
