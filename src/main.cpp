#include "mainwindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QSettings>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ByteMe");
    QCoreApplication::setApplicationName("ByteMe");
    QCoreApplication::setApplicationVersion("1.0.0");
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("ByteMe 局域网文件传输工具"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOptions({
        {{"p", "port"}, QStringLiteral("TCP 接收端口，0 表示自动分配"), "port", "45455"},
        {"udp-port", QStringLiteral("UDP 发现端口"), "port", "45454"},
        {"name", QStringLiteral("本机显示名称"), "name"},
        {"receive-dir", QStringLiteral("接收目录"), "directory"},
        {"no-discovery", QStringLiteral("关闭 UDP 发现")},
        {"settings-dir", QStringLiteral("使用独立的 INI 配置目录"), "directory"},
        {"screenshot", QStringLiteral("保存界面截图并退出（用于界面验证）"), "path"}
    });
    parser.process(app);
    bool tcpOk = false, udpOk = false;
    const int tcpPort = parser.value("port").toInt(&tcpOk);
    const int udpPort = parser.value("udp-port").toInt(&udpOk);
    if (!tcpOk || tcpPort < 0 || tcpPort > 65535 || !udpOk || udpPort < 1 || udpPort > 65535)
        parser.showHelp(1);
    if (parser.isSet("settings-dir")) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir(parser.value("settings-dir")).absolutePath());
    }
    AppOptions options;
    options.tcpPort = quint16(tcpPort);
    options.udpPort = quint16(udpPort);
    options.name = parser.value("name");
    options.receiveDirectory = parser.value("receive-dir");
    options.discovery = !parser.isSet("no-discovery");
    MainWindow window(options);
    window.show();
    if (parser.isSet("screenshot")) {
        QTimer::singleShot(600, &window, [&] {
            const bool saved = window.grab().save(parser.value("screenshot"));
            window.close();
            app.exit(saved ? 0 : 2);
        });
    }
    return app.exec();
}
