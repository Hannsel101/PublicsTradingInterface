#include "telegramwebhookhost.h"
#include "telegramwebhookmanager.h"
#include "telegraminspectorprocess.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHttpServer>
#include <QHttpServerResponder>
#include <QHttpHeaders>
#include <cmath>
#include <utility>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

namespace {
// Qt's keep-alive limit is idle-based, not a total header/body deadline.
// Keep a hard connection lifetime too, so a dripping peer cannot hold resources.
class PrivateTcpServer final : public QTcpServer
{
public:
    using QTcpServer::QTcpServer;
protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QTcpSocket(this);
        if (!socket->setSocketDescriptor(descriptor)) { socket->deleteLater(); return; }
        socket->setReadBufferSize(272 * 1024);
        auto *deadline = new QTimer(socket);
        deadline->setSingleShot(true);
        deadline->setTimerType(Qt::PreciseTimer);
        QObject::connect(deadline, &QTimer::timeout, socket, &QTcpSocket::abort);
        deadline->start(3000);
        addPendingConnection(socket);
    }
};
QString randomSecret()
{
    QByteArray bytes(32, Qt::Uninitialized);
    auto *rng = QRandomGenerator::system();
    for (auto &byte : bytes) byte = char(rng->generate() & 0xff);
    return QString::fromLatin1(bytes.toHex());
}
bool safeInspector(const QUrl &url)
{
    return url.isValid() && url.scheme() == QStringLiteral("http")
        && url.host() == QStringLiteral("127.0.0.1") && url.port() > 0
        && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment()
        && url.path() == QStringLiteral("/api/tunnels");
}
bool compatible(const TelegramWebhookHost::Options &a, const TelegramWebhookHost::Options &b)
{
    return a.executable == b.executable && a.argumentPrefix == b.argumentPrefix
        && a.port == b.port && a.inspectorUrl == b.inspectorUrl
        && a.ownershipDirectory == b.ownershipDirectory
        && a.discoveryDelayMs == b.discoveryDelayMs && a.startupTimeoutMs == b.startupTimeoutMs;
}
}

struct TelegramWebhookHost::State
{
    enum Phase { Idle, Preflight, AwaitingInspectorDecision, RestartingInspector, Starting, Discovering, Operational, Stopping } phase = Idle;
    struct Client { QString path; QString secret; AdmissionHandler admit; RequestHandler receive; };
    TelegramWebhookHost *q;
    Options options;
    QMap<QString, Client> clients;
    QStringList waitingRelease;
    QString health;
    QUrl publicUrl;
    QPointer<QProcess> process;
    QByteArray ownedReceipt;
    QPointer<QHttpServer> server;
    QPointer<QTcpServer> listener;
    QPointer<QTcpSocket> probe;
    QPointer<QNetworkReply> reply;
    QNetworkAccessManager network;
    QTimer deadline;
    QTimer discovery;
    QTimer killDeadline;
    QElapsedTimer elapsed;
    QTimer restartPoll;
    QElapsedTimer restartElapsed;
    std::unique_ptr<TelegramInspectorProcess> orphan;
    bool escalated = false;
    quint64 generation = 0;

    explicit State(TelegramWebhookHost *owner) : q(owner)
    {
        network.setProxy(QNetworkProxy::NoProxy);
        deadline.setSingleShot(true);
        deadline.setTimerType(Qt::PreciseTimer);
        discovery.setSingleShot(true);
        discovery.setTimerType(Qt::PreciseTimer);
        killDeadline.setSingleShot(true);
        restartPoll.setInterval(25);
        QObject::connect(&restartPoll, &QTimer::timeout, q, [this] { pollRestart(); });
        QObject::connect(&deadline, &QTimer::timeout, q, [this] {
            fail(QStringLiteral("Tunnel startup timed out."));
        });
        QObject::connect(&discovery, &QTimer::timeout, q, [this] { discover(); });
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, q, [this] {
            // No timer can be relied on after the application event loop exits.
            if (process && process->state() != QProcess::NotRunning) process->kill();
        });
        QObject::connect(&killDeadline, &QTimer::timeout, q, [this] {
            if (process && process->state() != QProcess::NotRunning) process->kill();
        });
    }
    void cancelIo()
    {
        deadline.stop();
        discovery.stop();
        restartPoll.stop();
        orphan.reset();
        if (probe) {
            auto *socket = probe.data(); probe = nullptr;
            socket->disconnect(q); socket->abort(); socket->deleteLater();
        }
        if (reply) {
            auto *response = reply.data(); reply = nullptr;
            response->disconnect(q); response->abort(); response->deleteLater();
        }
    }
    void closeServer()
    {
        if (listener) listener->close();
        listener = nullptr;
        if (server) {
            // A direct update observer may destroy the host inside the HTTP callback.
            // Keep the executing server/socket alive until the event-loop unwinds.
            server->setParent(nullptr);
            server->deleteLater();
            server = nullptr;
        }
        health.clear();
    }
    void finishStop()
    {
        killDeadline.stop();
        phase = Idle;
        const auto ids = std::exchange(waitingRelease, {});
        const QPointer<TelegramWebhookHost> guard(q);
        for (const auto &id : ids) {
            emit q->released(id);
            if (!guard) return;
        }
    }
    void stop()
    {
        if (phase == Stopping) return;
        phase = Stopping;
        ++generation;
        cancelIo();
        closeServer();
        publicUrl.clear();
        if (process && process->state() != QProcess::NotRunning) {
            process->terminate();
            killDeadline.start(1000);
        } else {
            if (process) { process->deleteLater(); process = nullptr; }
            finishStop();
        }
    }
    void fail(const QString &message)
    {
        if (phase == Stopping || phase == Idle) return;
        const auto ids = clients.keys();
        clients.clear();
        const QPointer<TelegramWebhookHost> guard(q);
        stop();
        if (!guard) return;
        for (const auto &id : ids) {
            emit q->failed(id, message);
            if (!guard) return;
        }
    }
    void notifyReady(const QString &id)
    {
        if (phase != Operational || !clients.contains(id)) return;
        const auto client = clients.value(id);
        QUrl url(publicUrl);
        url.setPath(client.path);
        emit q->ready(id, url, client.secret);
    }
    void receive(const QHttpServerRequest &request, QHttpServerResponder &responder)
    {
        using Status = QHttpServerResponder::StatusCode;
        if (phase != Operational || !request.remoteAddress().isLoopback()
            || request.url().hasQuery() || request.url().hasFragment()) {
            responder.write(Status::NotFound); return;
        }
        const auto path = request.url().path();
        const auto authentic = [&](const QByteArray &header, const QByteArray &expected) {
            const auto values = request.headers().values(header);
            if (values.size() != 1 || values.first().size() != expected.size()) return false;
            unsigned char difference = 0;
            for (qsizetype i = 0; i < expected.size(); ++i)
                difference |= static_cast<unsigned char>(values.first().at(i) ^ expected.at(i));
            return difference == 0;
        };
        if (path == QStringLiteral("/health") && request.method() == QHttpServerRequest::Method::Get
            && authentic("X-PTI-Health-Secret", health.toLatin1())) {
            responder.write(Status::Ok); return;
        }
        QString id;
        for (auto it = clients.cbegin(); it != clients.cend(); ++it) {
            if (it.value().path == path) { id = it.key(); break; }
        }
        if (id.isEmpty()) {
            responder.write(Status::NotFound); return;
        }
        if (request.method() != QHttpServerRequest::Method::Post) {
            responder.write(Status::MethodNotAllowed); return;
        }
        if (!authentic("X-Telegram-Bot-Api-Secret-Token", clients.value(id).secret.toLatin1())) {
            responder.write(Status::Forbidden); return;
        }
        const auto receiveHandler = clients.value(id).receive;
        if (receiveHandler) {
            const QPointer<TelegramWebhookHost> guard(q);
            const auto status = receiveHandler(request);
            if (guard) responder.write(static_cast<Status>(status));
            return;
        }
        const auto body = request.body();
        if (body.size() > 256 * 1024) { responder.write(Status::PayloadTooLarge); return; }
        const auto document = QJsonDocument::fromJson(body);
        const auto identity = document.object().value("update_id");
        const double numericId = identity.toDouble(-1);
        if (!document.isObject() || !identity.isDouble() || numericId <= 0
            || numericId >= 4503599627370496.0 || std::floor(numericId) != numericId) {
            responder.write(Status::BadRequest); return;
        }
        // The receiver only validates the transport envelope. The agent owns admission,
        // domain authorization and deduplication. Notify before acknowledging delivery.
        const QPointer<TelegramWebhookHost> guard(q);
        // Copy the callback: it may release the client or destroy this host.
        const auto admission = clients.value(id).admit;
        const bool accepted = !admission || admission(document.object());
        if (!guard) return;
        if (!accepted) { responder.write(Status::ServiceUnavailable); return; }
        emit q->updateReceived(id, document.object());
        if (guard) responder.write(Status::Ok);
    }
    void launch()
    {
        if (phase != Preflight) return;
        phase = Starting;
        server = new QHttpServer(q);
        QHttpServerConfiguration limits;
        limits.setMaximumBodySize(256 * 1024);
        limits.setMaximumUrlSize(1024);
        limits.setMaximumTotalHeaderSize(16 * 1024);
        limits.setMaximumHeaderFieldSize(4096);
        limits.setMaximumHeaderFieldCount(32);
        limits.setMaximumConnections(64);
        limits.setMaximumConnectionsPerHost(32);
        limits.setRateLimitPerSecond(100);
        limits.setKeepAliveTimeout(std::chrono::seconds(1));
        server->setConfiguration(limits);
        server->setMissingHandler(q, [this](const QHttpServerRequest &request, QHttpServerResponder &responder) {
            receive(request, responder);
        });
        listener = new PrivateTcpServer(server);
        if (!listener->listen(QHostAddress::LocalHost, options.port) || !server->bind(listener)) {
            fail(QStringLiteral("Cannot bind the private webhook receiver.")); return;
        }
        health = randomSecret();
        auto *child = new QProcess(q);
        process = child;
        child->setProcessChannelMode(QProcess::SeparateChannels);
        QObject::connect(child, &QProcess::readyReadStandardOutput, q, [child] { child->readAllStandardOutput(); });
        QObject::connect(child, &QProcess::readyReadStandardError, q, [child] { child->readAllStandardError(); });
        QObject::connect(child, &QProcess::started, q, [this, child] {
            if (process != child || phase != Starting) return;
            const auto port = quint16(options.inspectorUrl.port());
            const auto root = options.ownershipDirectory;
            const auto receipt = TelegramInspectorProcess::recordStarted(*child, port, root);
            if (receipt.isEmpty()) {
                child->kill(); // Never leave an untracked managed child active.
                fail(QStringLiteral("Cannot persist private tunnel ownership; the tunnel was stopped."));
                return;
            }
            ownedReceipt = receipt;
            QObject::connect(child, &QProcess::finished, child, [port, root, receipt] {
                TelegramInspectorProcess::forgetReceipt(port, root, receipt);
            });
            phase = Discovering;
            discovery.start(options.discoveryDelayMs);
        });
        QObject::connect(child, &QProcess::errorOccurred, q, [this, child](QProcess::ProcessError error) {
            if (process == child && error == QProcess::FailedToStart)
                fail(QStringLiteral("Cannot start ngrok; check its installation and configuration."));
        });
        QObject::connect(child, &QProcess::finished, q, [this, child](int, QProcess::ExitStatus) {
            if (process != child) return;
            TelegramInspectorProcess::forgetReceipt(quint16(options.inspectorUrl.port()),
                options.ownershipDirectory, std::exchange(ownedReceipt, {}));
            process = nullptr;
            child->deleteLater();
            if (phase == Stopping) finishStop();
            else fail(QStringLiteral("The managed tunnel process exited."));
        });
        QStringList arguments = options.argumentPrefix;
        arguments << QStringLiteral("http") << QStringLiteral("http://127.0.0.1:%1").arg(options.port);
        child->start(options.executable, arguments);
    }
    void restartInspector()
    {
        QStringList arguments = options.argumentPrefix;
        arguments << QStringLiteral("http") << QStringLiteral("http://127.0.0.1:%1").arg(options.port);
        orphan = TelegramInspectorProcess::findOrphan(options.executable, arguments,
            quint16(options.inspectorUrl.port()), options.ownershipDirectory);
        if (!orphan || !orphan->terminate()) {
            fail(QStringLiteral("Cannot safely close the inspector: no verified orphan ngrok owns this port."));
            return;
        }
        phase = RestartingInspector;
        restartElapsed.start();
        escalated = false;
        restartPoll.start();
    }
    void pollRestart()
    {
        if (phase != RestartingInspector || !orphan) return;
        if (!orphan->alive()) {
            restartPoll.stop();
            orphan->forgetExitedReceipt();
            orphan.reset();
            // Verify closure asynchronously. A replacement listener never inherits
            // the user's consent to terminate the original process instance.
            auto *socket = new QTcpSocket(q);
            probe = socket;
            const auto run = generation;
            QObject::connect(socket, &QTcpSocket::connected, q, [this, socket, run] {
                if (probe != socket || generation != run || phase != RestartingInspector) return;
                fail(QStringLiteral("The inspector port is still occupied after closing the orphan."));
            });
            QObject::connect(socket, &QTcpSocket::errorOccurred, q, [this, socket, run](QAbstractSocket::SocketError error) {
                if (probe != socket || generation != run || phase != RestartingInspector) return;
                probe = nullptr; socket->deleteLater();
                if (error != QAbstractSocket::ConnectionRefusedError) {
                    fail(QStringLiteral("Cannot verify that the inspector port closed.")); return;
                }
                phase = Preflight;
                launch();
            });
            socket->connectToHost(QStringLiteral("127.0.0.1"), quint16(options.inspectorUrl.port()));
            return;
        }
        if (restartElapsed.elapsed() >= 2500) {
            fail(QStringLiteral("The verified orphan inspector did not close in time.")); return;
        }
        if (!escalated && restartElapsed.elapsed() >= 1000) {
            escalated = true;
            if (!orphan->terminate(true)) {
                fail(QStringLiteral("The orphan inspector identity changed; forced close was refused."));
            }
        }
    }
    void preflight()
    {
        auto *socket = new QTcpSocket(q);
        probe = socket;
        QObject::connect(socket, &QTcpSocket::connected, q, [this, socket] {
            if (probe != socket || phase != Preflight) return;
            probe = nullptr;
            socket->disconnect(q); socket->abort(); socket->deleteLater();
            deadline.stop();
            phase = AwaitingInspectorDecision;
            const auto ids = clients.keys();
            const auto run = generation;
            const QPointer<TelegramWebhookHost> guard(q);
            for (const auto &id : ids) {
                if (clients.contains(id)) emit q->inspectorConflict(id);
                if (!guard || generation != run || phase != AwaitingInspectorDecision) return;
            }
        });
        QObject::connect(socket, &QTcpSocket::errorOccurred, q, [this, socket](QAbstractSocket::SocketError error) {
            if (probe != socket || phase != Preflight) return;
            probe = nullptr;
            socket->deleteLater();
            if (error == QAbstractSocket::ConnectionRefusedError) launch();
            else fail(QStringLiteral("Cannot verify exclusive ngrok inspector ownership."));
        });
        socket->connectToHost(options.inspectorUrl.host(), quint16(options.inspectorUrl.port()));
    }
    void discover()
    {
        if (phase != Discovering) return;
        if (elapsed.elapsed() >= options.startupTimeoutMs) {
            fail(QStringLiteral("Tunnel startup timed out.")); return;
        }
        QNetworkRequest request(options.inspectorUrl);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        auto *response = network.get(request);
        reply = response;
        response->setReadBufferSize(16 * 1024);
        struct Response { QByteArray body; bool oversized = false; bool timedOut = false; QElapsedTimer elapsed; };
        const auto data = std::make_shared<Response>();
        data->elapsed.start();
        auto *requestDeadline = new QTimer(response);
        requestDeadline->setSingleShot(true);
        requestDeadline->setTimerType(Qt::PreciseTimer);
        QObject::connect(requestDeadline, &QTimer::timeout, response, [response, data] {
            data->timedOut = true;
            response->abort();
        });
        requestDeadline->start(1500);
        const auto consume = [response, data] {
            if (!response->isOpen() || data->oversized || data->timedOut) return;
            data->body += response->readAll();
            if (data->body.size() > 64 * 1024) { data->oversized = true; response->abort(); }
        };
        QObject::connect(response, &QIODevice::readyRead, q, consume);
        const auto run = generation;
        QObject::connect(response, &QNetworkReply::finished, q, [this, response, data, consume, run, requestDeadline] {
            if (reply != response || generation != run) { response->deleteLater(); return; }
            reply = nullptr;
            requestDeadline->stop();
            consume();
            response->deleteLater();
            if (phase != Discovering) return;
            if (data->timedOut || data->elapsed.elapsed() >= 1500) {
                fail(QStringLiteral("Ngrok inspector request timed out.")); return;
            }
            if (elapsed.elapsed() >= options.startupTimeoutMs) {
                fail(QStringLiteral("Tunnel startup timed out.")); return;
            }
            const auto status = response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (data->oversized) { fail(QStringLiteral("Ngrok inspector response exceeded the size limit.")); return; }
            if (response->error() == QNetworkReply::ConnectionRefusedError && status == 0) {
                discovery.start(100); return;
            }
            if (response->error() != QNetworkReply::NoError || status != 200) {
                fail(QStringLiteral("Ngrok inspector request failed; redirects are not accepted.")); return;
            }
            const auto doc = QJsonDocument::fromJson(data->body);
            if (!doc.isObject() || !doc.object().value("tunnels").isArray()) {
                fail(QStringLiteral("Ngrok inspector returned invalid tunnel data.")); return;
            }
            const auto tunnels = doc.object().value("tunnels").toArray();
            if (tunnels.isEmpty()) { discovery.start(100); return; }
            QList<QUrl> matches;
            for (const auto &item : tunnels) {
                const auto tunnel = item.toObject();
                const auto rawAddress = tunnel.value("config").toObject().value("addr").toString();
                const QUrl address(rawAddress.contains(QStringLiteral("://"))
                    ? rawAddress : QStringLiteral("http://") + rawAddress, QUrl::StrictMode);
                if (tunnel.value("proto").toString() == QStringLiteral("https")
                    && address.isValid() && address.scheme() == QStringLiteral("http")
                    && (address.host() == QStringLiteral("127.0.0.1") || address.host() == QStringLiteral("localhost"))
                    && address.port() == options.port && address.userInfo().isEmpty()
                    && !address.hasQuery() && !address.hasFragment()
                    && (address.path().isEmpty() || address.path() == QStringLiteral("/")))
                    matches.append(QUrl(tunnel.value("public_url").toString(), QUrl::StrictMode));
            }
            if (matches.size() != 1 || !matches.first().isValid()
                || matches.first().scheme() != QStringLiteral("https") || matches.first().host().isEmpty()
                || !matches.first().userInfo().isEmpty() || matches.first().hasQuery() || matches.first().hasFragment()
                || (!matches.first().path().isEmpty() && matches.first().path() != QStringLiteral("/"))) {
                fail(QStringLiteral("Ngrok did not provide one safe matching HTTPS tunnel.")); return;
            }
            publicUrl = matches.first();
            phase = Operational;
            deadline.stop();
            const auto ids = clients.keys();
            const QPointer<TelegramWebhookHost> guard(q);
            for (const auto &id : ids) {
                notifyReady(id);
                if (!guard || generation != run || phase != Operational) return;
            }
        });
    }
};

TelegramWebhookHost::TelegramWebhookHost(QObject *parent) : QObject(parent), d(std::make_unique<State>(this)) {}
TelegramWebhookHost::~TelegramWebhookHost()
{
    d->cancelIo();
    d->closeServer();
    if (d->process) {
        // Never invoke QProcess's blocking running-child destructor on the UI thread.
        auto *child = d->process.data();
        child->disconnect(this);
        // Keep this short-lived reaper independent of application child teardown:
        // QProcess's destructor waits for a running child, even after terminate().
        child->setParent(nullptr);
        if (child->state() == QProcess::NotRunning) child->deleteLater();
        else {
            connect(child, &QProcess::finished, child, &QObject::deleteLater);
            connect(child, &QProcess::started, child, &QProcess::terminate);
            connect(child, &QProcess::readyReadStandardOutput, child, [child] { child->readAllStandardOutput(); });
            connect(child, &QProcess::readyReadStandardError, child, [child] { child->readAllStandardError(); });
            if (auto *application = QCoreApplication::instance())
                connect(application, &QCoreApplication::aboutToQuit, child, &QProcess::kill);
            if (QCoreApplication::closingDown() || !QCoreApplication::instance()) child->kill();
            else {
                child->terminate();
                QTimer::singleShot(1000, child, [child] { if (child->state() != QProcess::NotRunning) child->kill(); });
            }
        }
    }
}
TelegramWebhookHost *TelegramWebhookHost::instance()
{
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    static QPointer<TelegramWebhookHost> host;
    if (!host) host = new TelegramWebhookHost(QCoreApplication::instance());
    return host;
}
void TelegramWebhookHost::acquire(const QString &id, const Options &options, AdmissionHandler admit,
    QString secretToken, RequestHandler receive)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (d->phase == State::Stopping) { emit failed(id, QStringLiteral("Tunnel is stopping; retry later.")); return; }
    if (id.isEmpty() || options.port == 0 || options.executable.isEmpty() || !safeInspector(options.inspectorUrl)
        || options.discoveryDelayMs < 0 || options.startupTimeoutMs <= options.discoveryDelayMs) {
        emit failed(id, QStringLiteral("Invalid private tunnel configuration.")); return;
    }
    if (d->phase != State::Idle && !compatible(d->options, options)) {
        emit failed(id, QStringLiteral("An incompatible managed tunnel is already active.")); return;
    }
    if ((!secretToken.isEmpty() && !TelegramWebhookManager::validSecret(secretToken))
        || (receive && secretToken.isEmpty())) {
        emit failed(id, QStringLiteral("Invalid private webhook secret.")); return;
    }
    if (!d->clients.contains(id))
        d->clients.insert(id, {QStringLiteral("/telegram/") + randomSecret(),
            secretToken.isEmpty() ? randomSecret() : std::move(secretToken), std::move(admit), std::move(receive)});
    if (d->phase == State::AwaitingInspectorDecision) { emit inspectorConflict(id); return; }
    if (d->phase != State::Idle) { d->notifyReady(id); return; }
    d->options = options;
    ++d->generation;
    d->phase = State::Preflight;
    d->elapsed.start();
    d->deadline.start(options.startupTimeoutMs);
    d->preflight();
}
void TelegramWebhookHost::resolveInspectorConflict(const QString &id, InspectorConflictAction action)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (d->phase != State::AwaitingInspectorDecision || !d->clients.contains(id)) return;
    if (action == InspectorConflictAction::Cancel) {
        d->clients.remove(id);
        const QPointer<TelegramWebhookHost> guard(this);
        if (d->clients.isEmpty()) d->stop();
        if (guard) emit failed(id, QStringLiteral("Tunnel startup cancelled."));
        return;
    }
    if (action == InspectorConflictAction::Restart) {
        d->elapsed.restart();
        d->deadline.start(d->options.startupTimeoutMs);
        d->restartInspector();
    }
}
void TelegramWebhookHost::release(const QString &id)
{
    Q_ASSERT(thread() == QThread::currentThread());
    d->clients.remove(id);
    if (!d->clients.isEmpty()) { emit released(id); return; }
    if (!d->waitingRelease.contains(id)) d->waitingRelease.append(id);
    if (d->phase == State::Stopping) return;
    d->stop();
}
bool TelegramWebhookHost::ownsHealthUrl(const QUrl &url) const
{
    return d->phase == State::Operational && url.isValid() && url.scheme() == QStringLiteral("http")
        && (url.host() == QStringLiteral("localhost") || url.host() == QStringLiteral("127.0.0.1"))
        && url.port(80) == d->options.port && url.path() == QStringLiteral("/health")
        && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
}
QByteArray TelegramWebhookHost::healthSecret() const { return d->health.toLatin1(); }
