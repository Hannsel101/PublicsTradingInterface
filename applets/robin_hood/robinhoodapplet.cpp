#include "robinhoodapplet.h"
#include "rhnetworkengine.h"
#include "rhsessionmanager.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>

struct RobinHoodApplet::State
{
    State() : sessions(network)
    {
        Q_INIT_RESOURCE(rh_config);
        QString error;
        network.loadConfiguration(QStringLiteral(":/robinhood/config/robinhood-api.json"), &error);
    }
    RHNetworkEngine network;
    RHSessionManager sessions;
};

RobinHoodApplet::RobinHoodApplet(QObject *parent)
    : AppletInterface(parent), m_state(std::make_unique<State>())
{
    connect(&m_state->sessions, &RHSessionManager::busyChanged,
            this, &AppletInterface::navigationAvailabilityChanged);
    connect(&m_state->sessions, &RHSessionManager::approvalChanged,
            this, &AppletInterface::navigationAvailabilityChanged);
}

RobinHoodApplet::~RobinHoodApplet() = default;

QString RobinHoodApplet::id() const
{
    return QStringLiteral("robin_hood");
}

void RobinHoodApplet::initialize(QQmlApplicationEngine &engine)
{
    engine.rootContext()->setContextProperty(QStringLiteral("RobinSessions"), &m_state->sessions);
}

void RobinHoodApplet::start()
{
    m_state->sessions.load();
}

bool RobinHoodApplet::canStop() const
{
    return !m_state->sessions.busy() || m_state->sessions.approvalPending();
}

void RobinHoodApplet::stop()
{
    m_state->sessions.cancelSignIn();
    m_state->sessions.clearActiveSession();
}
