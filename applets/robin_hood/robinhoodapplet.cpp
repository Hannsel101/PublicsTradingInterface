#include "robinhoodapplet.h"
#include "rhnetworkengine.h"
#include "rhsessionmanager.h"
#include "rhnativeauthclient.h"
#include "rhtradingcontext.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <utility>

struct RobinHoodApplet::State
{
    State() : sessions(network), trades(network, makeRHTradingDependencies(sessions))
    {
        Q_INIT_RESOURCE(rh_config);
        QString error;
        network.loadConfiguration(QStringLiteral(":/robinhood/config/robinhood-api.json"), &error);
        sessions.setNativeAuthClient(&native);
        QObject::connect(&sessions, &RHSessionManager::busyChanged,
                         &trades, &RHTradeController::refreshAvailability);
        QObject::connect(&sessions, &RHSessionManager::sessionActiveChanged,
                         &trades, &RHTradeController::refreshAvailability);
        QObject::connect(&sessions, &RHSessionManager::accountCacheChanged,
                         &trades, &RHTradeController::refreshAvailability);
        QObject::connect(&sessions, &QAbstractItemModel::modelReset,
                         &trades, &RHTradeController::refreshAvailability);
        QObject::connect(&sessions, &QAbstractItemModel::dataChanged,
                         &trades, &RHTradeController::refreshAvailability);
        trades.refreshAvailability();
    }

    RHNetworkEngine network;
    RHNativeAuthClient native;
    RHSessionManager sessions;
    // Destroy the controller first: pending callbacks and its lease belong to this state.
    RHTradeController trades;
};

RobinHoodApplet::RobinHoodApplet(QObject *parent)
    : AppletInterface(parent), m_state(std::make_unique<State>())
{
    connect(&m_state->sessions, &RHSessionManager::busyChanged,
            this, &AppletInterface::navigationAvailabilityChanged);
    connect(&m_state->sessions, &RHSessionManager::nativeLoginChanged,
            this, &AppletInterface::navigationAvailabilityChanged);
}

RobinHoodApplet::~RobinHoodApplet() = default;

void RobinHoodApplet::prepareBrowserRuntime()
{
    // Retained launcher ABI; native login requires no browser runtime.
}

void RobinHoodApplet::initializeBrowserRuntime()
{
    // No WebEngine/WebView initialization for production authentication.
}

QString RobinHoodApplet::id() const
{
    return QStringLiteral("robin_hood");
}

void RobinHoodApplet::initialize(QQmlApplicationEngine &engine)
{
    engine.rootContext()->setContextProperty(QStringLiteral("RobinSessions"), &m_state->sessions);
    engine.rootContext()->setContextProperty(QStringLiteral("RobinTrades"), &m_state->trades);
}

void RobinHoodApplet::start()
{
    m_state->sessions.load();
}

bool RobinHoodApplet::canStop() const
{
    return !m_state->sessions.busy() || m_state->sessions.nativeLoginCancellable();
}

void RobinHoodApplet::stop()
{
    m_state->sessions.cancelSignIn();
    m_state->sessions.clearActiveSession();
}
