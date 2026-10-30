#include "publicauthclient.h"

#include <algorithm>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QPointer>

namespace {
class ApiKeySettings : public QSettings
{
public:
    ApiKeySettings()
        : QSettings(QStringLiteral("PublicsTradingInterface"),
                    QStringLiteral("PublicsTradingInterface"))
    {
    }
};
}

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
    : PublicAuthClient(secretKey, keychainServiceName, settingsGroupName,
                       QUrl(QStringLiteral("https://api.public.com/")), {}, parent)
{
}

PublicAuthClient::PublicAuthClient(const QString &secretKey,
                                   const QString &keychainServiceName,
                                   const QString &settingsGroupName,
                                   const QUrl &authBaseUrl,
                                   CredentialLoader credentialLoader,
                                   QObject *parent)
    : PublicAuthClient(secretKey, keychainServiceName, settingsGroupName, authBaseUrl,
                       std::move(credentialLoader), {}, parent)
{
}

PublicAuthClient::PublicAuthClient(const QString &secretKey,
                                   const QString &keychainServiceName,
                                   const QString &settingsGroupName,
                                   const QUrl &authBaseUrl,
                                   CredentialLoader credentialLoader,
                                   KeyIndexReader keyIndexReader,
                                   QObject *parent)
    : QObject(parent),
      m_secretKey(secretKey),
      m_keychainService(keychainServiceName),
      m_settingsGroup(settingsGroupName),
      m_authBaseUrl(authBaseUrl),
      m_credentialLoader(std::move(credentialLoader)),
      m_keyIndexReader(std::move(keyIndexReader))
{
    m_manager = new QNetworkAccessManager(this);
    setApiKeyReady(!m_secretKey.trimmed().isEmpty());
    ApiKeySettings settings;
    settings.beginGroup(m_settingsGroup);
    // Previous versions wrote to a no-organization settings file on Android.
    // Its values survive restarts, but sync() reports AccessError there.
    if (!settings.value(QStringLiteral("legacySettingsImported")).toBool()) {
        QSettings legacy;
        legacy.beginGroup(m_settingsGroup);
        if (!settings.contains(QStringLiteral("labels")) && legacy.contains(QStringLiteral("labels")))
            settings.setValue(QStringLiteral("labels"), legacy.value(QStringLiteral("labels")));
        legacy.beginGroup(QStringLiteral("nicknames"));
        settings.beginGroup(QStringLiteral("nicknames"));
        for (const QString &label : legacy.childKeys()) {
            if (!settings.contains(label))
                settings.setValue(label, legacy.value(label));
        }
        settings.endGroup();
        settings.setValue(QStringLiteral("legacySettingsImported"), true);
        settings.sync();
        if (settings.status() != QSettings::NoError)
            qWarning() << "Could not migrate local API key metadata.";
    }
    settings.beginGroup(QStringLiteral("nicknames"));
    for (const QString &label : settings.childKeys()) {
        const QString nickname = settings.value(label).toString().trimmed();
        if (!nickname.isEmpty())
            m_apiKeyNicknames.insert(label, nickname);
    }
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
    const quint64 generation = ++m_tokenRequestGeneration;
    if (!apiKeyReady() || secretKey().isEmpty()) {
        qWarning() << "Authorization skipped: no API key is selected or loaded.";
        setSessionActive(false);
        emit authorizationFailed(QStringLiteral("API key is not ready."));
        return;
    }

    QUrl url = m_authBaseUrl.resolved(QUrl(QStringLiteral("userapiauthservice/personal/access-tokens")));
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);

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
    const QString apiKeyLabel = m_selectedApiKeyLabel;
    connect(reply, &QNetworkReply::finished, this, [this, reply, apiKeyLabel, generation]()
            {
                handleReply(reply, apiKeyLabel, generation);
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
    if (apiKeyIndexLoading()) {
        return secretKeys();
    }

    ApiKeySettings settings;
    settings.beginGroup(m_settingsGroup);
    QStringList labels = normalizedStoredApiKeyLabels(settings.value("labels").toStringList());
    settings.setValue("labels", labels);
    settings.endGroup();

    // Reserve discovery before publishing even the local fallback model. Direct
    // observers must never mistake that model for a completed all-key scope.
    m_apiKeyIndexLoading = true;
    m_apiKeyIndexState = KeyIndexState::Loading;
    ++m_keyIndexGeneration;
    const QPointer<PublicAuthClient> guard(this);
    emit apiKeyIndexStateChanged();
    if (!guard) return {};
    emit apiKeyIndexLoadingChanged();
    if (!guard) return {};
    setApiKeyError({});
    if (!guard) return {};
    setSecretKeys(labels);
    if (!guard) return {};
    readApiKeyLabelIndex(labels);
    return guard ? secretKeys() : QStringList{};
}

QString PublicAuthClient::keychainKeyForLabel(const QString &label) const
{
    return label.trimmed();
}

QString PublicAuthClient::serializeApiKeyLabels(const QStringList &labels)
{
    QJsonArray serializedLabels;
    for (const QString &label : normalizedStoredApiKeyLabels(labels)) {
        serializedLabels.append(label);
    }
    return QString::fromUtf8(QJsonDocument(serializedLabels).toJson(QJsonDocument::Compact));
}

bool PublicAuthClient::deserializeApiKeyLabels(
    const QString &serializedLabels,
    QStringList *labels)
{
    if (!labels) {
        return false;
    }
    labels->clear();

    const QJsonDocument document = QJsonDocument::fromJson(serializedLabels.toUtf8());
    if (!document.isArray()) {
        return false;
    }

    QStringList parsedLabels;
    for (const QJsonValue &value : document.array()) {
        if (!value.isString()) {
            return false;
        }
        parsedLabels.append(value.toString());
    }

    const QStringList normalizedLabels = normalizedStoredApiKeyLabels(parsedLabels);
    if (normalizedLabels.size() != parsedLabels.size()) {
        return false;
    }
    *labels = normalizedLabels;
    return true;
}

void PublicAuthClient::readApiKeyLabelIndex(const QStringList &localLabels)
{
    const QPointer<PublicAuthClient> guard(this);
    const quint64 generation = m_keyIndexGeneration;
    const KeyIndexCallback done = [guard, generation, localLabels](QKeychain::Error error, const QString &data) {
        if (!guard || guard->m_keyIndexGeneration != generation
            || guard->m_apiKeyIndexState != KeyIndexState::Loading) return;
        guard->applyApiKeyLabelIndex(error, data, localLabels);
    };
    if (m_keyIndexReader) {
        const auto reader = m_keyIndexReader;
        reader(done);
        return;
    }
    auto *job = new QKeychain::ReadPasswordJob(m_keychainService, this);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(QString::fromLatin1(keychainLabelIndexKey));
    connect(job, &QKeychain::ReadPasswordJob::finished, this, [job, done]() {
        const auto error = job->error();
        const QString data = job->textData();
        job->deleteLater();
        done(error, data);
    });
    job->start();
}

void PublicAuthClient::applyApiKeyLabelIndex(QKeychain::Error error, const QString &serializedLabels,
                                            const QStringList &localLabels)
{
    QStringList storedLabels;
    if (error == QKeychain::NoError) {
        if (!deserializeApiKeyLabels(serializedLabels, &storedLabels)) {
            finishApiKeyDiscovery(false, QStringLiteral("Saved API key labels could not be read safely."));
            return;
        }
    } else if (error != QKeychain::EntryNotFound) {
        finishApiKeyDiscovery(false, QStringLiteral("Could not load saved API key labels from the system keychain."));
        return;
    }

    // Only confirmed absence permits the legacy local-metadata fallback.
    QStringList labels = storedLabels;
    labels.append(localLabels);
    labels = normalizedStoredApiKeyLabels(labels);
    ApiKeySettings settings;
    settings.beginGroup(m_settingsGroup);
    settings.setValue("labels", labels);
    settings.endGroup();
    const QPointer<PublicAuthClient> guard(this);
    setSecretKeys(labels);
    if (!guard) return;
    if (labels != storedLabels) {
        writeApiKeyLabelIndex(labels, [guard](bool succeeded) {
            if (guard) guard->finishApiKeyDiscovery(succeeded,
                succeeded ? QString() : QStringLiteral("Could not preserve the API key list in the system keychain."));
        });
    } else {
        finishApiKeyDiscovery(true);
    }
}

void PublicAuthClient::finishApiKeyDiscovery(bool succeeded, const QString &error)
{
    // Publish the terminal state and sanitized error before idle can admit work.
    m_apiKeyIndexState = succeeded ? KeyIndexState::Ready : KeyIndexState::Failed;
    const QPointer<PublicAuthClient> guard(this);
    setApiKeyError(error);
    if (!guard) return;
    emit apiKeyIndexStateChanged();
    if (!guard) return;
    setApiKeyIndexLoading(false); // Last action: observers may start a replacement.
}

void PublicAuthClient::writeApiKeyLabelIndex(
    const QStringList &labels,
    const std::function<void(bool)> &completion)
{
    auto *job = new QKeychain::WritePasswordJob(m_keychainService, this);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(QString::fromLatin1(keychainLabelIndexKey));
    job->setTextData(serializeApiKeyLabels(labels));

    connect(job, &QKeychain::WritePasswordJob::finished, this, [this, job, completion]() {
        const bool succeeded = job->error() == QKeychain::NoError;
        if (!succeeded) {
            qWarning() << "Failed to store the API key label index:" << job->errorString();
            setApiKeyError(QStringLiteral("Could not preserve the API key list in the system keychain."));
        }
        if (completion) {
            completion(succeeded);
        }
        job->deleteLater();
    });
    job->start();
}

void PublicAuthClient::deleteApiKeyFromKeychain(
    const QString &label,
    const std::function<void(bool)> &completion)
{
    auto *job = new QKeychain::DeletePasswordJob(m_keychainService, this);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(keychainKeyForLabel(label));

    connect(job, &QKeychain::DeletePasswordJob::finished, this, [job, label, completion]() {
        const bool succeeded = job->error() == QKeychain::NoError
                               || job->error() == QKeychain::EntryNotFound;
        if (!succeeded)
            qWarning() << "Failed to remove API key" << label << ":" << job->errorString();
        if (completion) {
            completion(succeeded);
        }
        job->deleteLater();
    });
    job->start();
}

void PublicAuthClient::readApiKeyFromKeychain(const QString &label)
{
    const quint64 generation = ++m_apiKeyReadGeneration;
    ++m_tokenRequestGeneration;
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

    if (m_credentialLoader) {
        m_credentialLoader(trimmedLabel, [this, trimmedLabel, generation](const QString &secret, const QString &error) {
            if (m_selectedApiKeyLabel != trimmedLabel || m_apiKeyReadGeneration != generation) return;
            if (!error.isEmpty() || secret.trimmed().isEmpty()) {
                m_secretKey.clear();
                setApiKeyReady(false);
                setApiKeyLoading(false);
                setApiKeyError(error.isEmpty() ? QStringLiteral("Could not load the API key.") : error);
                emit secretKeyChanged();
            } else {
                applyLoadedApiKey(trimmedLabel, generation, secret);
            }
        });
        return;
    }

    auto *job = new QKeychain::ReadPasswordJob(m_keychainService, this);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(keychainKeyForLabel(trimmedLabel));

    connect(job, &QKeychain::ReadPasswordJob::finished, this, [this, job, trimmedLabel, generation]() {
        if (m_selectedApiKeyLabel != trimmedLabel || m_apiKeyReadGeneration != generation) {
            job->deleteLater();
            return;
        }

        if (job->error() == QKeychain::NoError) {
            applyLoadedApiKey(trimmedLabel, generation, job->textData());
            job->deleteLater();
            return;
        }

        setApiKeyLoading(false);
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

bool PublicAuthClient::applyLoadedApiKey(
    const QString &label,
    quint64 generation,
    const QString &apiKey)
{
    if (m_selectedApiKeyLabel != label || m_apiKeyReadGeneration != generation) {
        return false;
    }

    setApiKeyLoading(false);
    m_secretKey = apiKey;
    setApiKeyReady(!m_secretKey.trimmed().isEmpty());
    emit secretKeyChanged();
    return true;
}

bool PublicAuthClient::storeNextApiKey(const QString &userName, const QString &apiKey)
{
    Q_UNUSED(userName)

    const QString trimmedApiKey = apiKey.trimmed();
    if (trimmedApiKey.isEmpty()) {
        qWarning() << "Refusing to store an empty API key.";
        return false;
    }
    if (apiKeyIndexLoading()) {
        setApiKeyError(QStringLiteral("Please wait for the saved API key list to finish loading."));
        return false;
    }

    QStringList labels = secretKeys();
    ApiKeySettings settings;
    settings.beginGroup(m_settingsGroup);
    labels.append(settings.value("labels").toStringList());
    settings.endGroup();
    labels = normalizedStoredApiKeyLabels(labels);
    const QString label = nextApiKeyLabel(labels);
    setApiKeyIndexLoading(true);

    auto *job = new QKeychain::WritePasswordJob(m_keychainService, this);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(keychainKeyForLabel(label));
    job->setTextData(trimmedApiKey);

    connect(job, &QKeychain::WritePasswordJob::finished, this, [this, job, label, trimmedApiKey]() {
        if (job->error() != QKeychain::NoError) {
            qWarning() << "Failed to store API key" << label << ":" << job->errorString();
            setApiKeyError(QStringLiteral("Could not store the API key in the system keychain."));
            setApiKeyIndexLoading(false);
            job->deleteLater();
            return;
        }

        QStringList labels = secretKeys();
        labels.append(label);
        labels = normalizedStoredApiKeyLabels(labels);

        writeApiKeyLabelIndex(labels, [this, label, trimmedApiKey, labels](bool succeeded) {
            if (!succeeded) {
                deleteApiKeyFromKeychain(label, [this](bool) {
                    setApiKeyIndexLoading(false);
                });
                return;
            }

            setApiKeyIndexLoading(false);

            ApiKeySettings settings;
            settings.beginGroup(m_settingsGroup);
            settings.setValue("labels", labels);
            settings.endGroup();

            setSecretKeys(labels);
            setSelectedApiKeyLabel(label);
            ++m_tokenRequestGeneration;
            m_secretKey = trimmedApiKey;
            setApiKeyError(QString());
            setApiKeyLoading(false);
            setApiKeyReady(true);
            emit secretKeyChanged();
        });
        job->deleteLater();
    });
    job->start();

    return true;
}

bool PublicAuthClient::removeStoredApiKey(const QString &label)
{
    if (apiKeyIndexLoading() || !m_secretKeys.contains(label))
        return false;

    ApiKeySettings settings;
    settings.beginGroup(m_settingsGroup);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setApiKeyError(QStringLiteral("Could not read local API key metadata."));
        return false;
    }
    const bool hadLocalLabels = settings.contains(QStringLiteral("labels"));
    const QVariant previousLabels = settings.value(QStringLiteral("labels"));
    const QString nicknamePath = QStringLiteral("nicknames/") + label;
    const bool hadNickname = settings.contains(nicknamePath);
    const QVariant previousNickname = settings.value(nicknamePath);

    QStringList remaining = m_secretKeys;
    remaining.removeAll(label);
    setApiKeyError({});
    setApiKeyIndexLoading(true);
    if (m_selectedApiKeyLabel == label) {
        clearUserSession();
        setSecretKey({});
    }

    const auto fail = [this, label](const QString &message) {
        setApiKeyError(message);
        setApiKeyIndexLoading(false);
        emit apiKeyRemovalFinished(label, false, message);
    };
    const auto restoreSettings = [this, label, hadLocalLabels, previousLabels, hadNickname,
                                  previousNickname]() {
        ApiKeySettings rollback;
        rollback.beginGroup(m_settingsGroup);
        if (hadLocalLabels)
            rollback.setValue(QStringLiteral("labels"), previousLabels);
        else
            rollback.remove(QStringLiteral("labels"));
        const QString path = QStringLiteral("nicknames/") + label;
        if (hadNickname)
            rollback.setValue(path, previousNickname);
        else
            rollback.remove(path);
        rollback.sync();
        return rollback.status() == QSettings::NoError;
    };

    deleteApiKeyFromKeychain(label, [this, label, remaining, nicknamePath, fail, restoreSettings](bool deleted) {
        if (!deleted) {
            fail(QStringLiteral("Could not remove the API key from the system keychain."));
            return;
        }

        ApiKeySettings settings;
        settings.beginGroup(m_settingsGroup);
        settings.setValue(QStringLiteral("labels"), remaining);
        settings.remove(nicknamePath);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            const bool restored = restoreSettings();
            fail(restored ? QStringLiteral("Could not update local API key metadata. Retry deletion.")
                          : QStringLiteral("Could not restore local API key metadata. Check saved keys before continuing."));
            return;
        }

        writeApiKeyLabelIndex(remaining, [this, label, remaining, fail, restoreSettings](bool saved) {
            if (!saved) {
                const bool restored = restoreSettings();
                fail(restored ? QStringLiteral("Could not update the keychain label list. Retry deletion.")
                              : QStringLiteral("Could not restore local API key metadata. Check saved keys before continuing."));
                return;
            }
            setSecretKeys(remaining);
            if (m_apiKeyNicknames.remove(label))
                emit apiKeyNicknamesChanged();
            setApiKeyError({});
            setApiKeyIndexLoading(false);
            emit apiKeyRemovalFinished(label, true, {});
        });
    });
    return true;
}

void PublicAuthClient::clearUserSession()
{
    ++m_tokenRequestGeneration;
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

void PublicAuthClient::handleReply(QNetworkReply *reply,
                                   const QString &apiKeyLabel,
                                   quint64 generation)
{
    if (m_selectedApiKeyLabel != apiKeyLabel
        || m_tokenRequestGeneration != generation) {
        reply->deleteLater();
        return;
    }

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

        applyAccessToken(apiKeyLabel, generation, accessToken);
    }
    else
    {
        qWarning() << "Authorization failed:" << reply->errorString();
        setSessionActive(false);
        emit authorizationFailed(QStringLiteral("Authorization failed: %1").arg(reply->errorString()));
    }

    reply->deleteLater();
}

bool PublicAuthClient::applyAccessToken(const QString &apiKeyLabel,
                                        quint64 generation,
                                        const QString &accessToken)
{
    if (m_selectedApiKeyLabel != apiKeyLabel
        || m_tokenRequestGeneration != generation) {
        return false;
    }

    setSessionActive(!accessToken.isEmpty());
    if (accessToken.isEmpty())
        emit authorizationFailed(QStringLiteral("Authorization returned no access token."));
    else
        emit tokenReceived(accessToken);
    return true;
}

QStringList PublicAuthClient::secretKeys() const
{
    return m_secretKeys;
}

QVariantMap PublicAuthClient::apiKeyNicknames() const
{
    return m_apiKeyNicknames;
}

QString PublicAuthClient::apiKeyDisplayName(const QString &label) const
{
    return m_apiKeyNicknames.value(label, label).toString();
}

bool PublicAuthClient::refreshApiKeyNicknames()
{
    ApiKeySettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError) return false;
    settings.beginGroup(m_settingsGroup);
    settings.beginGroup(QStringLiteral("nicknames"));
    QVariantMap nicknames;
    for (const auto &label : settings.childKeys()) {
        const QString nickname = settings.value(label).toString().trimmed();
        if (!nickname.isEmpty()) nicknames.insert(label, nickname);
    }
    if (m_apiKeyNicknames != nicknames) {
        m_apiKeyNicknames = nicknames;
        emit apiKeyNicknamesChanged();
    }
    return true;
}

bool PublicAuthClient::setApiKeyNickname(const QString &label, const QString &nickname)
{
    if (apiKeyIndexLoading() || !m_secretKeys.contains(label))
        return false;

    const QString trimmedNickname = nickname.trimmed();
    ApiKeySettings settings;
    settings.beginGroup(m_settingsGroup);
    settings.beginGroup(QStringLiteral("nicknames"));
    if (trimmedNickname.isEmpty())
        settings.remove(label);
    else
        settings.setValue(label, trimmedNickname);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return false;

    if (trimmedNickname.isEmpty()) {
        if (m_apiKeyNicknames.remove(label))
            emit apiKeyNicknamesChanged();
    } else if (m_apiKeyNicknames.value(label).toString() != trimmedNickname) {
        m_apiKeyNicknames.insert(label, trimmedNickname);
        emit apiKeyNicknamesChanged();
    }
    return true;
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

    if (m_secretKey == trimmedKeyOrLabel && m_selectedApiKeyLabel.isEmpty())
        return;

    ++m_apiKeyReadGeneration;
    ++m_tokenRequestGeneration;
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

bool PublicAuthClient::apiKeyIndexLoading() const
{
    return m_apiKeyIndexLoading;
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

void PublicAuthClient::setApiKeyIndexLoading(bool loading)
{
    if (m_apiKeyIndexLoading == loading)
        return;
    m_apiKeyIndexLoading = loading;
    emit apiKeyIndexLoadingChanged();
}

void PublicAuthClient::setApiKeyError(const QString &error)
{
    if (m_apiKeyError == error)
        return;
    m_apiKeyError = error;
    emit apiKeyErrorChanged();
}
