#include "mainwindow.h"
#include "sensordescriptions.h"
#include <unistd.h>
#include <sys/file.h>
#include <signal.h>
#include <fcntl.h>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QStatusBar>
#include <QMessageBox>
#include <QTime>
#include <QApplication>
#include <QDebug>
#include <QLabel>
#include <QPalette>
#include <QColor>
#include <QInputDialog>
#include <QSettings>
#include <QStandardPaths>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCommandLineParser>
#include <QDialog>
#include <QDialogButtonBox>
#include <QProcess>
#include <QCheckBox>
#include <QLineEdit>
#include <QVBoxLayout>



MainWindow::MainWindow(QWidget *parent, bool nonInteractive)
    : QMainWindow(parent),
      smcInterface(new SMCInterface(this)),
      hwmonInterface(new HWMonInterface(this)),
      tempPanel(new TemperaturePanel(this)),
      updateTimer(new QTimer(this)),
      nonInteractiveMode(nonInteractive),
      initialized(false),
      fanControlLockFd(-1),
      hasFanControlLock(false),
      lockWatchTimer(new QTimer(this))
{
    bool smcAvailable = false;
    bool hwmonAvailable = false;

    // Initialize SMC interface
    if (smcInterface->initialize()) {
        smcAvailable = true;
        qDebug() << "SMC interface initialized";
    } else {
        qWarning() << "SMC interface not available";
    }

    // Initialize HWMon interface
    hwmonInterface->setSmcAvailable(smcAvailable);
    if (hwmonInterface->initialize()) {
        hwmonAvailable = true;
        qDebug() << "HWMon interface initialized";
    } else {
        qWarning() << "HWMon interface not available";
    }

    // Check if at least one interface is available
    if (!smcAvailable && !hwmonAvailable) {
        initializationErrorMessage = "No fan control interfaces found. "
                                     "Make sure you're running on compatible hardware with "
                                     "applesmc or hwmon drivers loaded.";
        qCritical().noquote() << initializationErrorMessage;
        if (!nonInteractiveMode) {
            QMessageBox::critical(this, "Initialization Error", initializationErrorMessage);
            QTimer::singleShot(0, qApp, &QApplication::quit);
        }
        return;
    }

    // Check for write permissions
    bool canWriteSMC = smcAvailable && smcInterface->hasWritePermission();
    bool canWriteHWMon = hwmonAvailable && hwmonInterface->hasWritePermission();

    if ((smcAvailable && !canWriteSMC) || (hwmonAvailable && !canWriteHWMon)) {
        QString permissionWarning = "Application does not have write permissions to all fan interfaces.\n"
                                    "You may be able to monitor some fans but not control them.\n"
                                    "Run with: sudo macsfancontrol";
        qWarning().noquote() << permissionWarning;
        if (!nonInteractiveMode) {
            QMessageBox::warning(this, "Permission Warning", permissionWarning);
        }
    }

    // Acquire the single-writer fan-control lock. The interactive GUI is
    // allowed to take over from a running daemon (it will signal the daemon
    // to stand down); a daemon must not displace another live instance.
    hasFanControlLock = acquireFanControlLock(/*allowTakeover=*/!nonInteractiveMode);
    if (!hasFanControlLock) {
        initializationErrorMessage = "Another instance of Mac Fan Control is already "
                                     "controlling the fans.";
        qWarning().noquote() << initializationErrorMessage;
        if (nonInteractiveMode) {
            // Daemon could not acquire the lock: another instance owns it.
            return;
        }
    }

    // Daemon must continually verify it still owns the lock; if the GUI
    // launches and takes over, the daemon stands down WITHOUT restoring auto
    // mode (the GUI inherits fan control).
    if (nonInteractiveMode) {
        connect(lockWatchTimer, &QTimer::timeout, this, [this]() {
            if (fanControlLockFd < 0)
                return;
            // Re-test the lock non-blocking. If we can re-acquire it we still
            // own it (flock is associated with the open file description).
            if (flock(fanControlLockFd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
                qInfo() << "Fan control lock taken over by another instance; daemon standing down.";
                releaseFanControlLock();
                QCoreApplication::quit();
            }
        });
        lockWatchTimer->start(2000);
    }

    defaultPalette = qApp->palette();
    defaultStyleSheet = qApp->styleSheet();

    setupUI();
    createMenuBar();
    connectSignals();

    // Load custom sensor descriptions if available
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
                        "/macsfancontrol/sensor_descriptions.conf";
    SensorDescriptions::loadCustomDescriptions(configPath);

    // Load saved settings
    loadSettings();

    // Start update timer (1 second interval)
    updateTimer->start(1000);

    // Initial update
    updateSensorData();

    statusBar()->showMessage("Ready");
    initialized = true;
}

MainWindow::~MainWindow()
{
    if (!initialized)
        return;
    // Save current settings before exit
    saveSettings();
    // The daemon (nonInteractiveMode) NEVER restores auto mode on its own. It
    // is a failsafe that keeps the fans at the saved preset until the GUI
    // takes over. On GUI takeover the GUI immediately applies its own settings,
    // so restoring auto here would only cause a brief fan-speed blip. On system
    // shutdown the fans are powered off anyway, so restoring auto is pointless.
    //
    // The interactive GUI, however, SHOULD restore auto mode when it exits:
    // once the user closes the GUI, nothing is controlling the fans anymore, so
    // falling back to the firmware's automatic curve is the correct behavior.
    if (!nonInteractiveMode) {
        restoreAutoMode();
    }
    // Release the single-writer lock so another instance can take over.
    releaseFanControlLock();
}

void MainWindow::setupUI()
{
    setWindowTitle("Fan Control");
    resize(800, 600);

    // Pass Mac model to temperature panel for sensor description lookup
    tempPanel->setMacModel(smcInterface->getMacModel());

    // Create central widget
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    // Create main horizontal layout
    QHBoxLayout *mainLayout = new QHBoxLayout(centralWidget);

    // Left panel container
    QWidget *leftPanelContainer = new QWidget(this);
    QVBoxLayout *leftContainerLayout = new QVBoxLayout(leftPanelContainer);
    leftContainerLayout->setContentsMargins(0, 0, 0, 0);

    // Add title for fan section
    QLabel *fanTitle = new QLabel("<b>Fan Controls</b>", this);
    fanTitle->setStyleSheet("font-size: 14px; padding: 10px;");
    leftContainerLayout->addWidget(fanTitle);

    // Create scroll area for fan controls
    QScrollArea *fanScrollArea = new QScrollArea(this);
    fanScrollArea->setWidgetResizable(true);
    fanScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    fanScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Fan controls content widget
    QWidget *fanContentWidget = new QWidget();
    QVBoxLayout *fanLayout = new QVBoxLayout(fanContentWidget);
    fanLayout->setSpacing(10);
    fanLayout->setContentsMargins(10, 10, 10, 10);

    // Create fan control widgets for SMC fans
    QVector<FanInfo> smcFans = smcInterface->getFans();
    QString macModel = smcInterface->getMacModel();
    for (const FanInfo& fan : smcFans) {
        FanControlWidget *fanWidget = new FanControlWidget(fan, this);
        fanWidget->setMacModel(macModel);  // Set Mac model for sensor descriptions
        fanWidgets.append(fanWidget);
        fanSources.append(FAN_SOURCE_SMC);
        fanSourceIndices.append(fan.index - 1);  // SMC uses 1-based index
        fanLayout->addWidget(fanWidget);

        // Initialize sensor-based settings
        SensorBasedSettings settings = {false, -1, 40, 80};
        sensorSettings.append(settings);

        // Connect fan widget signals
        connect(fanWidget, &FanControlWidget::manualModeRequested,
                this, &MainWindow::onManualModeRequested);
        connect(fanWidget, &FanControlWidget::targetRPMChanged,
                this, &MainWindow::onTargetRPMChanged);
        connect(fanWidget, &FanControlWidget::sensorBasedModeChanged,
                this, &MainWindow::onSensorBasedModeChanged);
    }

    // Create fan control widgets for HWMon fans
    QVector<HWMonFan> hwmonFans = hwmonInterface->getFans();
    for (int i = 0; i < hwmonFans.size(); i++) {
        const HWMonFan& hwFan = hwmonFans[i];

        // Convert HWMonFan to FanInfo
        FanInfo fan;
        fan.index = fanWidgets.size() + 1;  // Sequential index
        fan.label = hwFan.label;
        fan.currentRPM = hwFan.currentRPM;
        fan.targetRPM = hwFan.currentRPM;
        fan.minRPM = hwFan.minRPM;
        fan.maxRPM = hwFan.maxRPM;
        fan.isManual = hwFan.isManual;
        fan.sysfsPath = hwFan.devicePath;

        FanControlWidget *fanWidget = new FanControlWidget(fan, this);
        fanWidgets.append(fanWidget);
        fanSources.append(FAN_SOURCE_HWMON);
        fanSourceIndices.append(i);  // HWMon index
        fanLayout->addWidget(fanWidget);

        // Initialize sensor-based settings
        SensorBasedSettings settings = {false, -1, 40, 80};
        sensorSettings.append(settings);

        // Connect fan widget signals
        connect(fanWidget, &FanControlWidget::manualModeRequested,
                this, &MainWindow::onManualModeRequested);
        connect(fanWidget, &FanControlWidget::targetRPMChanged,
                this, &MainWindow::onTargetRPMChanged);
        connect(fanWidget, &FanControlWidget::sensorBasedModeChanged,
                this, &MainWindow::onSensorBasedModeChanged);
    }

    // Initialize sensor list in fan widgets
    updateSensorListInFanWidgets();
    fanLayout->addStretch();

    // Add content to scroll area
    fanScrollArea->setWidget(fanContentWidget);
    leftContainerLayout->addWidget(fanScrollArea);

    // Right panel: Temperature display
    QWidget *rightPanel = new QWidget(this);
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(tempPanel);

    // Add panels to main layout
    mainLayout->addWidget(leftPanelContainer, 1);
    mainLayout->addWidget(rightPanel, 1);

    // Create status bar
    statusBar()->showMessage("Initializing...");
}

void MainWindow::createMenuBar()
{
    // File menu
    QMenu *fileMenu = menuBar()->addMenu("&File");

    QMenu *themeMenu = fileMenu->addMenu("&Theme");
    QActionGroup *themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);

    QAction *lightThemeAction = new QAction("&Light Mode", this);
    lightThemeAction->setCheckable(true);
    connect(lightThemeAction, &QAction::triggered, this, &MainWindow::setLightTheme);
    themeGroup->addAction(lightThemeAction);
    themeMenu->addAction(lightThemeAction);

    QAction *darkThemeAction = new QAction("&Dark Mode", this);
    darkThemeAction->setCheckable(true);
    connect(darkThemeAction, &QAction::triggered, this, &MainWindow::setDarkTheme);
    themeGroup->addAction(darkThemeAction);
    themeMenu->addAction(darkThemeAction);

    if (currentTheme == ThemeDark) {
        darkThemeAction->setChecked(true);
    } else {
        lightThemeAction->setChecked(true);
    }

    QAction *exitAction = new QAction("E&xit", this);
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QMainWindow::close);
    fileMenu->addAction(exitAction);

    // Presets menu
    QMenu *presetsMenu = menuBar()->addMenu("&Presets");

    QAction *savePresetAction = new QAction("&Save Preset...", this);
    savePresetAction->setShortcut(QKeySequence("Ctrl+S"));
    connect(savePresetAction, &QAction::triggered, this, &MainWindow::savePreset);
    presetsMenu->addAction(savePresetAction);

    QAction *loadPresetAction = new QAction("&Load Preset...", this);
    loadPresetAction->setShortcut(QKeySequence("Ctrl+L"));
    connect(loadPresetAction, &QAction::triggered, this, &MainWindow::loadPreset);
    presetsMenu->addAction(loadPresetAction);

    QAction *deletePresetAction = new QAction("&Delete Preset...", this);
    connect(deletePresetAction, &QAction::triggered, this, &MainWindow::deletePreset);
    presetsMenu->addAction(deletePresetAction);

    presetsMenu->addSeparator();

    QAction *exportPresetAction = new QAction("E&xport Preset...", this);
    exportPresetAction->setShortcut(QKeySequence("Ctrl+E"));
    connect(exportPresetAction, &QAction::triggered, this, &MainWindow::exportPreset);
    presetsMenu->addAction(exportPresetAction);

    QAction *importPresetAction = new QAction("&Import Preset...", this);
    importPresetAction->setShortcut(QKeySequence("Ctrl+I"));
    connect(importPresetAction, &QAction::triggered, this, &MainWindow::importPreset);
    presetsMenu->addAction(importPresetAction);

    // Help menu
    QMenu *helpMenu = menuBar()->addMenu("&Help");

    QAction *copyDebugAction = new QAction("Copy &Debug Log to Clipboard", this);
    copyDebugAction->setShortcut(QKeySequence("Ctrl+Shift+D"));
    connect(copyDebugAction, &QAction::triggered, this, &MainWindow::copyDebugLogToClipboard);
    helpMenu->addAction(copyDebugAction);

    helpMenu->addSeparator();

    QAction *aboutAction = new QAction("&About", this);
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, "About Mac Fan Control",
                         "Mac Fan Control for Linux\n\n"
                         "A Qt5 application for controlling fans on Linux.\n\n"
                         "Supports Apple SMC fans (applesmc) and hwmon devices (AMD/NVIDIA GPUs, etc.).\n\n"
                         "Features:\n"
                         "- Real-time fan speed monitoring\n"
                         "- Manual and automatic fan control\n"
                         "- Temperature sensor monitoring\n"
                         "- Sensor-based automatic control\n"
                         "- Save and load presets\n"
                         "- Settings persistence\n"
                         "- Support for multiple fan types (SMC + hwmon)\n"
                         "- Safety enforcement (min/max RPM limits)");
    });
    helpMenu->addAction(aboutAction);
}

void MainWindow::connectSignals()
{
    // Connect timer to update function
    connect(updateTimer, &QTimer::timeout, this, &MainWindow::updateSensorData);

    // Connect SMC interface signals
    connect(smcInterface, &SMCInterface::error, this, &MainWindow::showError);
    connect(smcInterface, &SMCInterface::warning, this, &MainWindow::showWarning);
    connect(tempPanel, &TemperaturePanel::unitChanged, this, &MainWindow::onTemperatureUnitChanged);
}

void MainWindow::setLightTheme()
{
    applyTheme(ThemeLight);
}

void MainWindow::setDarkTheme()
{
    applyTheme(ThemeDark);
}

void MainWindow::applyTheme(ThemeMode theme)
{
    currentTheme = theme;
    if (theme == ThemeDark) {
        qApp->setStyleSheet(
            "QWidget { background-color: #2b2b2b; color: #e8e8e8; }"
            "QMenuBar, QMenu, QStatusBar { background-color: #212121; color: #e8e8e8; }"
            "QMenu::item:selected { background-color: #3a6ea5; color: #ffffff; }"
            "QPushButton, QComboBox, QSpinBox, QLineEdit, QTextEdit { background-color: #353535; color: #e8e8e8; border: 1px solid #555555; }"
            "QToolTip { color: #ffffff; background-color: #2a2a2a; border: 1px solid #ffffff; }"
        );
        QPalette darkPalette;
        darkPalette.setColor(QPalette::Window, QColor(43, 43, 43));
        darkPalette.setColor(QPalette::WindowText, Qt::white);
        darkPalette.setColor(QPalette::Base, QColor(35, 35, 35));
        darkPalette.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
        darkPalette.setColor(QPalette::ToolTipBase, Qt::white);
        darkPalette.setColor(QPalette::ToolTipText, Qt::white);
        darkPalette.setColor(QPalette::Text, Qt::white);
        darkPalette.setColor(QPalette::Button, QColor(53, 53, 53));
        darkPalette.setColor(QPalette::ButtonText, Qt::white);
        darkPalette.setColor(QPalette::BrightText, Qt::red);
        darkPalette.setColor(QPalette::Link, QColor(42, 130, 218));
        darkPalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        darkPalette.setColor(QPalette::HighlightedText, Qt::white);
        qApp->setPalette(darkPalette);
    } else {
        qApp->setStyleSheet(defaultStyleSheet);
        qApp->setPalette(defaultPalette);
    }

    // Force all widgets to refresh after theme change.
    qApp->style()->unpolish(qApp);
    qApp->style()->polish(qApp);
    for (QWidget *widget : QApplication::allWidgets()) {
        if (widget) {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
            widget->update();
        }
    }
}

void MainWindow::updateSensorData()
{
    // The boot daemon applies its preset once at startup and then only
    // re-asserts fans whose actual sysfs state has drifted (see
    // ensureBootPresetApplied()). Sensor-based fan speeds are still
    // recomputed every tick below, which is required for the ramp curve.
    if (nonInteractiveMode && !activeBootPreset.isEmpty()) {
        ensureBootPresetApplied();
    }
    // Get temperature readings from both sources
    QVector<TempSensor> smcTemps = smcInterface->getTemperatures();
    QVector<HWMonSensor> hwmonSensors = hwmonInterface->getTemperatures();

    // Convert hwmon sensors to TempSensor format
    QVector<TempSensor> temps = smcTemps;
    for (const HWMonSensor& hwSensor : hwmonSensors) {
        TempSensor sensor;
        sensor.index = hwSensor.index;
        sensor.label = hwSensor.label;
        sensor.temperature = hwSensor.temperature;
        sensor.sysfsPath = hwSensor.devicePath;
        temps.append(sensor);
    }

    // Update all fan RPMs
    for (int i = 0; i < fanWidgets.size(); i++) {
        FanSource source = fanSources[i];
        int sourceIndex = fanSourceIndices[i];
        int rpm = -1;

        if (source == FAN_SOURCE_SMC) {
            rpm = smcInterface->getFanCurrentRPM(sourceIndex);
        } else if (source == FAN_SOURCE_HWMON) {
            rpm = hwmonInterface->getFanCurrentRPM(sourceIndex);
        }

        if (rpm >= 0) {  // Valid reading
            fanWidgets[i]->setCurrentRPM(rpm);
        }

        // Update sensor-based fans
        if (sensorSettings[i].enabled && sensorSettings[i].sensorIndex >= 0) {
            // Find the temperature for the selected sensor
            for (const TempSensor& sensor : temps) {
                if (sensor.index == sensorSettings[i].sensorIndex) {
                    fanWidgets[i]->updateSensorBasedSpeed(sensor.temperature);
                    break;
                }
            }
        }
    }

    // Update temperature panel
    tempPanel->updateTemperatures(temps);

    // Update sensor list in fan widgets periodically (every 10 seconds)
    static int updateCount = 0;
    if (++updateCount % 10 == 0) {
        updateSensorListInFanWidgets();
    }

    // Update status bar
    statusBar()->showMessage(QString("Last update: %1").arg(QTime::currentTime().toString("hh:mm:ss")));
}

void MainWindow::onTemperatureUnitChanged(bool useFahrenheit)
{
    for (FanControlWidget* fanWidget : fanWidgets) {
        fanWidget->setUseFahrenheit(useFahrenheit);
    }

    // Refresh sensor labels so the sensor dropdown unit labels update
    updateSensorListInFanWidgets();
}

void MainWindow::showError(const QString& message)
{
    // Show error in status bar
    statusBar()->showMessage("Error: " + message, 5000);

    // Log to stderr
    qWarning() << "SMC Error:" << message;

    // For critical permission errors, show dialog
    if (message.contains("permission", Qt::CaseInsensitive) ||
        message.contains("root", Qt::CaseInsensitive)) {
        if (!nonInteractiveMode) {
            QMessageBox::critical(this, "Permission Error",
                                "Cannot write to SMC interface.\n\n" + message +
                                "\n\nPlease run with: sudo macsfancontrol");
        }
    }
}

void MainWindow::showWarning(const QString& message)
{
    // Show warning in status bar
    statusBar()->showMessage("Warning: " + message, 3000);

    // Log to stderr
    qWarning() << "SMC Warning:" << message;
}

void MainWindow::copyDebugLogToClipboard()
{
    // Forward declaration — defined in main.cpp
    extern QStringList getDebugLog();

    QStringList lines;
    lines << "=== Mac Fan Control Debug Log ===";
    lines << QString("Timestamp: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    lines << QString("Mac Model:  %1").arg(smcInterface->getMacModel());
    lines << "";

    // SMC fans
    lines << "--- SMC Fans ---";
    for (const FanInfo& fan : smcInterface->getFans()) {
        lines << QString("  Fan %1 (%2): %3 RPM  [min:%4  max:%5  manual:%6]  path:%7")
                     .arg(fan.index).arg(fan.label).arg(fan.currentRPM)
                     .arg(fan.minRPM).arg(fan.maxRPM)
                     .arg(fan.isManual ? "yes" : "no")
                     .arg(fan.sysfsPath);
    }

    // HWMon fans
    lines << "";
    lines << "--- HWMon Fans ---";
    for (const HWMonFan& fan : hwmonInterface->getFans()) {
        lines << QString("  %1/%2: %3 RPM  [min:%4  max:%5  manual:%6]  path:%7")
                     .arg(fan.deviceName).arg(fan.label).arg(fan.currentRPM)
                     .arg(fan.minRPM).arg(fan.maxRPM)
                     .arg(fan.isManual ? "yes" : "no")
                     .arg(fan.devicePath);
    }

    // SMC temperatures (live read)
    lines << "";
    lines << "--- SMC Temperatures ---";
    {
        const QString macModel = smcInterface->getMacModel();
        for (const TempSensor& sensor : smcInterface->getTemperatures()) {
            QString description = SensorDescriptions::getDescription(sensor.label, macModel);
            QString nameField = QString("%1 (%2)").arg(sensor.label, -6).arg(description);
            lines << QString("  %1: %2 °C  (%3)")
                         .arg(nameField, -36)
                         .arg(sensor.temperature / 1000.0, 5, 'f', 1)
                         .arg(sensor.sysfsPath);
        }
    }

    // HWMon temperatures (live read)
    lines << "";
    lines << "--- HWMon Temperatures ---";
    for (const HWMonSensor& sensor : hwmonInterface->getTemperatures()) {
        lines << QString("  %1/%2: %3 °C  (%4)")
                     .arg(sensor.deviceName).arg(sensor.label, -20)
                     .arg(sensor.temperature / 1000.0, 5, 'f', 1)
                     .arg(sensor.devicePath);
    }

    // Saved presets
    lines << "";
    lines << "--- Saved Presets ---";
    {
        QSettings settings("macsfancontrol", "macsfancontrol-qt");
        settings.beginGroup("Presets");
        QStringList presetNames = settings.childGroups();
        if (presetNames.isEmpty()) {
            lines << "  (none)";
        } else {
            auto modeStr = [](int m) -> QString {
                switch (m) {
                case MODE_AUTO:         return "auto";
                case MODE_MANUAL:       return "manual";
                case MODE_SENSOR_BASED: return "sensor-based";
                default:                return QString("unknown(%1)").arg(m);
                }
            };
            for (const QString& name : presetNames) {
                settings.beginGroup(name);
                int fanCount = settings.value("fanCount", 0).toInt();
                lines << QString("  Preset: %1  (%2 fans)").arg(name).arg(fanCount);
                for (int i = 0; i < fanCount; i++) {
                    settings.beginGroup(QString("Fan%1").arg(i));
                    int mode       = settings.value("mode", MODE_AUTO).toInt();
                    int targetRPM  = settings.value("targetRPM", 0).toInt();
                    int sensorIdx  = settings.value("sensorIndex", -1).toInt();
                    int minTemp    = settings.value("minTemp", 0).toInt();
                    int maxTemp    = settings.value("maxTemp", 0).toInt();
                    QString entry  = QString("    Fan%1: mode=%2").arg(i).arg(modeStr(mode));
                    if (mode == MODE_MANUAL)
                        entry += QString("  targetRPM=%1").arg(targetRPM);
                    if (mode == MODE_SENSOR_BASED)
                        entry += QString("  sensor=%1  minTemp=%2  maxTemp=%3")
                                     .arg(sensorIdx).arg(minTemp).arg(maxTemp);
                    lines << entry;
                    settings.endGroup();
                }
                settings.endGroup();
            }
        }
        settings.endGroup();
    }

    // Application log captured since startup
    lines << "";
    lines << "--- Application Log ---";
    lines << getDebugLog();

    // Raw sysfs dump
    auto readSysfsValue = [](const QString& path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return "(unreadable)";
        QString v = QString(f.readLine()).trimmed();
        f.close();
        return v;
    };

    lines << "";
    lines << "=== Raw Sysfs Dump ===";

    // --- hwmon devices ---
    lines << "";
    lines << "--- /sys/class/hwmon ---";
    QDir hwmonDir("/sys/class/hwmon");
    if (hwmonDir.exists()) {
        QStringList hwmonDevs = hwmonDir.entryList(QStringList() << "hwmon*", QDir::Dirs, QDir::Name);
        for (const QString& dev : hwmonDevs) {
            QString devPath = "/sys/class/hwmon/" + dev;
            QString name = readSysfsValue(devPath + "/name");
            bool isTempDup = HWMonInterface::smcDuplicateDevices.contains(name);

            lines << QString("  %1  name=%2%3")
                         .arg(dev, -8).arg(name)
                         .arg(isTempDup ? "  [SMC duplicate: temps suppressed, fans kept]" : "");

            QDir d(devPath);

            // Fan files: input, min, max, label
            QStringList fanFiles = d.entryList(
                QStringList() << "fan*_input" << "fan*_min" << "fan*_max" << "fan*_label",
                QDir::Files, QDir::Name);
            for (const QString& f : fanFiles)
                lines << QString("    %1 = %2").arg(f, -22).arg(readSysfsValue(devPath + "/" + f));

            // PWM files: value and enable mode
            QStringList pwmFiles = d.entryList(QStringList() << "pwm*", QDir::Files, QDir::Name);
            for (const QString& f : pwmFiles) {
                QString val = readSysfsValue(devPath + "/" + f);
                QString note;
                if (f.endsWith("_enable")) {
                    if (val == "0") note = "  (disabled)";
                    else if (val == "1") note = "  (manual)";
                    else if (val == "2") note = "  (auto)";
                }
                lines << QString("    %1 = %2%3").arg(f, -22).arg(val).arg(note);
            }

            // Temp files: input and label
            QStringList tempFiles = d.entryList(
                QStringList() << "temp*_input" << "temp*_label",
                QDir::Files, QDir::Name);
            for (const QString& f : tempFiles) {
                QString val = readSysfsValue(devPath + "/" + f);
                QString note = (isTempDup && f.endsWith("_input")) ? "  (suppressed)" : "";
                lines << QString("    %1 = %2%3").arg(f, -22).arg(val).arg(note);
            }

            if (fanFiles.isEmpty() && pwmFiles.isEmpty() && tempFiles.isEmpty())
                lines << "    (no fan/pwm/temp files)";
        }
    } else {
        lines << "  /sys/class/hwmon not found";
    }

    // --- applesmc raw fan + temp count ---
    lines << "";
    lines << QString("--- AppSMC: %1 ---").arg(smcInterface->getBasePath());
    QDir smcDir(smcInterface->getBasePath());
    if (smcDir.exists()) {
        QStringList smcFanFiles = smcDir.entryList(QStringList() << "fan*", QDir::Files, QDir::Name);
        for (const QString& f : smcFanFiles)
            lines << QString("  %1 = %2").arg(f, -25)
                         .arg(readSysfsValue(smcInterface->getBasePath() + "/" + f));

        int tempCount = smcDir.entryList(QStringList() << "temp*_input", QDir::Files).size();
        lines << QString("  (%1 temp*_input files — see SMC Temperatures section above)").arg(tempCount);
    } else {
        lines << "  Path not found";
    }

    QApplication::clipboard()->setText(lines.join('\n'));
    statusBar()->showMessage("Debug log copied to clipboard", 3000);
}

void MainWindow::restoreAutoMode()
{
    // Restore all fans to automatic mode
    for (int i = 0; i < fanWidgets.size(); i++) {
        FanSource source = fanSources[i];
        int sourceIndex = fanSourceIndices[i];

        if (source == FAN_SOURCE_SMC) {
            smcInterface->setFanManualMode(sourceIndex, false);
        } else if (source == FAN_SOURCE_HWMON) {
            hwmonInterface->setFanManualMode(sourceIndex, false);
        }
    }
}


// --- Single-writer fan-control lock -------------------------------------
// Prevents multiple instances (e.g. a boot daemon and the interactive GUI,
// or two daemons from leftover systemd units) from writing to the same
// sysfs fan files at the same time. Without this, a daemon reapplying its
// preset every second silently overwrites whatever the user sets in the GUI.
//
// Strategy: an exclusive flock() on /run/macsfancontrol.lock, with the
// holder's PID written into the file. The interactive GUI is allowed to
// take over: it asks the current holder to quit (SIGTERM) and then acquires
// the lock. A daemon never displaces an existing holder.
static int readLockHolderPid(int fd)
{
    char buf[32] = {0};
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return -1;
    bool ok = false;
    int pid = QByteArray(buf).trimmed().toInt(&ok);
    return ok ? pid : -1;
}
static void writeLockHolderPid(int fd)
{
    if (ftruncate(fd, 0) == -1)
        return;
    lseek(fd, 0, SEEK_SET);
    QByteArray pid = QByteArray::number(QCoreApplication::applicationPid());
    ssize_t written = ::write(fd, pid.constData(), pid.size());
    (void)written;
}

bool MainWindow::acquireFanControlLock(bool allowTakeover)
{
    const char *lockPath = "/run/macsfancontrol.lock";
    int fd = ::open(lockPath, O_RDWR | O_CREAT, 064);
    if (fd < 0) {
        qWarning() << "Cannot open fan control lock" << lockPath << ":" << strerror(errno);
        // If /run is not writable (unlikely for root), continue without the
        // lock rather than blocking fan control entirely.
        return true;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        fanControlLockFd = fd;
        writeLockHolderPid(fd);
        return true;
    }
    // Lock is held by someone else (e.g. the boot daemon).
    int holderPid = readLockHolderPid(fd);
    if (!allowTakeover) {
        // A daemon must never displace another live instance.
        ::close(fd);
        return false;
    }
    // GUI takeover: the boot daemon is only a failsafe that ramps fans up at
    // boot when the machine is unattended. Once the user launches the GUI,
    // the GUI takes precedence. Politely ask the daemon to stand down (it
    // restores auto mode and exits on SIGTERM), then acquire the lock.
    if (holderPid > 0 && holderPid != QCoreApplication::applicationPid()) {
        if (::kill(holderPid, SIGTERM) == 0) {
            for (int i = 0; i < 20; ++i) {  // up to ~2s
                if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                    fanControlLockFd = fd;
                    writeLockHolderPid(fd);
                    return true;
                }
                usleep(100 * 1000);
            }
        }
    }
    // Last resort: blocking acquire (holder died without releasing).
    if (flock(fd, LOCK_EX) == 0) {
        fanControlLockFd = fd;
        writeLockHolderPid(fd);
        return true;
    }
    ::close(fd);
    return false;
}

void MainWindow::releaseFanControlLock()
{
    if (fanControlLockFd >= 0) {
        if (ftruncate(fanControlLockFd, 0) == 0)
            writeLockHolderPid(fanControlLockFd);
        flock(fanControlLockFd, LOCK_UN);
        ::close(fanControlLockFd);
        fanControlLockFd = -1;
    }
    hasFanControlLock = false;
}
// ------------------------------------------------------------------------


void MainWindow::onManualModeRequested(int fanWidgetIndex, bool enable)
{
    if (fanWidgetIndex < 0 || fanWidgetIndex >= fanWidgets.size()) {
        return;
    }

    FanSource source = fanSources[fanWidgetIndex];
    int sourceIndex = fanSourceIndices[fanWidgetIndex];

    if (source == FAN_SOURCE_SMC) {
        smcInterface->setFanManualMode(sourceIndex, enable);
    } else if (source == FAN_SOURCE_HWMON) {
        hwmonInterface->setFanManualMode(sourceIndex, enable);
    }
}

void MainWindow::onTargetRPMChanged(int fanWidgetIndex, int rpm)
{
    if (fanWidgetIndex < 0 || fanWidgetIndex >= fanWidgets.size()) {
        return;
    }

    FanSource source = fanSources[fanWidgetIndex];
    int sourceIndex = fanSourceIndices[fanWidgetIndex];

    if (source == FAN_SOURCE_SMC) {
        smcInterface->setFanSpeed(sourceIndex, rpm);
    } else if (source == FAN_SOURCE_HWMON) {
        hwmonInterface->setFanSpeed(sourceIndex, rpm);
    }
}

void MainWindow::onSensorBasedModeChanged(int fanWidgetIndex, bool enable, int sensorIndex, int minTemp, int maxTemp)
{
    if (fanWidgetIndex < 0 || fanWidgetIndex >= sensorSettings.size()) {
        return;
    }

    // Update sensor-based settings
    sensorSettings[fanWidgetIndex].enabled = enable;
    sensorSettings[fanWidgetIndex].sensorIndex = sensorIndex;
    sensorSettings[fanWidgetIndex].minTemp = minTemp;
    sensorSettings[fanWidgetIndex].maxTemp = maxTemp;

    // Log the change
    if (enable) {
        qDebug() << "Fan" << fanWidgetIndex << "sensor-based mode enabled:"
                 << "sensor" << sensorIndex
                 << "temp range" << minTemp << "-" << maxTemp << "°C";
    }
}

void MainWindow::updateSensorListInFanWidgets()
{
    // Get current temperature sensors from both sources
    QVector<TempSensor> smcTemps = smcInterface->getTemperatures();
    QVector<HWMonSensor> hwmonSensors = hwmonInterface->getTemperatures();

    // Convert hwmon sensors to TempSensor format
    QVector<TempSensor> temps = smcTemps;
    for (const HWMonSensor& hwSensor : hwmonSensors) {
        TempSensor sensor;
        sensor.index = hwSensor.index;
        sensor.label = hwSensor.label;
        sensor.temperature = hwSensor.temperature;
        sensor.sysfsPath = hwSensor.devicePath;
        temps.append(sensor);
    }

    // Update sensor list in each fan widget
    for (FanControlWidget* fanWidget : fanWidgets) {
        fanWidget->setSensorList(temps);
    }
}

void MainWindow::saveSettings()
{
    QSettings settings("macsfancontrol", "macsfancontrol-qt");
    settings.beginGroup("LastSession");
    settings.setValue("fanCount", fanWidgets.size());
    settings.setValue("useFahrenheit", tempPanel->isUsingFahrenheit());
    settings.setValue("canonicalCelsius", true);
    settings.setValue("theme", currentTheme == ThemeDark ? "dark" : "light");
    for (int i = 0; i < fanWidgets.size(); i++) {
        settings.beginGroup(QString("Fan%1").arg(i));
        settings.setValue("mode", static_cast<int>(fanWidgets[i]->getCurrentMode()));
        settings.setValue("targetRPM", fanWidgets[i]->getTargetRPM());
        settings.setValue("sensorIndex", fanWidgets[i]->getSelectedSensorIndex());
        settings.setValue("minTemp", fanWidgets[i]->getMinTemp());
        settings.setValue("maxTemp", fanWidgets[i]->getMaxTemp());
        settings.endGroup();
    }
    settings.endGroup();
    qDebug() << "Settings saved";
}

void MainWindow::loadSettings()
{
    QSettings settings("macsfancontrol", "macsfancontrol-qt");
    settings.beginGroup("LastSession");
    int savedFanCount = settings.value("fanCount", 0).toInt();
    bool savedUseFahrenheit = settings.value("useFahrenheit", false).toBool();
    bool canonicalCelsius = settings.value("canonicalCelsius", false).toBool();
    QString savedTheme = settings.value("theme", "light").toString();
    if (savedTheme == "dark") {
        applyTheme(ThemeDark);
    } else {
        applyTheme(ThemeLight);
    }

    if (savedFanCount != fanWidgets.size()) {
        qDebug() << "Fan count mismatch, skipping settings load";
        settings.endGroup();
        return;
    }

    tempPanel->setUseFahrenheit(savedUseFahrenheit);
    onTemperatureUnitChanged(savedUseFahrenheit);

    // The daemon must NOT apply the last session's fan settings: it should
    // only ever apply the preset marked "Launch at Boot" (loaded later via
    // loadBootPreset()). Applying the last session here would make the daemon
    // drive the fans with the last loaded preset regardless of the boot flag.
    if (!nonInteractiveMode) {
        for (int i = 0; i < fanWidgets.size(); i++) {
            settings.beginGroup(QString("Fan%1").arg(i));
            FanMode mode = static_cast<FanMode>(settings.value("mode", MODE_AUTO).toInt());
            int targetRPM = settings.value("targetRPM", 2000).toInt();
            int sensorIndex = settings.value("sensorIndex", -1).toInt();
            int minTemp = settings.value("minTemp", 40).toInt();
            int maxTemp = settings.value("maxTemp", 80).toInt();
            if (!canonicalCelsius && (minTemp > 100 || maxTemp > 120)) {
                minTemp = qRound((minTemp - 32.0) * 5.0 / 9.0);
                maxTemp = qRound((maxTemp - 32.0) * 5.0 / 9.0);
            }
            applyFanSettings(i, mode, targetRPM, sensorIndex, minTemp, maxTemp);
            settings.endGroup();
        }
    }

    settings.endGroup();
    qDebug() << "Settings loaded";
}

void MainWindow::setPresetLaunchAtBoot(const QString& presetName, bool launchAtBoot)
{
    QSettings settings("macsfancontrol", "macsfancontrol-qt");
    settings.beginGroup("Presets");
    if (launchAtBoot) {
        // Marking a preset for boot overrides/deactivates any other preset
        // that was previously marked. Unmarking (launchAtBoot == false) must
        // NOT clear other presets' flags, so a marking persists regardless of
        // cancelling, selecting, or loading another preset.
        QStringList presets = settings.childGroups();
        for (const QString& otherPreset : presets) {
            if (otherPreset == presetName) {
                continue;
            }

            settings.beginGroup(otherPreset);
            settings.setValue("launchAtBoot", false);
            settings.endGroup();
        }
    }

    settings.beginGroup(presetName);
    settings.setValue("launchAtBoot", launchAtBoot);
    settings.endGroup();

    settings.endGroup();
}

bool MainWindow::createUserAutostart(const QString& presetName)
{
    Q_UNUSED(presetName);
    // Boot launch is handled by the packaged systemd service
    // (macsfancontrol.service, WantedBy=multi-user.target). The service runs
    // /usr/bin/macsfancontrol-boot, which execs the daemon; the daemon reads
    // the preset marked "launchAtBoot" from QSettings and applies it.
    //
    // Nothing to do here beyond ensuring the service is enabled. If it is not
    // (e.g. the package was installed without systemd), warn the user.
    if (QFile::exists("/run/systemd/system")) {
        QProcess proc;
        proc.start("systemctl", QStringList() << "is-enabled" << "macsfancontrol.service");
        proc.waitForFinished(2000);
        QString out = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
        if (out != "enabled") {
            qWarning() << "macsfancontrol.service is not enabled; boot launch will not work.";
        }
    }
    return true;
}

void MainWindow::removeUserAutostart()
{
    qWarning() << "No action needed; boot launch is handled by the system service";
}

void MainWindow::savePresetToSettings(const QString& presetName, bool launchAtBoot)
{
    setPresetLaunchAtBoot(presetName, launchAtBoot);

    QSettings settings("macsfancontrol", "macsfancontrol-qt");
    settings.beginGroup("Presets");
    settings.beginGroup(presetName);

    settings.setValue("fanCount", fanWidgets.size());
    settings.setValue("launchAtBoot", launchAtBoot);
    settings.setValue("canonicalCelsius", true);

    for (int i = 0; i < fanWidgets.size(); i++) {
        settings.beginGroup(QString("Fan%1").arg(i));
        settings.setValue("mode", static_cast<int>(fanWidgets[i]->getCurrentMode()));
        settings.setValue("targetRPM", fanWidgets[i]->getTargetRPM());
        settings.setValue("sensorIndex", fanWidgets[i]->getSelectedSensorIndex());
        settings.setValue("minTemp", fanWidgets[i]->getMinTemp());
        settings.setValue("maxTemp", fanWidgets[i]->getMaxTemp());
        settings.endGroup();
    }
    settings.endGroup();
    settings.endGroup();
    qDebug() << "Preset saved:" << presetName;
}

bool MainWindow::loadPresetFromSettings(const QString& presetName)
{
    QSettings settings("macsfancontrol", "macsfancontrol-qt");
    settings.beginGroup("Presets");
    if (!settings.childGroups().contains(presetName)) {
        if (!nonInteractiveMode) {
            QMessageBox::warning(this, "Preset Error", QString("Preset '%1' does not exist.").arg(presetName));
        } else {
            qWarning() << "Preset" << presetName << "does not exist.";
        }
        settings.endGroup();
        return false;
    }
    settings.beginGroup(presetName);
    int savedFanCount = settings.value("fanCount", 0).toInt();
    if (savedFanCount != fanWidgets.size()) {
        if (!nonInteractiveMode) {
            QMessageBox::warning(this, "Preset Error",
                               "This preset was saved with a different fan configuration.");
        } else {
            qWarning() << "Preset" << presetName << "was saved with a different fan configuration.";
        }
        settings.endGroup();
        settings.endGroup();
        return false;
    }
    bool launchAtBoot = settings.value("launchAtBoot", false).toBool();
    bool canonicalCelsius = settings.value("canonicalCelsius", false).toBool();
    if (launchAtBoot) {
        // Use per-user autostart for all users (systemd installer removed)
        createUserAutostart(presetName);
    }

    for (int i = 0; i < fanWidgets.size(); i++) {
        settings.beginGroup(QString("Fan%1").arg(i));
        FanMode mode = static_cast<FanMode>(settings.value("mode", MODE_AUTO).toInt());
        int targetRPM = settings.value("targetRPM", 2000).toInt();
        int sensorIndex = settings.value("sensorIndex", -1).toInt();
        int minTemp = settings.value("minTemp", 40).toInt();
        int maxTemp = settings.value("maxTemp", 80).toInt();
        if (!canonicalCelsius && (minTemp > 100 || maxTemp > 120)) {
            minTemp = qRound((minTemp - 32.0) * 5.0 / 9.0);
            maxTemp = qRound((maxTemp - 32.0) * 5.0 / 9.0);
        }
        applyFanSettings(i, mode, targetRPM, sensorIndex, minTemp, maxTemp);
        settings.endGroup();
    }
    settings.endGroup();
    settings.endGroup();
    qDebug() << "Preset loaded:" << presetName;
    return true;
}
bool MainWindow::loadPresetByName(const QString& presetName)
{
    if (loadPresetFromSettings(presetName)) {
        activeBootPreset = presetName;
        bootPresetAppliedOnce = true;
        return true;
    }
    return false;
}

void MainWindow::ensureBootPresetApplied()
{
    // In steady state the applesmc/hwmon drivers hold whatever control values
    // were last written, so re-parsing the preset and rewriting every fan's
    // sysfs files once per second is pure waste (and spawns a `systemctl`
    // subprocess each time via createUserAutostart()). The preset is loaded
    // once (bootPresetAppliedOnce, set by loadPresetByName) and fans are only
    // re-asserted here when their ACTUAL sysfs state has drifted from the
    // intended values (e.g. applesmc driver re-insert, an external tool
    // flipping fanN_manual back to auto, a transient write failure). This
    // keeps the "Load at Boot" self-healing guarantee with zero steady-state
    // writes, config reads, subprocesses, or log spam.
    for (int i = 0; i < fanWidgets.size(); i++) {
        FanMode mode = fanWidgets[i]->getCurrentMode();
        bool wantManual = (mode == MODE_MANUAL || mode == MODE_SENSOR_BASED);

        FanSource source = fanSources[i];
        int sourceIndex = fanSourceIndices[i];
        bool isManual = false;
        bool hasTarget = false;
        int targetRPM = 0;

        bool ok = (source == FAN_SOURCE_SMC)
                ? smcInterface->getFanControlState(sourceIndex, isManual, hasTarget, targetRPM)
                : hwmonInterface->getFanControlState(sourceIndex, isManual, hasTarget, targetRPM);
        if (!ok) {
            // Read failure (or PWM-only hwmon fan without a readable target);
            // skip this round rather than churn writes.
            continue;
        }

        if (isManual != wantManual) {
            reapplyFanFromPreset(i);
            continue;
        }

        // Manual fans: re-apply only if the driver's set point drifted.
        // (Tiny tolerance guards against platform-specific rounding.)
        if (mode == MODE_MANUAL && hasTarget &&
            qAbs(targetRPM - fanWidgets[i]->getTargetRPM()) > 10) {
            reapplyFanFromPreset(i);
        }
        // MODE_SENSOR_BASED: the set point is recomputed every tick in
        // updateSensorData(); only the manual flag needed checking above.
    }
}

void MainWindow::reapplyFanFromPreset(int fanIndex)
{
    applyFanSettings(fanIndex,
                     fanWidgets[fanIndex]->getCurrentMode(),
                     fanWidgets[fanIndex]->getTargetRPM(),
                     sensorSettings[fanIndex].sensorIndex,
                     sensorSettings[fanIndex].minTemp,
                     sensorSettings[fanIndex].maxTemp);
    qInfo() << "Re-applied boot preset to fan" << (fanIndex + 1);
}
QString MainWindow::getBootPresetName() const
{
    QSettings settings("macsfancontrol", "macsfancontrol-qt");
    settings.beginGroup("Presets");
    QStringList presets = settings.childGroups();
    for (const QString& preset : presets) {
        settings.beginGroup(preset);
        bool launchAtBoot = settings.value("launchAtBoot", false).toBool();
        settings.endGroup();
        if (launchAtBoot) {
            settings.endGroup();
            return preset;
        }
    }
    settings.endGroup();
    return QString();
}
bool MainWindow::loadBootPreset()
{
    QString bootPreset = getBootPresetName();
    if (bootPreset.isEmpty()) {
        qDebug() << "No preset set to launch at boot.";
        return false;
    }
    qDebug() << "Loading boot preset:" << bootPreset;
    if (loadPresetByName(bootPreset)) {
        activeBootPreset = bootPreset;
        return true;
    }
    return false;
}

// Boot launch is handled by the packaged systemd service and helper script.

MainWindow::PresetBootChoice MainWindow::promptForPresetLaunchAtBoot(const QString& presetName, bool currentValue)
{
    QDialog dialog(this);
    dialog.setWindowTitle("Preset launch options");
    dialog.setModal(true);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    QLabel *label = new QLabel(QString("Preset: %1").arg(presetName), &dialog);
    layout->addWidget(label);

    QCheckBox *checkBox = new QCheckBox("Launch at Boot", &dialog);
    checkBox->setChecked(currentValue);
    layout->addWidget(checkBox);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return PresetBootCancelled;
    }
    return checkBox->isChecked() ? PresetBootLaunchAtBoot : PresetBootNoBoot;
}

void MainWindow::applyFanSettings(int fanIndex, FanMode mode, int targetRPM, int sensorIndex, int minTemp, int maxTemp)
{
    if (fanIndex < 0 || fanIndex >= fanWidgets.size()) {
        return;
    }

    FanSource source = fanSources[fanIndex];
    int sourceIndex = fanSourceIndices[fanIndex];

    // Apply settings to widget
    fanWidgets[fanIndex]->setMode(mode);
    fanWidgets[fanIndex]->setTargetRPM(targetRPM);

    if (mode == MODE_SENSOR_BASED) {
        fanWidgets[fanIndex]->setSensorBasedSettings(sensorIndex, minTemp, maxTemp);

        // Update sensor-based settings
        sensorSettings[fanIndex].enabled = true;
        sensorSettings[fanIndex].sensorIndex = sensorIndex;
        sensorSettings[fanIndex].minTemp = minTemp;
        sensorSettings[fanIndex].maxTemp = maxTemp;
    } else {
        sensorSettings[fanIndex].enabled = false;
    }

    // Apply to the correct interface
    if (mode == MODE_AUTO) {
        if (source == FAN_SOURCE_SMC) {
            smcInterface->setFanManualMode(sourceIndex, false);
        } else if (source == FAN_SOURCE_HWMON) {
            hwmonInterface->setFanManualMode(sourceIndex, false);
        }
    } else if (mode == MODE_MANUAL) {
        if (source == FAN_SOURCE_SMC) {
            smcInterface->setFanManualMode(sourceIndex, true);
            smcInterface->setFanSpeed(sourceIndex, targetRPM);
        } else if (source == FAN_SOURCE_HWMON) {
            hwmonInterface->setFanManualMode(sourceIndex, true);
            hwmonInterface->setFanSpeed(sourceIndex, targetRPM);
        }
    } else if (mode == MODE_SENSOR_BASED) {
        if (source == FAN_SOURCE_SMC) {
            smcInterface->setFanManualMode(sourceIndex, true);
        } else if (source == FAN_SOURCE_HWMON) {
            hwmonInterface->setFanManualMode(sourceIndex, true);
        }
    }
}

void MainWindow::savePreset()
{
    bool ok;
    QString presetName = QInputDialog::getText(this, "Save Preset",
                                               "Enter preset name:",
                                               QLineEdit::Normal,
                                               "", &ok);

    if (!ok || presetName.isEmpty()) {
        return;
    }

    bool launchAtBoot = false;
    QSettings settings("macsfancontrol", "macsfancontrol-qt");
    settings.beginGroup("Presets");
    settings.beginGroup(presetName);
    launchAtBoot = settings.value("launchAtBoot", false).toBool();
    settings.endGroup();
    settings.endGroup();

    PresetBootChoice choice = promptForPresetLaunchAtBoot(presetName, launchAtBoot);
    if (choice == PresetBootCancelled) {
        return;  // User cancelled: don't save, don't change any boot flags.
    }
    bool enableBoot = (choice == PresetBootLaunchAtBoot);
    savePresetToSettings(presetName, enableBoot);
    if (enableBoot) {
        createUserAutostart(presetName);
    } else {
        // If the user disabled launch at boot, remove any per-user autostart file
        removeUserAutostart();
    }
    statusBar()->showMessage(QString("Preset '%1' saved").arg(presetName), 3000);
}

void MainWindow::loadPreset()
{
    QSettings settings("macsfancontrol", "macsfancontrol-qt");
    settings.beginGroup("Presets");
    QStringList presets = settings.childGroups();
    settings.endGroup();

    if (presets.isEmpty()) {
        QMessageBox::information(this, "Load Preset",
                               "No saved presets found.\nUse 'Save Preset' to create one.");
        return;
    }

    bool ok;
    QString presetName = QInputDialog::getItem(this, "Load Preset",
                                              "Select preset to load:",
                                              presets, 0, false, &ok);

    if (ok && !presetName.isEmpty()) {
        bool launchAtBoot = false;
        QSettings settings("macsfancontrol", "macsfancontrol-qt");
        settings.beginGroup("Presets");
        settings.beginGroup(presetName);
        launchAtBoot = settings.value("launchAtBoot", false).toBool();
        settings.endGroup();
        settings.endGroup();

        PresetBootChoice choice = promptForPresetLaunchAtBoot(presetName, launchAtBoot);
        if (choice == PresetBootCancelled) {
            return;  // User cancelled: don't load the preset, don't change boot flags.
        }
        bool enableBoot = (choice == PresetBootLaunchAtBoot);
        setPresetLaunchAtBoot(presetName, enableBoot);
        if (enableBoot) {
            createUserAutostart(presetName);
        } else {
            removeUserAutostart();
        }

        if (loadPresetByName(presetName)) {
            statusBar()->showMessage(QString("Preset '%1' loaded").arg(presetName), 3000);
        }
    }
}

void MainWindow::deletePreset()
{
    QSettings settings("macsfancontrol", "macsfancontrol-qt");
    settings.beginGroup("Presets");
    QStringList presets = settings.childGroups();
    settings.endGroup();

    if (presets.isEmpty()) {
        QMessageBox::information(this, "Delete Preset",
                               "No saved presets found.");
        return;
    }

    bool ok;
    QString presetName = QInputDialog::getItem(this, "Delete Preset",
                                              "Select preset to delete:",
                                              presets, 0, false, &ok);

    if (ok && !presetName.isEmpty()) {
        QMessageBox::StandardButton reply = QMessageBox::question(this, "Confirm Delete",
                                                                  QString("Delete preset '%1'?").arg(presetName),
                                                                  QMessageBox::Yes | QMessageBox::No);

        if (reply == QMessageBox::Yes) {
            settings.beginGroup("Presets");
            settings.remove(presetName);
            settings.endGroup();
            statusBar()->showMessage(QString("Preset '%1' deleted").arg(presetName), 3000);
            qDebug() << "Preset deleted:" << presetName;
        }
    }
}

void MainWindow::exportPreset()
{
    QSettings settings("macsfancontrol", "macsfancontrol-qt");
    settings.beginGroup("Presets");
    QStringList presets = settings.childGroups();
    settings.endGroup();

    if (presets.isEmpty()) {
        QMessageBox::information(this, "Export Preset",
                               "No saved presets found.\nUse 'Save Preset' to create one.");
        return;
    }

    bool ok;
    QString presetName = QInputDialog::getItem(this, "Export Preset",
                                              "Select preset to export:",
                                              presets, 0, false, &ok);

    if (!ok || presetName.isEmpty()) {
        return;
    }

    // Build JSON document
    QJsonObject root;
    root["format"] = "macsfancontrol-preset";
    root["version"] = 1;
    root["presetName"] = presetName;
    root["fanCount"] = fanWidgets.size();

    // Get preset data from settings
    settings.beginGroup("Presets");
    settings.beginGroup(presetName);

    QJsonArray fans;
    for (int i = 0; i < fanWidgets.size(); i++) {
        settings.beginGroup(QString("Fan%1").arg(i));

        QJsonObject fan;
        fan["index"] = i;
        fan["mode"] = settings.value("mode", MODE_AUTO).toInt();
        fan["targetRPM"] = settings.value("targetRPM", 0).toInt();
        fan["sensorIndex"] = settings.value("sensorIndex", -1).toInt();
        fan["minTemp"] = settings.value("minTemp", 0).toInt();
        fan["maxTemp"] = settings.value("maxTemp", 0).toInt();

        // Add fan info for reference (label, min/max RPM)
        FanSource source = fanSources[i];
        int sourceIndex = fanSourceIndices[i];
        QJsonObject fanInfo;
        if (source == FAN_SOURCE_SMC) {
            FanInfo info = smcInterface->getFans()[sourceIndex];
            fanInfo["label"] = info.label;
            fanInfo["minRPM"] = info.minRPM;
            fanInfo["maxRPM"] = info.maxRPM;
            fanInfo["type"] = "SMC";
        } else {
            HWMonFan hwFan = hwmonInterface->getFans()[sourceIndex];
            fanInfo["label"] = hwFan.label;
            fanInfo["minRPM"] = hwFan.minRPM;
            fanInfo["maxRPM"] = hwFan.maxRPM;
            fanInfo["type"] = "HWMon";
        }
        fan["info"] = fanInfo;

        fans.append(fan);
        settings.endGroup();
    }
    root["fans"] = fans;
    root["canonicalCelsius"] = true;

    settings.endGroup();
    settings.endGroup();

    // Generate filename
    QString defaultFileName = presetName;
    defaultFileName.remove(QRegularExpression("[^a-zA-Z0-9_-]"));
    defaultFileName += ".json";

    QString filePath = QFileDialog::getSaveFileName(this, "Export Preset",
                                                     defaultFileName,
                                                     "JSON Files (*.json)");

    if (filePath.isEmpty()) {
        return;
    }

    // Write to file
    QJsonDocument doc(root);
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, "Export Error",
                            QString("Cannot write to file:\n%1").arg(filePath));
        return;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    statusBar()->showMessage(QString("Preset '%1' exported to %2").arg(presetName).arg(filePath), 5000);
    qDebug() << "Preset exported:" << presetName << "->" << filePath;
}

void MainWindow::importPreset()
{
    QString filePath = QFileDialog::getOpenFileName(this, "Import Preset",
                                                     QString(),
                                                     "JSON Files (*.json)");

    if (filePath.isEmpty()) {
        return;
    }

    // Read and parse JSON file
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, "Import Error",
                            QString("Cannot read file:\n%1").arg(filePath));
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        QMessageBox::critical(this, "Import Error",
                            QString("Invalid JSON file:\n%1\n\nError: %2")
                            .arg(filePath).arg(parseError.errorString()));
        return;
    }

    if (!doc.isObject()) {
        QMessageBox::critical(this, "Import Error",
                            "Invalid preset format: root must be an object");
        return;
    }

    QJsonObject root = doc.object();

    // Verify format
    QString format = root.value("format").toString();
    if (format != "macsfancontrol-preset") {
        QMessageBox::warning(this, "Import Warning",
                           QString("Unknown preset format: %1\nAttempting to import anyway...").arg(format));
    }

    QString presetName = root.value("presetName").toString();
    int fanCount = root.value("fanCount").toInt();

    if (presetName.isEmpty()) {
        QMessageBox::critical(this, "Import Error",
                            "Preset name is missing");
        return;
    }

    // Check fan count compatibility
    if (fanCount != fanWidgets.size()) {
        QMessageBox::StandardButton reply = QMessageBox::question(this, "Fan Count Mismatch",
                                                                  QString("This preset was created for %1 fans, "
                                                                          "but your system has %2 fans.\n\n"
                                                                          "Continue anyway?")
                                                                  .arg(fanCount).arg(fanWidgets.size()),
                                                                  QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            return;
        }
    }

    // Import as a new preset (allow user to rename)
    bool ok;
    QString saveName = QInputDialog::getText(this, "Import Preset",
                                             "Save as preset name:",
                                             QLineEdit::Normal,
                                             presetName, &ok);

    if (!ok || saveName.isEmpty()) {
        return;
    }

    // Save to settings
    QJsonArray fans = root.value("fans").toArray();
    bool canonicalCelsius = root.value("canonicalCelsius").toBool(false);
    QSettings settings("macsfancontrol", "macsfancontrol-qt");
    settings.beginGroup("Presets");
    settings.beginGroup(saveName);
    settings.setValue("fanCount", qMin(fans.size(), fanWidgets.size()));
    settings.setValue("canonicalCelsius", true);
    for (int i = 0; i < qMin(fans.size(), fanWidgets.size()); i++) {
        QJsonObject fan = fans.at(i).toObject();
        settings.beginGroup(QString("Fan%1").arg(i));
        settings.setValue("mode", fan.value("mode").toInt(MODE_AUTO));
        settings.setValue("targetRPM", fan.value("targetRPM").toInt(0));
        settings.setValue("sensorIndex", fan.value("sensorIndex").toInt(-1));
        int minTemp = fan.value("minTemp").toInt(0);
        int maxTemp = fan.value("maxTemp").toInt(0);
        if (!canonicalCelsius && (minTemp > 100 || maxTemp > 120)) {
            minTemp = qRound((minTemp - 32.0) * 5.0 / 9.0);
            maxTemp = qRound((maxTemp - 32.0) * 5.0 / 9.0);
        }
        settings.setValue("minTemp", minTemp);
        settings.setValue("maxTemp", maxTemp);
        settings.endGroup();
    }

    settings.endGroup();
    settings.endGroup();

    statusBar()->showMessage(QString("Preset imported as '%1'").arg(saveName), 5000);
    qDebug() << "Preset imported:" << saveName << "<-" << filePath;
}
