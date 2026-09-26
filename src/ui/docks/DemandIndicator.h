#ifndef DEMANDINDICATOR_H
#define DEMANDINDICATOR_H

// Painting shared by the plot list's rows and the logbook's column headers:
// the working indicator and the warning badge that present the demand layer's
// state (demandstate.h), and the clock that turns the indicator while
// something is working and stops when nothing is. QtCore and QtGui only: the
// glyphs, the glyph's size beside a line of text (glyphMetrics), and the
// clock's mechanics; no demand-layer type: what a view shows, its colour and
// hover, and how the one clock follows the demand layer are
// DemandIndicatorView.h's.

#include <QObject>
#include <QTimer>
#include <QtGlobal>

class QColor;
class QFontMetrics;
class QPainter;
class QRectF;

namespace FlySight {

/// The working indicator: an open arc of 270 degrees in `color`, with the pen
/// width of the badge, turned clockwise by `rotationDegrees` about the rect's
/// centre. Rotation 0 is the indicator at rest: the gap of 90 degrees starts
/// at twelve o'clock. Leaves the painter's state as it found it.
void drawWorkingGlyph(QPainter *painter, const QRectF &rect, const QColor &color, qreal rotationDegrees);
/// The warning badge: a filled amber triangle with a dark exclamation mark. The
/// colours are fixed, so it reads on light, dark and highlight backgrounds alike.
/// Leaves the painter's state as it found it.
void drawWarningGlyph(QPainter *painter, const QRectF &rect);

/// The glyph's side and the room it takes beside text, for one text line of
/// `metrics` in `room` pixels of height: side = min(room, line height),
/// spacing = max(2, side / 4). The plot rows and the column headers use it alike.
struct GlyphMetrics {
    int side = 0;
    int spacing = 0;
    int reserve() const { return side + spacing; }   ///< side and the spacing beside it
};
GlyphMetrics glyphMetrics(const QFontMetrics &metrics, int room);

/// The clock of the working indicator: one per application, created beside the
/// demand layer and handed to the views that show the indicator. It ticks only
/// while something is working (setActive(true); DemandIndicatorView.h's
/// followDemand() decides that), and it restarts at frame 0. It never repaints
/// anything itself: each view connects frameAdvanced() to its own repaint of
/// what is working, and reads angle() when it paints.
class WorkingAnimation : public QObject
{
    Q_OBJECT
public:
    static constexpr int kFrameIntervalMs = 80;   ///< 12.5 frames per second
    static constexpr int kFramesPerTurn = 12;     ///< 30 degrees per frame: one turn in 0.96 s

    explicit WorkingAnimation(QObject *parent = nullptr);

    void setActive(bool active);   ///< false also resets the frame to 0
    bool isActive() const { return m_active; }     ///< something is working
    bool isTicking() const;        ///< the timer runs: active and not frozen
    int  frame() const { return m_frame; }         ///< 0 .. kFramesPerTurn-1
    qreal angle() const;           ///< frame() * 360 / kFramesPerTurn

    // ---- test seams ----------------------------------------------------------
    void setFrozen(bool frozen);   ///< a frozen clock never ticks; isActive() is unaffected
    void advance();                ///< one frame now (wraps), and frameAdvanced()

signals:
    void frameAdvanced();

private:
    QTimer m_timer;                // interval kFrameIntervalMs, Qt::CoarseTimer
    int m_frame = 0;
    bool m_active = false;
    bool m_frozen = false;
};

} // namespace FlySight

#endif // DEMANDINDICATOR_H
