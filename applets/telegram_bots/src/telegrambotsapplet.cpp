#include "telegrambotsapplet.h"
#include "telegrambotscontroller.h"
#include <QQmlApplicationEngine>
#include <QQmlContext>

TelegramBotsApplet::TelegramBotsApplet(QObject *parent)
    : AppletInterface(parent), m_controller(new TelegramBotsController(this)) { connectController(); }

TelegramBotsApplet::TelegramBotsApplet(TelegramBotsController *controller, QObject *parent)
    : AppletInterface(parent), m_controller(controller) { connectController(); }

void TelegramBotsApplet::connectController()
{
    if (m_controller)
        connect(m_controller, &TelegramBotsController::stateChanged,
                this, &AppletInterface::navigationAvailabilityChanged);
}

void TelegramBotsApplet::initialize(QQmlApplicationEngine &engine)
{
    engine.rootContext()->setContextProperty(QStringLiteral("TelegramBots"), m_controller.data());
}

void TelegramBotsApplet::start()
{
    if (m_controller) m_controller->load();
}

bool TelegramBotsApplet::canStop() const
{
    return m_controller && !m_controller->busy() && !m_controller->hasRunningBots();
}

void TelegramBotsApplet::stop()
{
    if (m_controller && m_controller->hasRunningBots()) m_controller->stopAll();
}
