#pragma once

class QJsonObject;

// Debug-only, authenticated incoming-command diagnostics; never log headers or raw bodies.
void logTelegramIncomingUpdate(const QJsonObject &update, bool webhook);
