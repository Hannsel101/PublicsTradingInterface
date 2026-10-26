#include "publicsapplet.h"

#include "auth/publicauthclient.h"
#include "api/publicapiworker.h"
#include "trading/autotradecontroller.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>

struct PublicsApplet::State
{
    State() : auth(QString()), autoTrader(&auth, &worker) {}

    PublicAuthClient auth;
    PublicApiWorker worker;
    AutoTradeController autoTrader;
};

PublicsApplet::PublicsApplet(QObject *parent)
    : AppletInterface(parent), m_state(std::make_unique<State>())
{
    connect(&m_state->auth, &PublicAuthClient::tokenReceived,
            &m_state->worker, &PublicApiWorker::processToken);
    connect(&m_state->auth, &PublicAuthClient::selectedApiKeyLabelChanged,
            &m_state->worker, [this]() {
                if (m_state->auth.selectedApiKeyLabel().isEmpty())
                    m_state->worker.clearSubAccountsList();
            });
    connect(&m_state->auth, &PublicAuthClient::apiKeyIndexLoadingChanged,
            this, &AppletInterface::navigationAvailabilityChanged);
    connect(&m_state->auth, &PublicAuthClient::apiKeyLoadingChanged,
            this, &AppletInterface::navigationAvailabilityChanged);
    connect(&m_state->worker, &PublicApiWorker::tradeResultsBusyChanged,
            this, &AppletInterface::navigationAvailabilityChanged);
    connect(&m_state->autoTrader, &AutoTradeController::tradeResultsCompleteChanged,
            this, &AppletInterface::navigationAvailabilityChanged);
}

PublicsApplet::~PublicsApplet() = default;

QString PublicsApplet::id() const
{
    return QStringLiteral("publics");
}

void PublicsApplet::initialize(QQmlApplicationEngine &engine)
{
    engine.rootContext()->setContextProperty(QStringLiteral("AuthClient"), &m_state->auth);
    engine.rootContext()->setContextProperty(QStringLiteral("ApiWorker"), &m_state->worker);
    engine.rootContext()->setContextProperty(QStringLiteral("AutoTrader"), &m_state->autoTrader);
}

void PublicsApplet::start()
{
    m_state->auth.listStoredApiKeys();
}

bool PublicsApplet::canStop() const
{
    return !m_state->auth.apiKeyIndexLoading() && !m_state->auth.apiKeyLoading()
        && !m_state->worker.tradeResultsBusy() && !m_state->autoTrader.tradeResultsBusy();
}

void PublicsApplet::stop()
{
    m_state->auth.clearUserSession();
    m_state->auth.setSecretKey(QString());
    m_state->worker.clearSubAccountsList();
    m_state->worker.dismissTradeResults();
    m_state->autoTrader.dismissTradeResults();
}
