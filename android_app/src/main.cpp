#include "client_config.hpp"

#include <QGuiApplication>
#include <QCommandLineParser>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QTimer>
#include <QUrl>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Tina"));
    app.setApplicationName(QStringLiteral("Tina Android IDE"));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser parser;
    parser.addHelpOption();
    // 默认值由本地配置编译生成；桌面调试仍可通过启动参数临时覆盖。
    parser.addOption({"host", "服务端地址", "address",
                      QString::fromUtf8(tina::ui::config::ServerHost)});
    parser.addOption({"port", "服务端端口", "port",
                      QString::number(tina::ui::config::ServerPort)});
    parser.addOption({"smoke-test", "启动界面后退出，用于构建检查"});
    parser.process(app);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.setInitialProperties({{"serverHost", parser.value("host")},
                                 {"serverPort", parser.value("port").toInt()}});
    engine.loadFromModule("TinaIde", "Main");

    if (app.arguments().contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(300, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
