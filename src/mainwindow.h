#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>
#include <QVector>
#include <QSettings>
#include "smcinterface.h"
#include "hwmoninterface.h"
#include "fancontrolwidget.h"
#include "temperaturepanel.h"

enum FanSource {
    FAN_SOURCE_SMC = 0,
    FAN_SOURCE_HWMON = 1
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr, bool nonInteractive = false);
    ~MainWindow();
    bool isInitialized() const { return initialized; }
    QString initializationError() const { return initializationErrorMessage; }

private slots:
    void updateSensorData();
    void showError(const QString& message);
    void showWarning(const QString& message);
    void onManualModeRequested(int fanWidgetIndex, bool enable);
    void onTargetRPMChanged(int fanWidgetIndex, int rpm);
    void onSensorBasedModeChanged(int fanWidgetIndex, bool enable, int sensorIndex, int minTemp, int maxTemp);
    void savePreset();
    void loadPreset();
    void deletePreset();
    void exportPreset();
    void importPreset();
    void copyDebugLogToClipboard();
    void onTemperatureUnitChanged(bool useFahrenheit);
    void setLightTheme();
    void setDarkTheme();

public:
    bool loadPresetByName(const QString& presetName);
    QString getBootPresetName() const;
    bool loadBootPreset();

    // Single-writer coordination: only one instance (GUI or daemon) may
    // drive the fans at a time. The interactive GUI always takes precedence
    // over a background daemon so the user's manual changes are not silently
    // overwritten by a preset-applying daemon a second later.
    bool acquireFanControlLock(bool allowTakeover);
    void releaseFanControlLock();

private:
    enum ThemeMode {
        ThemeLight,
        ThemeDark
    };


private:
    SMCInterface *smcInterface;
    HWMonInterface *hwmonInterface;
    QVector<FanControlWidget*> fanWidgets;
    QVector<FanSource> fanSources;  // Track which interface each fan belongs to
    QVector<int> fanSourceIndices;  // Index within the source interface
    TemperaturePanel *tempPanel;
    QTimer *updateTimer;
    bool nonInteractiveMode;
    bool initialized;
    QString initializationErrorMessage;

    // Single-writer lock state (flock on /run/macsfancontrol.lock)
    int fanControlLockFd;
    QTimer *lockWatchTimer;

    QPalette defaultPalette;
    QString defaultStyleSheet;

    // Sensor-based control settings
    struct SensorBasedSettings {
        bool enabled;
        int sensorIndex;
        int minTemp;
        int maxTemp;
    };
    QVector<SensorBasedSettings> sensorSettings;
    ThemeMode currentTheme = ThemeLight;

    void setupUI();
    void createMenuBar();
    void connectSignals();
    void restoreAutoMode();
    void updateSensorListInFanWidgets();
    void applyTheme(ThemeMode theme);

    // Settings management
    void saveSettings();
    void loadSettings();
    void savePresetToSettings(const QString& presetName, bool launchAtBoot);
    void setPresetLaunchAtBoot(const QString& presetName, bool launchAtBoot);
    bool loadPresetFromSettings(const QString& presetName);
    void applyFanSettings(int fanIndex, FanMode mode, int targetRPM, int sensorIndex, int minTemp, int maxTemp);

    bool promptForPresetLaunchAtBoot(const QString& presetName, bool currentValue);
    // Autostart helpers (per-user)
    bool createUserAutostart(const QString& presetName);
    void removeUserAutostart();
};

#endif // MAINWINDOW_H
