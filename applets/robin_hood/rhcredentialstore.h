#pragma once

#include <QObject>
#include <QString>
#include <functional>

// Asynchronous secret-store seam: no credentials are exposed through QML roles.
class RHCredentialStore : public QObject
{
    Q_OBJECT
public:
    using Completion = std::function<void(bool, QString)>;
    using ReadCompletion = std::function<void(bool, QString, QString)>;
    using QObject::QObject;
    ~RHCredentialStore() override = default;

    virtual void write(int index, const QString &token, Completion done) = 0;
    virtual void read(int index, ReadCompletion done) = 0;
    virtual void remove(int index, Completion done) = 0;
};

class RHQtKeychainStore final : public RHCredentialStore
{
    Q_OBJECT
public:
    explicit RHQtKeychainStore(QObject *parent = nullptr);
    void write(int index, const QString &token, Completion done) override;
    void read(int index, ReadCompletion done) override;
    void remove(int index, Completion done) override;
};
