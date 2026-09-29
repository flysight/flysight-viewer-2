#ifndef PREFERENCEPAGESTYLE_H
#define PREFERENCEPAGESTYLE_H

#include <QLabel>
#include <QStyle>
#include <QWidget>

// The one place a preference page reaches for a layout decision the form
// layout does not make by itself. docs/PREFERENCE_PAGES.md is the style
// these helpers implement.
namespace FlySight::PreferencePage {

// The label of a row that belongs to the radio button above it: indented by
// a radio indicator's width so its text lines up with the button's text, as
// the platform draws it. The caller adds the row to the form layout as any
// other row.
inline QLabel *subordinateLabel(const QString &text, QWidget *parent)
{
    QLabel *label = new QLabel(text, parent);
    const QStyle *style = parent->style();
    label->setIndent(style->pixelMetric(QStyle::PM_ExclusiveIndicatorWidth)
                     + style->pixelMetric(QStyle::PM_RadioButtonLabelSpacing));
    return label;
}

} // namespace FlySight::PreferencePage

#endif // PREFERENCEPAGESTYLE_H
