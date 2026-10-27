#pragma once

#include <QObject>
#include <QString>
#include <functional>

namespace QKeychain { class Job; }

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
    // ok=true with an empty token means confirmed absence. Backend/permission
    // failures return ok=false; callers must not treat them as missing entries.
    virtual void read(int index, ReadCompletion done) = 0;
    virtual void remove(int index, Completion done) = 0;
};

class RHQtKeychainStore final : public RHCredentialStore
{
    Q_OBJECT
public:
    explicit RHQtKeychainStore(QObject *parent = nullptr);
    // C++-only dispatch seam: tests can finish real jobs without starting an OS backend.
    using JobStarter = std::function<void(QKeychain::Job *)>;
    explicit RHQtKeychainStore(JobStarter starter, QObject *parent = nullptr);
    void write(int index, const QString &token, Completion done) override;
    void read(int index, ReadCompletion done) override;
    void remove(int index, Completion done) override;
private:
    void startJob(QKeychain::Job *job);
    JobStarter m_starter;
};
