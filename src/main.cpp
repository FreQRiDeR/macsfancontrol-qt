#include <QApplication>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QStringList>
#include <QCommandLineParser>
#include <QDebug>
#include "mainwindow.h"
#include <unistd.h>
#include <cstdio>
#include <csignal>
// The boot daemon is a failsafe that ramps fans up at boot when the machine is
// unattended. When the user launches the GUI, the GUI sends SIGTERM to the
// daemon to take over. Qt's default SIGTERM handler calls exit(0) without
// running the MainWindow destructor, which would leave the lock held. Install
// a handler that triggers a clean Qt shutdown so the destructor runs
// releaseFanControlLock(). The daemon never restores auto mode on its own — it
// keeps the fans at the preset until the GUI takes over.
static void daemonSigtermHandler(int /*sig*/)
{
    QCoreApplication::quit();
}

// Captured debug log messages (appended by the message handler below)
static QStringList g_debugLog;

static QString findPrivilegeElevator()
{
    QString elevator = QStandardPaths::findExecutable("pkexec");
    if (!elevator.isEmpty()) {
        return elevator;
    }
    return QString();
}

static bool launchElevated(const QStringList &args)
{
    QString elevator = findPrivilegeElevator();
    if (elevator.isEmpty()) {
        return false;
    }

    // pkexec strips the environment by default. Without DISPLAY, XAUTHORITY
    // and DBUS_SESSION_BUS_ADDRESS the elevated Qt GUI cannot connect to the
    // user's graphical session, so the app would appear to never launch.
    // Run through `env` to pass the graphical session through to the child.
    QStringList commandArgs;
    commandArgs << QStringLiteral("--")
                << QStringLiteral("env");
    const char *envVars[] = { "DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS" };
    for (const char *var : envVars) {
        const QByteArray value = qgetenv(var);
        if (!value.isEmpty()) {
            commandArgs << QStringLiteral("%1=%2").arg(QLatin1String(var),
                                                        QString::fromLocal8Bit(value));
        }
    }
    commandArgs << QCoreApplication::applicationFilePath();
    commandArgs << args;

    return QProcess::startDetached(elevator, commandArgs);
}

// systemd installer helper removed

static void debugMessageHandler(QtMsgType type, const QMessageLogContext& /*context*/, const QString& msg)
{
    const char *prefix;
    switch (type) {
    case QtDebugMsg:    prefix = "[DEBUG]"; break;
    case QtWarningMsg:  prefix = "[WARN] "; break;
    case QtCriticalMsg: prefix = "[ERROR]"; break;
    case QtFatalMsg:    prefix = "[FATAL]"; break;
    default:            prefix = "[INFO] "; break;
    }
    g_debugLog.append(QString("%1 %2").arg(prefix, msg));
    fprintf(stderr, "%s %s\n", prefix, msg.toLocal8Bit().constData());
    if (type == QtFatalMsg)
        abort();
}

// Called from MainWindow to retrieve the captured log
QStringList getDebugLog()
{
    return g_debugLog;
}

int main(int argc, char *argv[])
{
    qInstallMessageHandler(debugMessageHandler);

    QCoreApplication::setAttribute(Qt::AA_Use96Dpi, true);

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--daemon") == 0) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            break;
        }
    }

    QApplication app(argc, argv);

    // Set application information
    app.setApplicationName("Mac Fan Control");
    app.setApplicationVersion("1.0.3");
    app.setOrganizationName("macsfancontrol-qt");

    QCommandLineParser parser;
    parser.setApplicationDescription("Mac Fan Control");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"daemon", "Run without showing the GUI"});
    parser.addOption({"preset", "Load a saved preset on startup", "preset"});

    parser.process(app);

    bool daemonMode = parser.isSet("daemon");
    QString presetName = parser.value("preset");

    // systemd installer handling removed

    if (daemonMode) {
        if (geteuid() != 0) {
            qWarning() << "Daemon mode requires root privileges";
            return 1;
        }
        // Install a SIGTERM handler so the GUI can gracefully take over: the
        // handler triggers QCoreApplication::quit(), which runs the MainWindow
        // destructor (releaseFanControlLock) before exiting. The daemon never
        // restores auto mode on its own.
        std::signal(SIGTERM, daemonSigtermHandler);
        MainWindow window(nullptr, true);
        if (!window.isInitialized()) {
            qCritical().noquote() << "Daemon startup failed:" << window.initializationError();
            return 1;
        }
        window.hide();
        if (!presetName.isEmpty()) {
            if (!window.loadPresetByName(presetName)) {
                qCritical() << "Failed to load requested preset:" << presetName;
                return 1;
            }
        } else {
            if (!window.loadBootPreset()) {
                qInfo() << "No preset configured to launch at boot. Exiting daemon.";
                return 0;
            }
        }
        return app.exec();
    }

    if (geteuid() != 0) {
        QMessageBox msgBox;
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setWindowTitle("Permission Required");
        msgBox.setText("This application requires root privileges to control fans.");
        msgBox.setInformativeText("You can continue in read-only mode, or authenticate to run as administrator.");
        QPushButton *runAsAdmin = msgBox.addButton("Run as Administrator", QMessageBox::AcceptRole);
        msgBox.addButton("Continue Read-Only", QMessageBox::DestructiveRole);
        QPushButton *cancel = msgBox.addButton(QMessageBox::Cancel);
        msgBox.setDefaultButton(runAsAdmin);

        msgBox.exec();
        if (msgBox.clickedButton() == cancel) {
            return 0;
        }
        if (msgBox.clickedButton() == runAsAdmin) {
            QStringList elevatedArgs = app.arguments().mid(1);
            if (launchElevated(elevatedArgs)) {
                return 0;
            }
            QMessageBox::critical(nullptr, "Elevation Failed",
                                  "Unable to request administrator privileges. "
                                  "Please install a policy agent such as polkit or run the app with sudo.");
            return 1;
        }
        // Continue in read-only mode
    }

    MainWindow window;
    if (!presetName.isEmpty()) {
        window.loadPresetByName(presetName);
    }
    window.show();

    return app.exec();
}
