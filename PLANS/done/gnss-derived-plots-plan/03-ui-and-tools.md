# Phase 3: UI & Tools

## Overview

This phase wires the measurements registered in Phase 2 into the user-facing UI: plot registration with correct categories/colors, Plots menu with keyboard shortcuts, the "Set Course" tool (mirroring "Set Ground"), the "Course" marker, and an Aerodynamics preferences page. After this phase, all new GNSS-derived plots are visible, selectable, and configurable by the user.

## Dependencies

- **Depends on:** Phase 2 (Measurements) -- all 16 new calculated measurements must be registered so plots can reference them.
- **Blocks:** None
- **Assumptions:**
  - Phase 1 delivered: session keys `WindN`, `WindE`, `CourseRef`; preference keys `AeroMass`, `AeroArea` with defaults registered in `initializePreferences()`; unit types `ratio`, `specific_energy`, `specific_power`, `coefficient`.
  - Phase 2 delivered: all measurement registrations in `gnsscalculations.cpp` for `course`, `courseRate`, `glideRatio`, `diveAngle`, `diveAngleRate`, `accH`, `accD` (refactored), `wcVelH`, `accAlongTrack`, `accCrossTrack`, `lift`, `drag`, `specificEnergy`, `specificEnergyRate`.

## Tasks

### Task 3.1: Reorganize Plot Registration (Categories and New Plots)

**Purpose:** Rename the GNSS category, reorder existing plots, and register all new GNSS plots with correct colors so they appear in the plot selection dock.

**Files to modify:**
- `src/mainwindow.cpp` -- modify `registerBuiltInPlots()` (lines 907-987): rename category strings, reorder entries, add new plot registrations

**Technical Approach:**

In the `defaults` vector inside `registerBuiltInPlots()`:

1. **Rename category**: Change all existing `"GNSS"` category strings to `"GNSS (Basic)"`.

2. **Move accD**: Remove the `"Vertical acceleration"` entry from the GNSS (Basic) section and place it in the new GNSS (Advanced) section (see step 4).

3. **Insert new GNSS (Basic) plots** after "Total speed" and before "Horizontal accuracy", in this order:
   - `{"GNSS (Basic)", "Course",          "deg",   Qt::cyan,                                "GNSS", "course",        "angle"}`
   - `{"GNSS (Basic)", "Course rate",     "deg/s", Qt::darkCyan,                            "GNSS", "courseRate",     "rotation"}`
   - `{"GNSS (Basic)", "Glide ratio",     "",      Qt::darkCyan,                            "GNSS", "glideRatio",    "ratio"}`
   - `{"GNSS (Basic)", "Dive angle",      "deg",   Qt::magenta,                             "GNSS", "diveAngle",     "angle"}`
   - `{"GNSS (Basic)", "Dive angle rate", "deg/s", Qt::darkYellow,                          "GNSS", "diveAngleRate", "rotation"}`

4. **Add GNSS (Advanced) section** after the last GNSS (Basic) entry ("Number of satellites"), before the IMU section. Use specified colors for plots with original-viewer equivalents; pick visually distinct HSL colors for the four without originals (accH, wcVelH, accAlongTrack, accCrossTrack). The order must be:
   - `{"GNSS (Advanced)", "Horizontal acceleration",          "m/s^2", <color>, "GNSS", "accH",              "acceleration"}`
   - `{"GNSS (Advanced)", "Vertical acceleration",            "m/s^2", QColor::fromHsl(120, S, L_c), "GNSS", "accD", "acceleration"}`  (moved from Basic, keep original HSL color)
   - `{"GNSS (Advanced)", "Wind-corrected horizontal speed",  "m/s",   <color>, "GNSS", "wcVelH",            "speed"}`
   - `{"GNSS (Advanced)", "Along-track acceleration",         "m/s^2", <color>, "GNSS", "accAlongTrack",     "acceleration"}`
   - `{"GNSS (Advanced)", "Cross-track acceleration",         "m/s^2", <color>, "GNSS", "accCrossTrack",     "acceleration"}`
   - `{"GNSS (Advanced)", "Lift coefficient",                 "",      Qt::darkGreen,  "GNSS", "lift",        "coefficient"}`
   - `{"GNSS (Advanced)", "Drag coefficient",                 "",      Qt::darkBlue,   "GNSS", "drag",        "coefficient"}`
   - `{"GNSS (Advanced)", "Specific energy",                  "kJ/kg", Qt::darkGreen,  "GNSS", "specificEnergy",     "specific_energy"}`
   - `{"GNSS (Advanced)", "Specific energy rate",             "W/kg",  Qt::darkBlue,   "GNSS", "specificEnergyRate", "specific_power"}`

5. **Choose colors for the four "no-original" plots**. Use HSL values that are distinct from existing GNSS colors and from each other. Suggested approach -- pick hues not already used in the GNSS group (existing hues: 0, 120, 240, 300). Good candidates:
   - Horizontal acceleration: `QColor::fromHsl(30, S, L_w)` (orange)
   - Wind-corrected horizontal speed: `QColor::fromHsl(200, S, L_b)` (sky blue)
   - Along-track acceleration: `QColor::fromHsl(60, S, L_c)` (yellow-green)
   - Cross-track acceleration: `QColor::fromHsl(270, S, L_b)` (purple)

   These use the existing palette constants (`S`, `L_w`, `L_c`, `L_b`) defined at the top of `registerBuiltInPlots()`.

**Final GNSS (Basic) order** (13 entries):
1. Elevation
2. Horizontal speed
3. Vertical speed
4. Total speed
5. Course
6. Course rate
7. Glide ratio
8. Dive angle
9. Dive angle rate
10. Horizontal accuracy
11. Vertical accuracy
12. Speed accuracy
13. Number of satellites

**Acceptance Criteria:**
- [ ] All existing GNSS plots have category `"GNSS (Basic)"` instead of `"GNSS"`
- [ ] `accD` (Vertical acceleration) appears in `"GNSS (Advanced)"` category, not Basic
- [ ] 5 new Basic plots registered: course, courseRate, glideRatio, diveAngle, diveAngleRate
- [ ] 8 new Advanced plots registered (accH, wcVelH, accAlongTrack, accCrossTrack, lift, drag, specificEnergy, specificEnergyRate) plus the moved accD = 9 total in Advanced
- [ ] Colors match the specification table for plots with original-viewer equivalents
- [ ] Plot order within each category matches the specification
- [ ] Application builds and all plots appear in the plot selection dock under correct categories

**Complexity:** M

---

### Task 3.2: Update Plots Menu

**Purpose:** Add new plots to the Plots menu with correct keyboard shortcuts and separator grouping.

**Files to modify:**
- `src/mainwindow.cpp` -- modify `initializePlotsMenu()` (lines 1119-1199): update the `plotsMenuItems` vector

**Technical Approach:**

Replace the `plotsMenuItems` vector in `initializePlotsMenu()` with the full specified layout. Follow the existing `PlotMenuItem` pattern (see `src/mainwindow.h` lines 99-118 for the struct).

The new vector contents:

```
PlotMenuItem("Elevation", QKeySequence(Qt::Key_E), "GNSS", "z"),

PlotMenuItem(PlotMenuItemType::Separator),

PlotMenuItem("Horizontal Speed", QKeySequence(Qt::Key_H), "GNSS", "velH"),
PlotMenuItem("Vertical Speed", QKeySequence(Qt::Key_V), "GNSS", "velD"),
PlotMenuItem("Total Speed", QKeySequence(Qt::Key_S), "GNSS", "vel"),

PlotMenuItem(PlotMenuItemType::Separator),

PlotMenuItem("Course", QKeySequence(Qt::Key_C), "GNSS", "course"),
PlotMenuItem("Course Rate", QKeySequence(Qt::SHIFT | Qt::Key_C), "GNSS", "courseRate"),

PlotMenuItem(PlotMenuItemType::Separator),

PlotMenuItem("Glide Ratio", QKeySequence(Qt::Key_G), "GNSS", "glideRatio"),
PlotMenuItem("Dive Angle", QKeySequence(Qt::Key_A), "GNSS", "diveAngle"),
PlotMenuItem("Dive Angle Rate", QKeySequence(Qt::SHIFT | Qt::Key_A), "GNSS", "diveAngleRate"),

PlotMenuItem(PlotMenuItemType::Separator),

PlotMenuItem("Horizontal Accuracy", QKeySequence(Qt::SHIFT | Qt::Key_H), "GNSS", "hAcc"),
PlotMenuItem("Vertical Accuracy", QKeySequence(Qt::SHIFT | Qt::Key_V), "GNSS", "vAcc"),
PlotMenuItem("Speed Accuracy", QKeySequence(Qt::SHIFT | Qt::Key_S), "GNSS", "sAcc"),

PlotMenuItem(PlotMenuItemType::Separator),

PlotMenuItem("Number of Satellites", QKeySequence(Qt::SHIFT | Qt::Key_N), "GNSS", "numSV"),

PlotMenuItem(PlotMenuItemType::Separator),

PlotMenuItem("Lift Coefficient", QKeySequence(Qt::Key_L), "GNSS", "lift"),
PlotMenuItem("Drag Coefficient", QKeySequence(Qt::Key_D), "GNSS", "drag"),
```

Note: The existing hotkeys (E, H, V, S, Shift+H, Shift+V, Shift+S, Shift+N) are preserved. New hotkeys added: C, Shift+C, G, A, Shift+A, L, D.

**Acceptance Criteria:**
- [ ] Menu shows all 16 entries in the specified order with correct separator grouping
- [ ] All hotkeys work: E, H, V, S, C, Shift+C, G, A, Shift+A, Shift+H, Shift+V, Shift+S, Shift+N, L, D
- [ ] Each menu action toggles the correct plot (correct sensorID/measurementID pairing)
- [ ] Existing hotkeys still function as before

**Complexity:** S

---

### Task 3.3: Create Set Course Tool

**Purpose:** Allow users to set a course reference value by clicking on the plot, mirroring the Set Ground tool pattern.

**Files to create:**
- `src/plottool/setcoursetool.h` -- Header for SetCourseTool class
- `src/plottool/setcoursetool.cpp` -- Implementation of SetCourseTool

**Files to modify:**
- `src/CMakeLists.txt` -- Add `plottool/setcoursetool.h plottool/setcoursetool.cpp` to the source list (after the `setgroundtool` entries around line 258)
- `src/ui/docks/plot/PlotWidget.h` -- Add `SetCourse` to `Tool` enum (line 50, after `SetGround`); add forward declaration `class SetCourseTool;` (line 28, after `SetGroundTool`); add `std::unique_ptr<SetCourseTool> m_setCourseTool;` member (after `m_setGroundTool`, around line 196)
- `src/ui/docks/plot/PlotWidget.cpp` -- Add `#include "plottool/setcoursetool.h"`; instantiate `m_setCourseTool = std::make_unique<SetCourseTool>(ctx);` (after `m_setGroundTool` construction, around line 134); add `case Tool::SetCourse:` to `setCurrentTool()` switch (after `SetGround` case, around line 266)
- `src/mainwindow.ui` -- Add `action_SetCourse` action definition (follow `action_SetGround` pattern): checkable=true, text=`"Set C&ourse"`, shortcut=`"O"`; add `<addaction name="action_SetCourse"/>` after `action_SetGround` in the Tools menu
- `src/mainwindow.h` -- Add slot declaration `void on_action_SetCourse_triggered();` (after `on_action_SetGround_triggered()`, line 84)
- `src/mainwindow.cpp` -- Add `on_action_SetCourse_triggered()` handler (after `on_action_SetGround_triggered()`); add `case PlotWidget::Tool::SetCourse:` to `onPlotWidgetToolChanged()` switch (after SetGround case, around line 880); add `toolActionGroup->addAction(ui->action_SetCourse);` to `setupPlotTools()` (after the SetGround line, around line 1371)

**Technical Approach:**

**`setcoursetool.h`**: Copy `setgroundtool.h` exactly, renaming:
- Class: `SetCourseTool`
- Include guard: `SETCOURSETOOL_H`
- Private method: `computeCourseAtCursor(SessionData &session, double xCoord) const` returning `double`

**`setcoursetool.cpp`**: Copy `setgroundtool.cpp` and adapt:
- `computeCourseAtCursor()`: Same interpolation logic as `computeGroundElevation()`, but instead of `"hMSL"` as the measurement, use `"course"` (the raw unwrapped course measurement registered in Phase 2). The sensor remains `"GNSS"`.
- `mousePressEvent()`: Same structure as SetGroundTool. For each traced session, call `computeCourseAtCursor()` and store the result via `m_model->updateAttribute(sessionId, SessionKeys::CourseRef, newCourse)`.
- Include `"sessiondata.h"` for `SessionKeys::CourseRef`.

**`mainwindow.ui` action** (XML to add after the `action_SetGround` closing `</action>` tag, around line 218):
```xml
<action name="action_SetCourse">
 <property name="checkable">
  <bool>true</bool>
 </property>
 <property name="text">
  <string>Set C&amp;ourse</string>
 </property>
 <property name="shortcut">
  <string>O</string>
 </property>
</action>
```

**`mainwindow.cpp` handler**:
```cpp
void MainWindow::on_action_SetCourse_triggered()
{
    auto* plotFeature = findFeature<PlotDockFeature>();
    if (plotFeature && plotFeature->plotWidget()) {
        plotFeature->plotWidget()->setCurrentTool(PlotWidget::Tool::SetCourse);
    }
}
```

**`onPlotWidgetToolChanged()` case**:
```cpp
case PlotWidget::Tool::SetCourse:
    ui->action_SetCourse->setChecked(true);
    break;
```

**Acceptance Criteria:**
- [ ] `SetCourseTool` class compiles and follows the exact pattern of `SetGroundTool`
- [ ] "Set Course" appears in Tools menu with hotkey O, is checkable, and is part of the tool action group
- [ ] Clicking the plot while Set Course is active interpolates the `course` measurement at the cursor and stores it as `_COURSE_REF`
- [ ] Tool reverts to primary tool after click
- [ ] Tool action group properly syncs (checking Set Course unchecks other tools and vice versa)

**Complexity:** M

---

### Task 3.4: Register Course Marker

**Purpose:** Display a "Course" reference marker on the plot when the user sets a course reference, matching the pattern of existing reference markers.

**Files to modify:**
- `src/mainwindow.cpp` -- Add a new entry to the `defaults` vector in `registerBuiltInMarkers()` (lines 884-905)

**Technical Approach:**

Add the following entry to the `defaults` vector in `registerBuiltInMarkers()`, in the "Reference" category section (after the "Landing" marker or wherever appropriate among the Reference markers):

```cpp
{"Reference", "Course", "Crs", QColor(0, 255, 255), SessionKeys::CourseRef, {}, true, {}, true},
```

Field mapping to `MarkerDefinition` struct:
- `category`: `"Reference"`
- `displayName`: `"Course"`
- `shortLabel`: `"Crs"`
- `color`: `QColor(0, 255, 255)` (cyan, matching the course plot color)
- `attributeKey`: `SessionKeys::CourseRef` (which is `"_COURSE_REF"`, registered in Phase 1)
- `measurements`: `{}` (empty -- this is a reference marker, not tied to a specific data measurement for display)
- `editable`: `true`
- `groupId`: `{}` (empty -- statically registered)
- `defaultEnabled`: `true`

**Acceptance Criteria:**
- [ ] "Course" marker appears in the marker list under the "Reference" category
- [ ] Marker short label is "Crs"
- [ ] Marker is editable (user can drag to reposition)
- [ ] Marker is enabled by default
- [ ] Setting a course reference via the Set Course tool causes the marker to appear on the plot

**Complexity:** S

---

### Task 3.5: Add Aerodynamics Preferences Page

**Purpose:** Provide UI controls for the mass and planform area preferences used by lift/drag coefficient calculations.

**Files to create:**
- `src/preferences/aerodynamicssettingspage.h` -- Header for the preferences page
- `src/preferences/aerodynamicssettingspage.cpp` -- Implementation

**Files to modify:**
- `src/CMakeLists.txt` -- Add `preferences/aerodynamicssettingspage.h preferences/aerodynamicssettingspage.cpp` to the source list (near other preferences pages, around line 263)
- `src/preferences/preferencesdialog.cpp` -- Add `#include "aerodynamicssettingspage.h"`; add `"Aerodynamics"` item to `categoryList` (after `"Logbook"`, line 34); add `AerodynamicsSettingsPage` widget to `stackedWidget` (after the logbook page, around line 49); connect the OK button's accepted signal to the page's `saveSettings()` slot

**Technical Approach:**

Follow the `ImportSettingsPage` pattern (`src/preferences/importsettingspage.h` and `.cpp`).

**`aerodynamicssettingspage.h`**:
```cpp
class AerodynamicsSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit AerodynamicsSettingsPage(QWidget *parent = nullptr);
public slots:
    void saveSettings();
private:
    QDoubleSpinBox *m_massSpinBox;
    QDoubleSpinBox *m_areaSpinBox;
    QGroupBox* createBodyGroup();
};
```

**`aerodynamicssettingspage.cpp`**:

Constructor:
- Create layout with `createBodyGroup()`, then `addStretch()`
- Connect both spin boxes' `valueChanged(double)` to `saveSettings()`

`createBodyGroup()`:
- QGroupBox titled "Body parameters"
- QFormLayout with two rows:
  - "Mass" label + `m_massSpinBox`: range 0.1-500, decimals 1, suffix " kg", singleStep 0.1
  - "Planform area" label + `m_areaSpinBox`: range 0.01-100, decimals 2, suffix " m\u00B2", singleStep 0.01
- Initialize spin box values from `PreferencesManager::instance().getValue(PreferenceKeys::AeroMass).toDouble()` and `PreferenceKeys::AeroArea`

`saveSettings()`:
- `prefs.setValue(PreferenceKeys::AeroMass, m_massSpinBox->value())`
- `prefs.setValue(PreferenceKeys::AeroArea, m_areaSpinBox->value())`

**`preferencesdialog.cpp` changes**:

1. Add include: `#include "aerodynamicssettingspage.h"`
2. After `categoryList->addItem(tr("Logbook"));` (line 34), add: `categoryList->addItem(tr("Aerodynamics"));`
3. After the logbook page is added to `stackedWidget` (line 49), add:
   ```cpp
   AerodynamicsSettingsPage *aeroPage = new AerodynamicsSettingsPage(this);
   stackedWidget->addWidget(aeroPage);    // Index 9: Aerodynamics
   ```
4. After `connect(buttonBox, &QDialogButtonBox::accepted, logbookPage, &LogbookSettingsPage::saveSettings);` (line 59), add:
   ```cpp
   connect(buttonBox, &QDialogButtonBox::accepted, aeroPage, &AerodynamicsSettingsPage::saveSettings);
   ```

**Acceptance Criteria:**
- [ ] "Aerodynamics" appears in the preferences sidebar after "Logbook"
- [ ] Page shows "Body parameters" group with Mass and Planform area spin boxes
- [ ] Mass spin box: range 0.1-500, 1 decimal place, default 1.0, suffix "kg"
- [ ] Planform area spin box: range 0.01-100, 2 decimal places, default 1.0, suffix "m^2"
- [ ] Changes auto-save on spin box value change
- [ ] Changes also save on OK button click
- [ ] Values persist across preferences dialog open/close cycles

**Complexity:** M

---

## Testing Requirements

### Unit Tests
- No new unit tests are strictly required for this phase (UI wiring), but if the project has existing test infrastructure for preferences or registries, verify that new registrations don't break existing tests.

### Integration Tests
- None required beyond manual verification.

### Manual Verification
1. **Plot selection dock**: Open the plot selection dock. Verify "GNSS (Basic)" and "GNSS (Advanced)" categories appear. Verify all 13 Basic and 9 Advanced plots are listed in the correct order. Verify "Vertical acceleration" is under Advanced, not Basic.
2. **Plots menu**: Open the Plots menu. Verify all 16 entries appear with correct separators. Test each hotkey (E, H, V, S, C, Shift+C, G, A, Shift+A, Shift+H, Shift+V, Shift+S, Shift+N, L, D) toggles the correct plot.
3. **Plot colors**: Enable each new plot and verify it renders with the correct default color.
4. **Set Course tool**: Load a GNSS track. Select "Set Course" from Tools menu (or press O). Click on the plot. Verify the course reference marker appears. Verify the course plot values shift by the reference amount. Verify the tool reverts to Pan after click.
5. **Course marker**: After setting a course reference, verify the "Crs" marker appears on the plot. Verify it is draggable/editable.
6. **Aerodynamics preferences**: Open Preferences, select "Aerodynamics". Verify Mass and Planform area controls appear with correct defaults, ranges, and units. Change values, close and reopen -- verify persistence.
7. **Tool action group**: Verify that selecting Set Course unchecks other tools, and selecting another tool unchecks Set Course.

## Notes for Implementer

### Gotchas
- **Category list/stacked widget order**: In `preferencesdialog.cpp`, the order of `categoryList->addItem()` calls must exactly match the order of `stackedWidget->addWidget()` calls. Adding "Aerodynamics" after "Logbook" in both places is critical.
- **mainwindow.ui XML**: The `.ui` file is machine-generated XML. When adding `action_SetCourse`, place it in both the `<addaction>` list inside the Tools `<widget>` (for menu ordering) and as a standalone `<action>` element (for the definition). Follow the exact XML structure of `action_SetGround`.
- **Qt::Named colors**: `Qt::cyan`, `Qt::darkCyan`, `Qt::magenta`, `Qt::darkYellow`, `Qt::darkGreen`, `Qt::darkBlue` are valid `QColor` constructors. They can be used directly in the `PlotValue` struct initialization.
- **Course measurement interpolation**: The Set Course tool interpolates the `"course"` measurement, which is the unwrapped course. This is correct -- the courseRef is subtracted from the unwrapped value to produce the final displayed course.
- **Preference keys**: `PreferenceKeys::AeroMass` and `PreferenceKeys::AeroArea` must already exist in `preferencekeys.h` (added in Phase 1). Verify they are present before implementing the preferences page.
- **Auto-connect naming**: Qt's auto-connect mechanism requires `on_action_SetCourse_triggered()` to match the action name `action_SetCourse` in the `.ui` file exactly.

### Decisions Made
- **Course marker color**: Chose `QColor(0, 255, 255)` (pure cyan) to match the course plot's `Qt::cyan` default color, maintaining visual consistency between marker and plot.
- **Course marker measurements**: Left empty (`{}`), consistent with other reference markers like Exit, Video sync, etc., which don't reference specific data vectors.
- **HSL colors for new plots without originals**: Suggested specific hue values (30, 200, 60, 270) using existing palette constants. These are suggestions; the implementer may adjust as long as they are visually distinct and use the existing `S`, `L_w`, `L_c`, `L_b` constants.
- **Preferences page group title**: Used "Body parameters" as the group box title, as it describes both mass and planform area together.

### Open Questions
- None. All requirements are fully specified.

## Definition of Done

This phase is complete when:
1. All tasks have passing acceptance criteria
2. Application builds without errors
3. All existing plots, tools, and preferences continue to function
4. New plots appear in correct categories with correct colors
5. Plots menu shows all entries with working hotkeys
6. Set Course tool functions correctly (interpolate, store attribute, revert)
7. Course marker appears and is editable
8. Aerodynamics preferences page persists mass and area values
9. No TODOs or placeholder code remains
