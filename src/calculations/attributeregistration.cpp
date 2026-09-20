#include "attributeregistration.h"
#include "../attributeregistry.h"
#include "../sessiondata.h"
#include "../units/unitdefinitions.h"

using namespace FlySight;

void FlySight::registerBuiltInAttributes() {
    auto& reg = AttributeRegistry::instance();

    reg.registerAttribute({
        QStringLiteral("Session"),
        QStringLiteral("Description"),
        SessionKeys::Description,
        AttributeFormatType::Text,
        true
    });

    reg.registerAttribute({
        QStringLiteral("Session"),
        QStringLiteral("Device Name"),
        SessionKeys::DeviceId,
        AttributeFormatType::Text,
        false   // recorded by the device: a header attribute is never edited
    });

    reg.registerAttribute({
        QStringLiteral("Session"),
        QStringLiteral("Start Time"),
        SessionKeys::StartTime,
        AttributeFormatType::DateTime,
        false
    });

    reg.registerAttribute({
        QStringLiteral("Session"),
        QStringLiteral("Duration"),
        SessionKeys::Duration,
        AttributeFormatType::Duration,
        false
    });

    reg.registerAttribute({
        QStringLiteral("Session"),
        QStringLiteral("Import Time"),
        SessionKeys::ImportTime,
        AttributeFormatType::DateTime,
        false
    });

    reg.registerAttribute({
        QStringLiteral("Session"),
        QStringLiteral("Exit Time"),
        SessionKeys::ExitTime,
        AttributeFormatType::DateTime,
        false
    });

    reg.registerAttribute({
        QStringLiteral("Location"),
        QStringLiteral("Ground Elevation"),
        SessionKeys::GroundElev,
        AttributeFormatType::Double,
        true,
        MeasurementTypes::Altitude
    });

    reg.registerAttribute({
        QStringLiteral("Location"),
        QStringLiteral("Course Reference"),
        SessionKeys::CourseRef,
        AttributeFormatType::DateTime,
        true
    });

    reg.registerAttribute({
        QStringLiteral("Aerodynamics"),
        QStringLiteral("Jumper Mass"),
        SessionKeys::JumperMass,
        AttributeFormatType::Double,
        true,
        MeasurementTypes::Mass
    });

    reg.registerAttribute({
        QStringLiteral("Aerodynamics"),
        QStringLiteral("Planform Area"),
        SessionKeys::PlanformArea,
        AttributeFormatType::Double,
        true,
        MeasurementTypes::Area
    });
}
