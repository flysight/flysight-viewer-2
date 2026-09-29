#ifndef IMPORTSETTINGSPAGE_H
#define IMPORTSETTINGSPAGE_H

#include <QWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QRadioButton>
#include <QGroupBox>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QVBoxLayout>
#include <QSettings>

namespace FlySight {

class ImportSettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit ImportSettingsPage(QWidget *parent = nullptr);

private slots:
    void saveSettings();

private:
    QRadioButton *automaticRadioButton;
    QRadioButton *fixedRadioButton;
    QLineEdit *fixedElevationLineEdit;
    QDoubleSpinBox *descentPauseSpinBox;
    QCheckBox *hideOthersCheckBox;
    QComboBox *orientationComboBox;

    QGroupBox* createGroundReferenceGroup();
    QGroupBox* createDescentPauseGroup();
    QGroupBox* createTrackVisibilityGroup();
    QGroupBox* createOrientationGroup();
};

} // namespace FlySight

#endif // IMPORTSETTINGSPAGE_H
