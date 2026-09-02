#include "publicauthclient.h"

#include <algorithm>
#include <QRegularExpression>
#include <QSet>

PublicAuthClient::PublicAuthClient(const QString &secretKey, QObject *parent)
    : PublicAuthClient(secretKey,
                       QString::fromLatin1(defaultKeychainService),
                       QString::fromLatin1(defaultSettingsGroup),
                       parent)
{
}

PublicAuthClient::PublicAuthClient(const QString &secretKey,
                                   const QString &keychainServiceName,
                                   const QString &settingsGroupName,
                                   QObject *parent)
    : QObject(parent),
      m_secretKey(secretKey),
      m_keychainService(keychainServiceName),
      m_settingsGroup(settingsGroupName)
{
    m_manager = new QNetworkAccessManager(this);
    setApiKeyReady(!m_secretKey.trimmed().isEmpty());
}

bool PublicAuthClient::sessionActive() const
{
    return m_sessionActive;
}

void PublicAuthClient::setSessionActive(bool newSessionActive)
{
    if (m_sessionActive == newSessionActive)
        return;
    m_sessionActive = newSessionActive;
    emit sessionActiveChanged();
}

void PublicAuthClient::requestToken()
{
    if (!apiKeyReady() || secretKey().isEmpty()) {
        qWarning() << "Authorization skipped: no API key is selected or loaded.";
        setSessionActive(false);
        return;
    }

    QUrl url("https://api.public.com/userapiauthservice/personal/access-tokens");
    QNetworkRequest request(url);

    // Set required headers
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    // Construct the JSON payload
    QJsonObject jsonObj;
    jsonObj["secret"] = secretKey();
    jsonObj["validityInMinutes"] = 5; // Adjust as needed

    QJsonDocument doc(jsonObj);
    QByteArray data = doc.toJson();

    // Send asynchronous POST request
    QNetworkReply *reply = m_manager->post(request, data);

    // Connect reply finished signal to our slot/lambda
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
            {
                handleReply(reply);
            });
}

QStringList PublicAuthClient::normalizedStoredApiKeyLabels(const QStringList &labels)
{
    QSet<int> usedIndexes;
    const QRegularExpression labelPattern(QStringLiteral("^PublicsApiKey(\\d+)$"));

    for (const QString &label : labels) {
        const QRegularExpressionMatch match = labelPattern.match(label.trimmed());
        if (!match.hasMatch()) {
            continue;
        }
        bool ok = false;
        const int index = match.captured(1).toInt(&ok);
        if (ok && index >= 0) {
            usedIndexes.insert(index);
        }
    }

    QList<int> sortedIndexes = usedIndexes.values();
    std::sort(sortedIndexes.begin(), sortedIndexes.end());

    QStringList normalizedLabels;
    normalizedLabels.reserve(sortedIndexes.size());
    for (int index : sortedIndexes) {
        normalizedLabels.append(QStringLiteral("PublicsApiKey%1").arg(index));
    }
    return normalizedLabels;
}

QString PublicAuthClient::nextApiKeyLabel(const QStringList &labels)
{
    const QStringList normalizedLabels = normalizedStoredApiKeyLabels(labels);
    QSet<int> usedIndexes;
    const QRegularExpression labelPattern(QStringLiteral("^PublicsApiKey(\\d+)$"));

    for (const QString &label : normalizedLabels) {
        const QRegularExpressionMatch match = labelPattern.match(label);
        if (!match.hasMatch()) {
            continue;
        }
        usedIndexes.insert(match.captured(1).toInt());
    }

    int nextIndex = 0;
    while (usedIndexes.contains(nextIndex)) {
        ++nextIndex;
    }
    return QStringLiteral("PublicsApiKey%1").arg(nextIndex);
}

QStringList PublicAuthClient::listStoredApiKeys()
{
    QSettings settings;
    settings.beginGroup(m_settingsGroup);
    QStringList labels = normalizedStoredApiKeyLabels(settings.value("labels").toStringList());
    settings.setValue("labels", labels);
    settings.endGroup();

    setSecretKeys(labels);
    return secretKeys();
}

QString PublicAuthClient::keychainKeyForLabel(const QString &label) const
{
    return label.trimmed();
}

void PublicAuthClient::readApiKeyFromKeychain(const QString &label)
{
    const QString trimmedLabel = label.trimmed();
    if (trimmedLabel.isEmpty()) {
        m_secretKey.clear();
        emit secretKeyChanged();
        setSelectedApiKeyLabel(QString());
        setApiKeyLoading(false);
        setApiKeyReady(false);
        return;
    }

    setSelectedApiKeyLabel(trimmedLabel);
    setApiKeyError(QString());
    setApiKeyReady(false);
    setApiKeyLoading(true);

    auto *job = new QKeychain::ReadPasswordJob(m_keychainService);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(keychainKeyForLabel(trimmedLabel));

    connect(job, &QKeychain::ReadPasswordJob::finished, this, [this, job, trimmedLabel]() {
        setApiKeyLoading(false);
        if (job->error() == QKeychain::NoError) {
            m_secretKey = job->textData();
            setApiKeyReady(!m_secretKey.trimmed().isEmpty());
            emit secretKeyChanged();
            job->deleteLater();
            return;
        }

        qWarning() << "Failed to load API key" << trimmedLabel << ":" << job->errorString();
        if (m_selectedApiKeyLabel == trimmedLabel) {
            m_secretKey.clear();
            setApiKeyReady(false);
            setApiKeyError(QStringLiteral("Could not load %1 from the system keychain.").arg(trimmedLabel));
            emit secretKeyChanged();
        }
        job->deleteLater();
    });
    job->start();
}

bool PublicAuthClient::storeNextApiKey(const QString &userName, const QString &apiKey)
{
    Q_UNUSED(userName)

    const QString trimmedApiKey = apiKey.trimmed();
    if (trimmedApiKey.isEmpty()) {
        qWarning() << "Refusing to store an empty API key.";
        return false;
    }

    QStringList labels = secretKeys();
    QSettings settings;
    settings.beginGroup(m_settingsGroup);
    labels.append(settings.value("labels").toStringList());
    settings.endGroup();
    labels = normalizedStoredApiKeyLabels(labels);
    const QString label = nextApiKeyLabel(labels);

    auto *job = new QKeychain::WritePasswordJob(m_keychainService);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(keychainKeyForLabel(label));
    job->setTextData(trimmedApiKey);

    connect(job, &QKeychain::WritePasswordJob::finished, this, [this, job, label, trimmedApiKey]() {
        if (job->error() != QKeychain::NoError) {
            qWarning() << "Failed to store API key" << label << ":" << job->errorString();
            setApiKeyError(QStringLiteral("Could not store the API key in the system keychain."));
            job->deleteLater();
            return;
        }

        QSettings settings;
        settings.beginGroup(m_settingsGroup);
        QStringList labels = normalizedStoredApiKeyLabels(settings.value("labels").toStringList());
        if (!labels.contains(label)) {
            labels.append(label);
            labels = normalizedStoredApiKeyLabels(labels);
            settings.setValue("labels", labels);
        }
        settings.endGroup();

        setSecretKeys(labels);
        setSelectedApiKeyLabel(label);
        m_secretKey = trimmedApiKey;
        setApiKeyError(QString());
        setApiKeyLoading(false);
        setApiKeyReady(true);
        emit secretKeyChanged();
        job->deleteLater();
    });
    job->start();

    return true;
}

void PublicAuthClient::clearUserSession()
{
    setSessionActive(false);
    if (!m_manager) return;

    // 1. Wipe Qt's underlying connection pool & authentication cache
    // This destroys any open sockets or cached session states.
    m_manager->clearAccessCache();       // Flushes auth tokens & SSL tickets
    m_manager->clearConnectionCache();   // Drops active TCP connections to the API server

    // 2. Purge the HTTP Cookie Jar (if the API sets any cookie data)
    if (m_manager->cookieJar())
    {
        QNetworkCookieJar *emptyJar = new QNetworkCookieJar(m_manager);
        m_manager->setCookieJar(emptyJar); // Overwriting deletes the old cookie cache
    }

    // 3. Wipe physical disk cache
    if (m_manager->cache())
    {
        m_manager->cache()->clear();
    }
}

void PublicAuthClient::handleReply(QNetworkReply *reply)
{
    if (reply->error() == QNetworkReply::NoError)
    {
        QByteArray responseData = reply->readAll();
        QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);
        QJsonObject jsonObj = jsonDoc.object();

        // Extract the access token (adjust key name if API differs slightly)
        QString accessToken = jsonObj["accessToken"].toString();
        if (accessToken.isEmpty())
        {
            // Sometimes it might just be under "token" or similar depending on the exact schema
            accessToken = jsonObj["token"].toString();
        }

        setSessionActive(!accessToken.isEmpty());
        emit tokenReceived(accessToken);
    }
    else
    {
        qWarning() << "Authorization failed:" << reply->errorString();
        setSessionActive(false);
    }

    reply->deleteLater();
}

QStringList PublicAuthClient::secretKeys() const
{
    return m_secretKeys;
}

void PublicAuthClient::setSecretKeys(const QStringList &newSecretKeys)
{
    const QStringList normalizedKeys = normalizedStoredApiKeyLabels(newSecretKeys);
    if (m_secretKeys == normalizedKeys)
        return;
    m_secretKeys = normalizedKeys;
    emit secretKeysChanged();
}

QString PublicAuthClient::secretKey() const
{
    return m_secretKey;
}

void PublicAuthClient::setSecretKey(const QString &newSecretKey)
{
    const QString trimmedKeyOrLabel = newSecretKey.trimmed();
    if (m_secretKeys.contains(trimmedKeyOrLabel)) {
        if (m_selectedApiKeyLabel == trimmedKeyOrLabel && m_apiKeyReady) {
            return;
        }
        readApiKeyFromKeychain(trimmedKeyOrLabel);
        return;
    }

    if (m_secretKey == trimmedKeyOrLabel)
        return;

    m_secretKey = trimmedKeyOrLabel;
    setSelectedApiKeyLabel(QString());
    setApiKeyLoading(false);
    setApiKeyReady(!m_secretKey.isEmpty());
    emit secretKeyChanged();
}

QString PublicAuthClient::selectedApiKeyLabel() const
{
    return m_selectedApiKeyLabel;
}

bool PublicAuthClient::apiKeyReady() const
{
    return m_apiKeyReady;
}

bool PublicAuthClient::apiKeyLoading() const
{
    return m_apiKeyLoading;
}

QString PublicAuthClient::apiKeyError() const
{
    return m_apiKeyError;
}

void PublicAuthClient::setSelectedApiKeyLabel(const QString &label)
{
    if (m_selectedApiKeyLabel == label)
        return;
    m_selectedApiKeyLabel = label;
    emit selectedApiKeyLabelChanged();
}

void PublicAuthClient::setApiKeyReady(bool ready)
{
    if (m_apiKeyReady == ready)
        return;
    m_apiKeyReady = ready;
    emit apiKeyReadyChanged();
}

void PublicAuthClient::setApiKeyLoading(bool loading)
{
    if (m_apiKeyLoading == loading)
        return;
    m_apiKeyLoading = loading;
    emit apiKeyLoadingChanged();
}

void PublicAuthClient::setApiKeyError(const QString &error)
{
    if (m_apiKeyError == error)
        return;
    m_apiKeyError = error;
    emit apiKeyErrorChanged();
}
