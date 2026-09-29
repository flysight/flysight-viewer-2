#ifndef ALTITUDEMARKERSSETTINGSPAGE_H
#define ALTITUDEMARKERSSETTINGSPAGE_H

#include <QWidget>
#include <QColor>

class QVBoxLayout;
class QGroupBox;
class QListWidget;
class QPushButton;
class QComboBox;

namespace FlySight {

class AltitudeMarkersSettingsPage : public QWidget {
    Q_OBJECT

public:
    explicit AltitudeMarkersSettingsPage(QWidget *parent = nullptr);

public slots:
    void saveSettings();

private slots:
    void onColorButtonClicked();
    void onAddAltitude();
    void onRemoveAltitude();
private:
    QComboBox   *m_unitsComboBox;
    QPushButton *m_colorButton;
    QListWidget *m_altitudeList;
    QColor       m_currentColor;

    QGroupBox* createMarkersGroup();
    QGroupBox* createAltitudesGroup();

    void updateColorButtonStyle(const QColor &color);
    void loadSettings();
};

} // namespace FlySight

#endif // ALTITUDEMARKERSSETTINGSPAGE_H
