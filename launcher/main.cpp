#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExtensionPlugin>

#include "launchercontroller.h"
#include "publicsapplet.h"

Q_IMPORT_QML_PLUGIN(PublicsTradingInterfacePlugin)
Q_IMPORT_QML_PLUGIN(TradingLauncherPlugin)

int main(int argc, char *argv[])
{
#if defined(Q_OS_ANDROID)
    qputenv("ANDROID_OPENSSL_SUFFIX", "_3");
#endif
    QGuiApplication app(argc, argv);
#if defined(Q_OS_WIN) || (defined(Q_OS_MACOS) && !defined(Q_OS_IOS))
    app.setWindowIcon(QIcon(QStringLiteral(":/ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png")));
#endif
    QQmlApplicationEngine engine;
    PublicsApplet publics;
    LauncherController launcher(engine, {&publics});
    engine.rootContext()->setContextProperty(QStringLiteral("Launcher"), &launcher);

#ifdef RELEASE
    engine.rootContext()->setContextProperty(QStringLiteral("isDebugMode"), false);
#else
    engine.rootContext()->setContextProperty(QStringLiteral("isDebugMode"), true);
#endif

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule("TradingLauncher", "LauncherWindow");

    return QGuiApplication::exec();
}
