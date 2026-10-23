/**
 * @file main.cc
 * @brief Frontend entry point.
 *
 * Usage: andor_emccd_gui [--host HOST] [--control-port N] [--data-port N]
 *                        [--tcp] [--ipc-dir DIR] [--no-connect]
 *                        [--screenshot FILE --screenshot-delay MS]
 *                        [--export-icon DIR]
 */

#include <cmath>

#include <QApplication>
#include <QDir>
#include <QCommandLineParser>
#include <QHostAddress>
#include <QMessageBox>
#include <QTcpServer>
#include <QTimer>
#include <QWidget>

#include "AppIcon.h"
#include "MainWindow.h"

namespace aec = AndorEMCCDcontroller;

namespace {
constexpr quint16 kInstanceLockPort = 47109;
}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setOrganizationName("AndorEMCCDcontroller");
  QApplication::setApplicationName("AndorEMCCDcontroller");
  QApplication::setApplicationVersion(AEC_VERSION);
  QApplication::setWindowIcon(aec::CreateAppIcon());
  // Linux desktops match the window to andor_emccd_gui.desktop by this.
  QGuiApplication::setDesktopFileName("andor_emccd_gui");

  QCommandLineParser parser;
  parser.setApplicationDescription("Frontend for the iXon EMCCD backend");
  parser.addHelpOption();
  parser.addVersionOption();
  QCommandLineOption host({"H", "host"}, "Backend host.", "host");
  QCommandLineOption control_port("control-port", "Control port.", "port");
  QCommandLineOption data_port("data-port", "Data port.", "port");
  QCommandLineOption tcp("tcp", "Use TCP even on the backend host.");
  QCommandLineOption ipc_dir("ipc-dir", "IPC socket directory.", "dir");
  QCommandLineOption no_connect("no-connect", "Do not connect at start.");
  QCommandLineOption name("name", "Client name shown to others (name@ip).",
                          "name");
  QCommandLineOption request_control(
      "request-control", "Request the settings control right after connecting.");
  QCommandLineOption screenshot(
      "screenshot", "Save a window screenshot and quit (for testing).", "file");
  QCommandLineOption screenshot_delay("screenshot-delay",
                                      "Delay before the screenshot.", "ms",
                                      "3000");
  QCommandLineOption export_icon(
      "export-icon",
      "Write the icon as icon_<size>.png into the directory and quit.", "dir");
  QCommandLineOption buffer_frames("buffer-frames", "Client ring capacity in frames.",
                                    "frames", "64");
  parser.addOption(buffer_frames);
  QCommandLineOption stream_save("stream-save", "Save received raw frames to a new BigTIFF file.", "file");
  QCommandLineOption save_fps("save-max-fps", "Maximum save FPS; 0 saves all received frames.", "fps", "10");
  parser.addOptions({stream_save, save_fps});
  parser.addOptions({host, control_port, data_port, tcp, ipc_dir, no_connect,
                     name, request_control, screenshot, screenshot_delay,
                     export_icon});
  parser.process(app);
  bool valid_save_fps = false;
  const double max_save_fps = parser.value(save_fps).toDouble(&valid_save_fps);
  if (!valid_save_fps || !std::isfinite(max_save_fps) || max_save_fps < 0 ||
      max_save_fps > 100000 || (parser.isSet(stream_save) && parser.isSet(no_connect))) {
    qCritical("invalid --save-max-fps or --stream-save with --no-connect");
    return 2;
  }
  bool valid_buffer = false;
  const int capacity = parser.value(buffer_frames).toInt(&valid_buffer);
  if (!valid_buffer || capacity < 1) {
    qCritical("--buffer-frames must be a positive integer");
    return 2;
  }

  if (parser.isSet(export_icon)) {
    const QDir dir(parser.value(export_icon));
    for (const int size : {16, 24, 32, 48, 64, 128, 256}) {
      const QString file =
          dir.filePath(QStringLiteral("icon_%1.png").arg(size));
      if (!aec::RenderAppIcon(size).save(file)) {
        qCritical("cannot write %s", qPrintable(file));
        return 1;
      }
    }
    return 0;
  }

  // One frontend per machine: a client only connects, so nothing else
  // would stop a second instance. Binding a fixed loopback port is a
  // machine-wide lock that the OS releases even after a crash.
  QTcpServer instance_lock;
  if (!instance_lock.listen(QHostAddress::LocalHost, kInstanceLockPort)) {
    qCritical("another frontend is already running (port %u in use)",
              kInstanceLockPort);
    QMessageBox::critical(
        nullptr, QObject::tr("Already running"),
        QObject::tr("Another iXon EMCCD frontend is already running on this "
                    "machine (port %1 is in use).")
            .arg(kInstanceLockPort));
    return 1;
  }

  aec::MainWindow window;
  window.resize(1500, 950);
  window.show();

  if (!parser.isSet(no_connect)) {
    aec::ServerAddress address = window.Address();
    if (parser.isSet(buffer_frames)) address.buffer_frames = capacity;
    if (parser.isSet(host)) address.host = parser.value(host).toStdString();
    if (parser.isSet(control_port)) {
      address.control_port = parser.value(control_port).toInt();
    }
    if (parser.isSet(data_port)) {
      address.data_port = parser.value(data_port).toInt();
    }
    if (parser.isSet(tcp)) address.use_ipc = 0;
    if (parser.isSet(name)) address.client_name = parser.value(name).toStdString();
    if (parser.isSet(ipc_dir)) {
      address.ipc_dir = parser.value(ipc_dir).toStdString();
    }
    const bool take = parser.isSet(request_control);
    const QString stream_path = parser.value(stream_save);
    QTimer::singleShot(0, &window, [&window, address, take, stream_path, max_save_fps] {
      window.ConnectTo(address);
      if (!stream_path.isEmpty()) window.StartStreamRecording(stream_path, max_save_fps);
      if (take) window.RequestControl();
    });
  }
  if (parser.isSet(screenshot)) {
    const QString file = parser.value(screenshot);
    QTimer::singleShot(parser.value(screenshot_delay).toInt(), &window,
                       [&window, file] {
                         // AEC_SCREENSHOT_DOCK=<objectName> raises a dock.
                         const QString dock =
                             qEnvironmentVariable("AEC_SCREENSHOT_DOCK");
                         if (auto* w = window.findChild<QWidget*>(dock)) {
                           w->raise();
                           QApplication::processEvents();
                         }
                         window.grab().save(file);
                         window.close();
                       });
  }
  return app.exec();
}
