#ifndef ZOOMSETTINGSPAGE_H
#define ZOOMSETTINGSPAGE_H

#include <QWidget>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QRadioButton>
#include <QGroupBox>

namespace FlySight {

class ZoomSettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit ZoomSettingsPage(QWidget *parent = nullptr);

private slots:
    void saveSettings();

private:
    QRadioButton *m_allDataRadio;
    QRadioButton *m_markerRangeRadio;
    QComboBox *m_startMarkerCombo;
    QComboBox *m_endMarkerCombo;
    QDoubleSpinBox *m_marginSpinBox;

    QGroupBox* createExtentGroup();
    void populateMarkerCombos();
    void updateMarkerCombosEnabled();
};

} // namespace FlySight

#endif // ZOOMSETTINGSPAGE_H
