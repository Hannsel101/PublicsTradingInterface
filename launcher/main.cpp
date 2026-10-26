#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExtensionPlugin>

#include "launchercontroller.h"
#include "publicsapplet.h"
#include "robinhoodapplet.h"

Q_IMPORT_QML_PLUGIN(PublicsTradingInterfacePlugin)
Q_IMPORT_QML_PLUGIN(RobinHoodInterfacePlugin)
Q_IMPORT_QML_PLUGIN(TradingLauncherPlugin)

int main(int argc, char *argv[])
{
#if defined(Q_OS_ANDROID)
    qputenv("ANDROID_OPENSSL_SUFFIX", "_3");
#endif
    RobinHoodApplet::prepareBrowserRuntime();
    QGuiApplication app(argc, argv);
    RobinHoodApplet::initializeBrowserRuntime();
#if defined(Q_OS_WIN) || (defined(Q_OS_MACOS) && !defined(Q_OS_IOS))
    app.setWindowIcon(QIcon(QStringLiteral(":/ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png")));
#endif
    QQmlApplicationEngine engine;
    PublicsApplet publics;
    RobinHoodApplet robinHood;
    LauncherController launcher(engine, {&publics, &robinHood});
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
