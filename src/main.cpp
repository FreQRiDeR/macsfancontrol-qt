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

    return QProcess::startDetached(elevator, args);
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

    QCommandLineParser parser;
    parser.setApplicationDescription("Mac Fan Control");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"daemon", "Run without showing the GUI"});
    parser.addOption({"preset", "Load a saved preset on startup", "preset"});

    // systemd installer option removed

    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments << QString::fromLocal8Bit(argv[i]);
    }
    parser.process(arguments);

    bool daemonMode = parser.isSet("daemon");
    QString presetName = parser.value("preset");

    if (daemonMode) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);

    // Set application information
    app.setApplicationName("Mac Fan Control");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("macsfancontrol-qt");

    // systemd installer handling removed

    if (daemonMode) {
        if (geteuid() != 0) {
            qWarning() << "Daemon mode requires root privileges";
            return 1;
        }

        MainWindow window;
        window.hide();
        if (!presetName.isEmpty()) {
            window.loadPresetByName(presetName);
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
            QStringList elevatedArgs = arguments.mid(1);
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
