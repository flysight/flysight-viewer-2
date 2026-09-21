#ifndef FLYSIGHTTEST_PLOTFIXTURE_H
#define FLYSIGHTTEST_PLOTFIXTURE_H

#include <QString>
#include <QStringList>
#include <QVector>

#include "plotregistry.h"

// Synthetic plots over the explicit calculations of jobfixture.h, for tests of
// the plot request logic. Plots need measurement names; JobWorld's calculations
// produce attributes. On-demand "bridge" calculations turn one into the other,
// which also makes every row test exercise "sees through on-demand
// intermediates".

namespace FlySight {
class SessionModel;
}

namespace FlySightTest {

/// Registers on the GLOBAL registry and removes exactly its own registrations
/// when destroyed. Construct it AFTER JobWorld (and before the SessionModel);
/// destroy it BEFORE JobWorld.
///
/// Bridges: all OnDemand, one attribute input, one one-element measurement
/// output under sensor "Syn":
///
///  | Bridge id | Input attribute | Output    | Value        | Behind it                                      |
///  |-----------|-----------------|-----------|--------------|------------------------------------------------|
///  | plotG     | G_OUT           | Syn/g     | {G_OUT}      | gated                                          |
///  | plotG2    | G_OUT           | Syn/g2    | {G_OUT * 2}  | gated (a second plot on the same job)          |
///  | plotDB    | DB              | Syn/db    | {DB}         | derivB <- expB <- expA (a chain)               |
///  | plotEA    | EA1             | Syn/ea    | {EA1}        | expA (rejects EA_IN < 0: "negative input")     |
///  | plotT     | T_OUT           | Syn/t     | {T_OUT}      | thrower ("synthetic failure")                  |
///  | plotPlain | P_IN (stored)   | Syn/plain | {P_IN}       | nothing explicit                               |
///  | plotH     | H_OUT           | Syn/h     | {H_OUT}      | afterG <- gated (first link holds in the gate) |
///
/// plus one EXPLICIT calculation of the fixture: afterG (title "After G"),
/// input attribute G_OUT, output attribute H_OUT = G_OUT + 1, pure, no gate.
///
/// Literals for G_IN = 4: Syn/g {5}, Syn/g2 {10}, Syn/h {6}; for EA_IN = 4,
/// EB_IN = 10: Syn/db {19}, Syn/ea {5}.
class PlotFixture {
public:
    PlotFixture();
    ~PlotFixture();
    PlotFixture(const PlotFixture &) = delete;
    PlotFixture &operator=(const PlotFixture &) = delete;

    /// Every id this fixture registered, in registration order.
    QStringList registeredIds() const { return m_ids; }

    /// The seven plots of the table, in table order: category "Synthetic",
    /// plotName = the measurement id, role Dependent. For PlotModel::setPlots().
    static QVector<FlySight::PlotValue> plots();

    /// Shows or hides sessions through SessionModel::setRowsVisibility(), the
    /// application's path. Unknown ids are ignored.
    static void show(FlySight::SessionModel &model, const QStringList &ids, bool visible = true);
    /// Stores an input attribute through SessionModel::updateAttribute(), the
    /// application's edit path. False when the model refused.
    static bool giveInput(FlySight::SessionModel &model, const QString &id, const QString &key, double value);

private:
    QStringList m_ids;
};

} // namespace FlySightTest

#endif // FLYSIGHTTEST_PLOTFIXTURE_H
