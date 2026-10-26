#include "rhsessionmanager.h"
#include "rhnetworkengine.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>
#include <cmath>

namespace {
constexpr qint64 maxLifetime = 30LL * 24 * 60 * 60;

bool pushType(const QString &type)
{
    return type == QStringLiteral("prompt") || type == QStringLiteral("push")
        || type == QStringLiteral("device_approval");
}

bool rejected(const QString &status)
{
    return status == QStringLiteral("rejected") || status == QStringLiteral("denied")
        || status == QStringLiteral("expired") || status == QStringLiteral("failed");
}

bool approved(const QJsonObject &response)
{
    return response.value(QStringLiteral("type_context")).toObject().value(QStringLiteral("result")).toString()
                   == QStringLiteral("workflow_status_approved")
        || response.value(QStringLiteral("verification_workflow")).toObject()
                   .value(QStringLiteral("workflow_status")).toString()
                   == QStringLiteral("workflow_status_approved");
}
}

void RHSessionManager::sendAuthRequest(const QString &endpoint, const QVariantMap &values,
                                       const QVariantMap &body, AuthResponse onReply)
{
    QString problem;
    QNetworkReply *reply = m_network.send(endpoint, values, {}, body, &problem);
    if (!reply) {
        failLogin(problem);
        return;
    }
    m_reply = reply;
    const quint64 generation = m_authGeneration;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation, onReply = std::move(onReply)]() {
        if (generation != m_authGeneration) {
            reply->deleteLater();
            return;
        }
        if (m_approvalPending && m_approvalClock.elapsed() >= m_approvalTimeoutMs) {
            reply->deleteLater();
            m_reply = nullptr;
            failLogin(QStringLiteral("Device approval timed out."));
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        m_reply = nullptr;
        onReply(document.isObject() ? document.object() : QJsonObject{}, ok, status);
    });
}

bool RHSessionManager::signIn(const QString &email, const QString &password, const QString &mfaCode)
{
    if (m_busy || !m_loaded || !m_network.isConfigured() || email.trimmed().isEmpty() || password.isEmpty()) {
        setError(QStringLiteral("Enter your account email and password after loading network configuration."));
        return false;
    }
    if (m_deviceEmail != email.trimmed()) {
        m_deviceToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_deviceEmail = email.trimmed();
    }
    m_loginName = email.trimmed();
    m_loginBody = {{QStringLiteral("username"), m_loginName},
                   {QStringLiteral("password"), password},
                   {QStringLiteral("expires_in"), maxLifetime},
                   {QStringLiteral("device_token"), m_deviceToken},
                   {QStringLiteral("mfa_code"), mfaCode.trimmed().isEmpty() ? QVariant() : QVariant(mfaCode.trimmed())}};
    ++m_authGeneration;
    setError({});
    setBusy(true);
    sendAuthRequest(QStringLiteral("Login"), {}, m_loginBody,
                    [this](const QJsonObject &response, bool ok, int status) {
        handleLogin(response, ok, status, false);
    });
    return m_busy;
}

void RHSessionManager::handleLogin(const QJsonObject &response, bool ok, int status, bool retry)
{
    if (ok && response.contains(QStringLiteral("access_token"))) {
        finishLogin(response);
        return;
    }
    if (!retry && status >= 200 && status < 500) {
        const QString workflowId = response.value(QStringLiteral("verification_workflow"))
                                       .toObject().value(QStringLiteral("id")).toString();
        const QJsonValue challenge = response.value(QStringLiteral("challenge"));
        const QJsonObject challengeObject = challenge.toObject();
        const QString challengeId = response.value(QStringLiteral("challenge_id")).toString().isEmpty()
            ? challengeObject.value(QStringLiteral("id")).toString().isEmpty()
                  ? challenge.toString() : challengeObject.value(QStringLiteral("id")).toString()
            : response.value(QStringLiteral("challenge_id")).toString();
        const QString type = challengeObject.value(QStringLiteral("type")).toString().isEmpty()
            ? response.value(QStringLiteral("mfa_type")).toString().isEmpty()
                  ? response.value(QStringLiteral("type")).toString()
                  : response.value(QStringLiteral("mfa_type")).toString()
            : challengeObject.value(QStringLiteral("type")).toString();
        if (!workflowId.isEmpty() || (!challengeId.isEmpty() && pushType(type))) {
            beginApproval(workflowId, challengeId);
            return;
        }
    }
    failLogin(retry ? QStringLiteral("Device approval did not complete sign-in. Please try again.")
                    : QStringLiteral("Sign-in failed. Check credentials or the requested verification method."));
}

void RHSessionManager::beginApproval(const QString &workflowId, const QString &challengeId)
{
    m_approvalPending = true;
    m_approvalSecondsRemaining = (m_approvalTimeoutMs + 999) / 1000;
    m_approvalClock.start();
    m_countdownTimer.start();
    m_approvalTimeout.start(m_approvalTimeoutMs);
    m_challengeId = challengeId;
    emit approvalChanged();
    if (workflowId.isEmpty()) {
        readPrompt();
        return;
    }
    const QVariantMap body{{QStringLiteral("device_id"), m_deviceToken},
                           {QStringLiteral("flow"), QStringLiteral("suv")},
                           {QStringLiteral("input"), QVariantMap{{QStringLiteral("workflow_id"), workflowId}}}};
    sendAuthRequest(QStringLiteral("BeginVerification"), {}, body,
                    [this](const QJsonObject &response, bool ok, int) {
        m_machineId = response.value(QStringLiteral("id")).toString();
        if (!ok || m_machineId.isEmpty()) {
            failLogin(QStringLiteral("Could not begin device approval."));
            return;
        }
        m_nextPoll = ApprovalStep::Inquiry;
        schedulePoll();
    });
}

void RHSessionManager::schedulePoll()
{
    if (m_approvalPending)
        m_pollTimer.start(m_approvalPollMs);
}

void RHSessionManager::pollApproval()
{
    if (!m_approvalPending || m_reply)
        return;
    switch (m_nextPoll) {
    case ApprovalStep::Inquiry: readInquiry(); break;
    case ApprovalStep::Prompt: readPrompt(); break;
    case ApprovalStep::Continue: continueVerification(); break;
    }
}

void RHSessionManager::readInquiry()
{
    sendAuthRequest(QStringLiteral("ReadVerificationInquiry"),
                    {{QStringLiteral("machine_id"), m_machineId}}, {},
                    [this](const QJsonObject &response, bool ok, int status) {
        if (!ok) {
            if (status == 0 || status >= 500) {
                m_nextPoll = ApprovalStep::Inquiry;
                schedulePoll();
            } else {
                failLogin(QStringLiteral("Device approval inquiry was rejected."));
            }
            return;
        }
        if (response.contains(QStringLiteral("access_token"))) {
            finishLogin(response);
            return;
        }
        if (approved(response)) {
            retryLogin();
            return;
        }
        if (rejected(response.value(QStringLiteral("verification_workflow")).toObject()
                         .value(QStringLiteral("workflow_status")).toString())) {
            failLogin(QStringLiteral("Device approval was declined or expired."));
            return;
        }
        const QJsonObject challenge = response.value(QStringLiteral("context")).toObject()
                                          .value(QStringLiteral("sheriff_challenge")).toObject();
        const QString statusText = challenge.value(QStringLiteral("status")).toString();
        if (rejected(statusText)) {
            failLogin(QStringLiteral("Device approval was declined or expired."));
        } else if (statusText == QStringLiteral("validated")) {
            continueVerification();
        } else if (!challenge.isEmpty()) {
            const QString type = challenge.value(QStringLiteral("type")).toString();
            m_challengeId = challenge.value(QStringLiteral("id")).toString();
            if (!pushType(type) || m_challengeId.isEmpty()) {
                failLogin(QStringLiteral("This verification method is not supported by device approval."));
                return;
            }
            m_nextPoll = ApprovalStep::Prompt;
            readPrompt();
        } else {
            m_nextPoll = ApprovalStep::Inquiry;
            schedulePoll();
        }
    });
}

void RHSessionManager::readPrompt()
{
    sendAuthRequest(QStringLiteral("CheckPrompt"),
                    {{QStringLiteral("challenge_id"), m_challengeId}}, {},
                    [this](const QJsonObject &response, bool ok, int status) {
        if (!ok) {
            if (status == 0 || status >= 500) {
                m_nextPoll = ApprovalStep::Prompt;
                schedulePoll();
            } else {
                failLogin(QStringLiteral("Device approval check was rejected."));
            }
            return;
        }
        if (response.contains(QStringLiteral("access_token"))) {
            finishLogin(response);
            return;
        }
        const QString state = response.value(QStringLiteral("challenge_status")).toString().isEmpty()
            ? response.value(QStringLiteral("status")).toString()
            : response.value(QStringLiteral("challenge_status")).toString();
        if (state == QStringLiteral("validated") || state == QStringLiteral("approved")) {
            if (m_machineId.isEmpty())
                retryLogin();
            else
                continueVerification();
        } else if (rejected(state)) {
            failLogin(QStringLiteral("Device approval was declined or expired."));
        } else {
            m_nextPoll = ApprovalStep::Prompt;
            schedulePoll();
        }
    });
}

void RHSessionManager::continueVerification()
{
    const QVariantMap body{{QStringLiteral("sequence"), 0},
                           {QStringLiteral("user_input"), QVariantMap{{QStringLiteral("status"),
                                                                        QStringLiteral("continue")}}}};
    sendAuthRequest(QStringLiteral("ContinueVerification"),
                    {{QStringLiteral("machine_id"), m_machineId}}, body,
                    [this](const QJsonObject &response, bool ok, int status) {
        if (!ok) {
            if (status == 0 || status >= 500) {
                m_nextPoll = ApprovalStep::Continue;
                schedulePoll();
            } else {
                failLogin(QStringLiteral("Device approval workflow was rejected."));
            }
            return;
        }
        if (response.contains(QStringLiteral("access_token"))) {
            finishLogin(response);
        } else if (approved(response)) {
            retryLogin();
        } else if (rejected(response.value(QStringLiteral("verification_workflow")).toObject()
                                   .value(QStringLiteral("workflow_status")).toString())) {
            failLogin(QStringLiteral("Device approval was declined or expired."));
        } else {
            m_nextPoll = ApprovalStep::Continue;
            schedulePoll();
        }
    });
}

void RHSessionManager::retryLogin()
{
    sendAuthRequest(QStringLiteral("Login"), {}, m_loginBody,
                    [this](const QJsonObject &response, bool ok, int status) {
        handleLogin(response, ok, status, true);
    });
}

void RHSessionManager::finishLogin(const QJsonObject &response)
{
    const double seconds = response.value(QStringLiteral("expires_in")).toDouble(-1);
    const QString token = response.value(QStringLiteral("access_token")).toString();
    const QString type = response.value(QStringLiteral("token_type")).toString();
    if (token.isEmpty() || type.compare(QStringLiteral("Bearer"), Qt::CaseInsensitive) != 0
        || !std::isfinite(seconds) || seconds < 1) {
        failLogin(QStringLiteral("Sign-in did not return a usable token and expiry."));
        return;
    }
    const bool activate = m_approvalPending;
    const QString name = m_loginName;
    endApproval();
    setBusy(false);
    m_activateAfterApproval = activate;
    if (!saveAuthenticatedSession(name, token,
                                  static_cast<qint64>(qMin(seconds, static_cast<double>(maxLifetime))), type))
        m_activateAfterApproval = false;
}

void RHSessionManager::endApproval()
{
    ++m_authGeneration;
    m_pollTimer.stop();
    m_approvalTimeout.stop();
    m_countdownTimer.stop();
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_loginBody.clear();
    m_loginName.clear();
    m_machineId.clear();
    m_challengeId.clear();
    m_deviceToken.clear();
    m_deviceEmail.clear();
    if (m_approvalPending || m_approvalSecondsRemaining) {
        m_approvalPending = false;
        m_approvalSecondsRemaining = 0;
        emit approvalChanged();
    }
}

void RHSessionManager::failLogin(const QString &message)
{
    endApproval();
    m_activateAfterApproval = false;
    setError(message);
    setBusy(false);
}

void RHSessionManager::cancelSignIn()
{
    if (!m_approvalPending)
        return;
    endApproval();
    setError({});
    setBusy(false);
}
