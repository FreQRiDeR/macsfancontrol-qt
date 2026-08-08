#ifndef TEMPERATUREPANEL_H
#define TEMPERATUREPANEL_H

#include <QWidget>
#include <QScrollArea>
#include <QGridLayout>
#include <QLabel>
#include <QCheckBox>
#include <QMap>
#include <QVector>
#include "smcinterface.h"

class TemperaturePanel : public QWidget {
    Q_OBJECT

public:
    explicit TemperaturePanel(QWidget *parent = nullptr);

    void updateTemperatures(const QVector<TempSensor>& sensors);
    void setMacModel(const QString& model) { macModel = model; }
    bool isUsingFahrenheit() const { return useFahrenheit; }

signals:
    void unitChanged(bool useFahrenheit);

private slots:
    void onUnitToggleChanged(bool enabled);

public slots:
    void setUseFahrenheit(bool useFahrenheit);

private:
    QScrollArea *scrollArea;
    QWidget *contentWidget;
    QGridLayout *gridLayout;
    QMap<int, QLabel*> tempLabels;  // Maps sensor index to temperature label
    QString macModel;
    QCheckBox *unitToggle;
    bool useFahrenheit;
    QVector<TempSensor> lastTemps;

    QString formatTemperature(int millidegrees);
    QColor getTemperatureColor(double celsius);
};

#endif // TEMPERATUREPANEL_H
