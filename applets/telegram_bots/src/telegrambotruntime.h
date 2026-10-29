#pragma once

#include <QObject>
#include <QUrl>
#include <memory>

// Explicitly started, desktop-only Telegram acknowledgement worker. No commands,
// brokerage integration, persistent credentials or QML-visible token state.
class TelegramBotRuntime final : public QObject
{
    Q_OBJECT
public:
    explicit TelegramBotRuntime(QObject *parent = nullptr);
    explicit TelegramBotRuntime(QUrl apiRoot, QObject *parent = nullptr);
    ~TelegramBotRuntime() override;

    bool active() const;
    bool starting() const;
    void start(QString token);
    void stop();

signals:
    void stateChanged();
    void errorOccurred(const QString &message);

private:
    struct State;
    std::unique_ptr<State> d;
};
