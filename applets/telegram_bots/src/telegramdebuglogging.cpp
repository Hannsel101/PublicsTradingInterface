#include "telegramdebuglogging.h"

#if PUBLICS_TELEGRAM_DEBUG_PAYLOADS
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSet>

namespace {
Q_LOGGING_CATEGORY(incomingCommands, "publics.telegram.incoming")

bool recognizedText(const QString &text)
{
    if (text.size() > 4096) return false;
    static const QRegularExpression syntax(QStringLiteral(
        "\\A(?:/?status|/?publics_auto_(?:buy|sell)[ \t]+[A-Za-z][A-Za-z0-9]{0,9}(?:[.-][A-Za-z0-9]{1,4})?"
        "(?:[ \t]*,[ \t]*[A-Za-z][A-Za-z0-9]{0,9}(?:[.-][A-Za-z0-9]{1,4})?){0,49})\\z"));
    static const QRegularExpression robinhoodSyntax(QStringLiteral(
        "\\A/?robinhood_auto_(?:buy|sell)[ \t]+[A-Za-z][A-Za-z0-9]{0,9}(?:[.-][A-Za-z0-9]{1,4})?"
        "(?:[ \t]*,[ \t]*[A-Za-z][A-Za-z0-9]{0,9}(?:[.-][A-Za-z0-9]{1,4})?)*\\z"));
    static const QRegularExpression combinedSyntax(QStringLiteral(
        "\\A/?auto_(?:buy|sell)[ \t]+[A-Za-z]{1,10}(?:[ \t]*,[ \t]*[A-Za-z]{1,10})*\\z"));
    const auto trimmed = text.trimmed();
    return (text.size() <= 1024 && syntax.match(trimmed).hasMatch())
        || robinhoodSyntax.match(trimmed).hasMatch() || combinedSyntax.match(trimmed).hasMatch();
}

QJsonValue redact(const QJsonValue &value, const QString &path, int depth, int &budget)
{
    if (depth > 6 || --budget < 0) return QStringLiteral("[truncated]");
    if (value.isObject()) {
        static const QSet<QString> fields{
            "update_id", "message", "message_id", "date", "from", "chat", "text", "is_bot",
            "type", "entities", "offset", "length", "reply_to_message", "user"};
        static const QSet<QString> privateFields{
            "id", "first_name", "last_name", "username", "language_code", "phone_number", "email",
            "password", "token", "bot_token", "secret_token", "access_token", "api_key", "authorization",
            "url", "caption", "file_id", "file_unique_id"};
        const auto object = value.toObject();
        QJsonObject result;
        int visited = 0, omitted = 0;
        for (auto it = object.begin(); it != object.end(); ++it, ++visited) {
            if (visited >= 32 || budget <= 0) { omitted += object.size() - visited; break; }
            const auto key = it.key();
            if (privateFields.contains(key)) result.insert(key, QStringLiteral("[redacted]"));
            else if (fields.contains(key)) {
                const auto child = path.isEmpty() ? key : path + QLatin1Char('.') + key;
                result.insert(key, redact(it.value(), child, depth + 1, budget));
            } else ++omitted; // Unknown keys themselves can contain private data.
        }
        if (omitted) result.insert(QStringLiteral("_redacted_field_count"), omitted);
        return result;
    }
    if (value.isArray()) {
        const auto array = value.toArray();
        QJsonArray result;
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (i >= 16 || budget <= 0) { result.append(QStringLiteral("[truncated]")); break; }
            result.append(redact(array.at(i), path + QStringLiteral("[]"), depth + 1, budget));
        }
        return result;
    }
    if (path == QStringLiteral("message.text"))
        return recognizedText(value.toString()) ? value : QJsonValue(QStringLiteral("[redacted unrecognized command]"));
    if (value.isDouble() && (path == QStringLiteral("update_id") || path == QStringLiteral("message.message_id")
        || path == QStringLiteral("message.date") || path == QStringLiteral("message.entities[].offset")
        || path == QStringLiteral("message.entities[].length"))) return value;
    if (value.isBool() && path == QStringLiteral("message.from.is_bot")) return value;
    if (path == QStringLiteral("message.chat.type") && value == QJsonValue(QStringLiteral("private"))) return value;
    return QStringLiteral("[redacted]");
}
}
#endif

void logTelegramIncomingUpdate(const QJsonObject &update, bool webhook)
{
#if PUBLICS_TELEGRAM_DEBUG_PAYLOADS
    if (!incomingCommands().isDebugEnabled()) return;
    int budget = 256;
    QJsonObject record{{"format", "Telegram Update JSON"}, {"transport", webhook ? "webhook" : "polling"},
        {"payload", redact(update, {}, 0, budget)}};
    auto bytes = QJsonDocument(record).toJson(QJsonDocument::Indented);
    if (bytes.size() > 8192) {
        record.insert(QStringLiteral("payload"), QStringLiteral("[truncated]"));
        bytes = QJsonDocument(record).toJson(QJsonDocument::Indented);
    }
    qCDebug(incomingCommands).noquote().nospace() << QString::fromUtf8(bytes);
#else
    (void)update;
    (void)webhook;
#endif
}
