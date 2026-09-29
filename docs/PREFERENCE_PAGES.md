# Preference pages

How a page of the Preferences dialog is laid out. The pages live in
`src/preferences/*settingspage.cpp`; a new page follows this document, and a
change to an existing page keeps it in line with it.

## Why a convention

Each page used to build its rows by hand from horizontal and vertical box
layouts, so every row decided for itself where its label sat, whether its
field grew, and how far a subordinate row was indented. Rows on one page did
not line up with each other, and no two pages looked alike. Qt has a layout
made for this: `QFormLayout` asks the platform style how a label-and-field
row should look, so one source lays pages out the Windows way on Windows
(labels left, fields grow) and the macOS way on macOS (labels right, fields
in a common column). Every rule below exists so that the form layout can do
that job.

## The page

- A page is a vertical layout of group boxes, with a stretch at the bottom
  so the groups sit at the top. A group holding a list or tree takes the
  stretch instead, so the list grows with the dialog.
- Each group names one topic in sentence case: "Ground reference",
  "Track appearance". The title names the topic, not the setting, so a
  group's rows never repeat it.
- A page whose settings can be reset has one "Reset to defaults" button,
  right-aligned at the bottom of the page, outside any group.

## The group

- A group's layout is a `QFormLayout`. Nothing sets the layout's label
  alignment, field growth policy or margins: the platform style chooses
  them.
- A row is one label and one field, added with `addRow(label, field)`. The
  label is sentence case, ends with a colon, and names the setting in a few
  words: "Text size:", "Maximum cached sessions:".
- A checkbox or a radio button is a row on its own, added with
  `addRow(widget)` so it spans both columns. Its text is the control, so it
  has no colon.
- A unit goes in the spin box's suffix, with a leading space where the
  convention writes one (" px", " kg", but "%"). A field that has no suffix,
  such as a line edit, is followed by a unit label inside a small horizontal
  layout with no margins; that layout is the row's field.
- Nobody sets a fixed width on a field. The platform policy decides whether
  a field grows, and the pages that already used a form layout look the way
  they do because of it. A colour swatch button is the exception: it is a
  swatch, not a text field, and keeps its size.

## A choice between modes

A setting with a few exclusive modes is a column of radio buttons, one per
row. A field that only means something in one mode sits on the row below
that mode's button, with its label indented by the width of a radio
indicator so it lines up with the button's text
(`PreferencePage::subordinateLabel` in `preferencepagestyle.h`). The field
is disabled while its mode is not the chosen one. A field that applies in
every mode comes after the mode rows, unindented.

## Lists

A group whose content is a list or tree holds the widget and, below it, a
row of buttons that act on the selection ("Add...", "Remove", "Move up"),
left-aligned with a stretch after them. The list takes the page's stretch.

## What this does not cover

Which settings exist and what they mean is the concern of the feature
documents. This document is only the shape of the page.
