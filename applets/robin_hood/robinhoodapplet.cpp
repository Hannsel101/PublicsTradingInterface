#include "robinhoodapplet.h"
#include "rhnetworkengine.h"
#include "rhsessionmanager.h"
#include "rhbrowserlogin.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <utility>

struct RobinHoodApplet::State
{
    State() : sessions(network), browser([this](QByteArray token, QDateTime expires) {
        sessions.acceptBrowserToken(std::move(token), expires);
    })
    {
        Q_INIT_RESOURCE(rh_config);
        QString error;
        network.loadConfiguration(QStringLiteral(":/robinhood/config/robinhood-api.json"), &error);
        sessions.setBrowserLogin(&browser);
    }
    RHNetworkEngine network;
    RHSessionManager sessions;
    RHBrowserLogin browser;
};

RobinHoodApplet::RobinHoodApplet(QObject *parent)
    : AppletInterface(parent), m_state(std::make_unique<State>())
{
    connect(&m_state->sessions, &RHSessionManager::busyChanged,
            this, &AppletInterface::navigationAvailabilityChanged);
    connect(&m_state->sessions, &RHSessionManager::browserLoginPendingChanged,
            this, &AppletInterface::navigationAvailabilityChanged);
}

RobinHoodApplet::~RobinHoodApplet() = default;

void RobinHoodApplet::prepareBrowserRuntime()
{
    RHBrowserLogin::prepareApplication();
}

void RobinHoodApplet::initializeBrowserRuntime()
{
    RHBrowserLogin::initializeApplication();
}

QString RobinHoodApplet::id() const
{
    return QStringLiteral("robin_hood");
}

void RobinHoodApplet::initialize(QQmlApplicationEngine &engine)
{
    engine.rootContext()->setContextProperty(QStringLiteral("RobinSessions"), &m_state->sessions);
    engine.rootContext()->setContextProperty(QStringLiteral("RobinBrowser"), &m_state->browser);
}

void RobinHoodApplet::start()
{
    m_state->sessions.load();
}

bool RobinHoodApplet::canStop() const
{
    return !m_state->sessions.busy() || m_state->sessions.browserLoginPending();
}

void RobinHoodApplet::stop()
{
    m_state->sessions.cancelSignIn();
    m_state->sessions.clearActiveSession();
}
