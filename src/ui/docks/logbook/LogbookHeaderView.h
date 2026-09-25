#ifndef LOGBOOKHEADERVIEW_H
#define LOGBOOKHEADERVIEW_H

#include <QHeaderView>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QString>

#include "calculationdemand.h"

class QStyleOptionHeaderV2;

namespace FlySight {

class SessionModel;
class WorkingAnimation;

/// The logbook's column header. It paints what CalculationDemand reports for a
/// logbook column and decides nothing.
///
/// PAINTING. While any of a column's demand is waiting or running, the working
/// indicator (an arc that turns about once a second, DemandIndicator.h) sits at
/// the right of the section's text; once the work is finished and some
/// sessions could not be computed, the warning badge sits there instead. The
/// two are never shown together, and neither carries a count: the section's
/// two lines have no room, and the hover carries the numbers. A section whose
/// state isPlain() - every column that is not over a requested calculation,
/// and every requested column with nothing waiting, running or failed - is
/// painted by the unmodified base class, pixel for pixel.
///
/// MAPPING. The tree shows the SessionModel itself (no proxy): a logical
/// section index is a SessionModel column, and its id is
/// CalculationDemand::columnId() of that column. The id is computed on every
/// call and never cached, because rebuilding the columns and sorting both
/// reset the model. Moved sections need nothing more: painting, repainting and
/// hit tests all work on logical indices.
///
/// LAYOUT. The glyph is one text line tall and lies inside the style's
/// SE_HeaderLabel rect, which excludes the sort arrow, immediately right of
/// the centred text (left of it, right to left). The text keeps a symmetric
/// reserve for it, so that it stays centred, and each of its lines ("name",
/// "(unit)") is elided on its own. A section narrower than the glyph shows
/// none; its hover still works. sectionSizeFromContents() adds the reserve for
/// requested columns, so a double click on the section handle fits both.
///
/// HOVER. The whole section, glyph included, shows DemandState::toolTip.
///
/// NO GESTURE. No mouse handler is overridden: a click anywhere in a section,
/// glyph included, sorts exactly as the base class does, and no work is
/// started or stopped.
///
/// REPAINT. columnStateChanged(id) repaints that column's section; while any
/// column is working (workingColumnIds()) a WorkingAnimation repaints the
/// working sections 12.5 times a second, and it stops as soon as nothing is
/// working (statesChanged()).
///
/// The demand layer is held weakly: without it (never given, or destroyed
/// first) the header is exactly QHeaderView.
class LogbookHeaderView : public QHeaderView
{
    Q_OBJECT
public:
    /// A horizontal header for the logbook's tree, configured as QTreeView's own
    /// (movable sections, last section stretched, AlignLeft|AlignVCenter default
    /// alignment). `model` is the session model the tree shows; `demand` may be
    /// null (then the header is exactly QHeaderView).
    LogbookHeaderView(SessionModel *model, CalculationDemand *demand, QWidget *parent = nullptr);

    /// What hovering the section shows: columnState(<its column id>).toolTip;
    /// empty for a plain section, out of range, or without a demand layer.
    QString toolTipForSection(int logicalIndex) const;
    /// Where the section's indicator or badge is painted, in viewport
    /// coordinates; null when none is (plain, hidden, or too narrow). Tests only.
    QRect indicatorRect(int logicalIndex) const;
    WorkingAnimation *animation() const { return m_animation; }    ///< tests only

protected:
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override;
    QSize sectionSizeFromContents(int logicalIndex) const override;
    bool viewportEvent(QEvent *event) override;

private slots:
    void onColumnStateChanged(const QString &columnId);
    void syncAnimation();
    void onAnimationFrame();

private:
    QString columnIdOf(int logicalIndex) const;   ///< empty when out of range or not our model
    DemandState stateFor(int logicalIndex) const; ///< DemandState() when columnIdOf is empty or no demand
    /// The option QHeaderView::paintSection() paints `logicalIndex` with, for `rect`.
    /// True when the section's brushes differ from the header's (the brush
    /// origin then moves to the section).
    bool initSectionOption(QStyleOptionHeaderV2 &opt, int logicalIndex, const QRect &rect) const;
    /// Elides opt.text for the glyph's room and returns the glyph's rect (null
    /// when the label is narrower than the glyph). `opt` is fully initialized.
    QRect layoutGlyph(QStyleOptionHeaderV2 &opt) const;
    QRect sectionViewportRect(int logicalIndex) const;

    QPointer<SessionModel> m_model;
    QPointer<CalculationDemand> m_demand;
    WorkingAnimation *m_animation;                // child; never null
};

} // namespace FlySight

#endif // LOGBOOKHEADERVIEW_H
