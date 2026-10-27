#include "rhcredentialstore.h"

#include <QScopedPointer>
#include <utility>

#if __has_include(<qt6keychain/keychain.h>)
#include <qt6keychain/keychain.h>
#elif __has_include(<qtkeychain/keychain.h>)
#include <qtkeychain/keychain.h>
#else
#include <keychain.h>
#endif

namespace {
QString keyFor(int index)
{
    return QStringLiteral("RobinHoodAuth%1").arg(index);
}
const QString service = QStringLiteral("PublicsTradingInterface.RobinHood");

template<typename Callback, typename... Args>
void completeJob(QKeychain::Job *job, Callback &done, Args &&...args)
{
    if (!done)
        return;
    // QtKeychain's emitFinished() accesses the job after emitting finished.
    // A callback can delete the store (or its owner), so detach first. Only
    // schedule deletion after the callback and its captures have been released:
    // the callback may itself process deferred-delete events.
    job->setParent(nullptr);
    const QScopedPointer<QKeychain::Job, QScopedPointerDeleteLater> cleanup(job);
    auto completion = std::exchange(done, {});
    completion(std::forward<Args>(args)...);
}
}

RHQtKeychainStore::RHQtKeychainStore(QObject *parent) : RHCredentialStore(parent) {}

RHQtKeychainStore::RHQtKeychainStore(JobStarter starter, QObject *parent)
    : RHCredentialStore(parent), m_starter(std::move(starter)) {}

void RHQtKeychainStore::startJob(QKeychain::Job *job)
{
    // An injected synchronous completion may destroy this store.
    const auto starter = m_starter;
    if (starter)
        starter(job);
    else
        job->start();
}

void RHQtKeychainStore::write(int index, const QString &token, Completion done)
{
    auto *job = new QKeychain::WritePasswordJob(service, this);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(keyFor(index));
    job->setTextData(token);
    connect(job, &QKeychain::WritePasswordJob::finished, this,
            [job, done = std::move(done)]() mutable {
        const bool success = job->error() == QKeychain::NoError;
        completeJob(job, done, success,
                    success ? QString() : QStringLiteral("System keychain could not save this session."));
    });
    startJob(job);
}

void RHQtKeychainStore::read(int index, ReadCompletion done)
{
    auto *job = new QKeychain::ReadPasswordJob(service, this);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(keyFor(index));
    connect(job, &QKeychain::ReadPasswordJob::finished, this,
            [job, done = std::move(done)]() mutable {
        const bool found = job->error() == QKeychain::NoError;
        const bool success = found || job->error() == QKeychain::EntryNotFound;
        completeJob(job, done, success, found ? job->textData() : QString(),
             success ? QString() : QStringLiteral("Saved session token is unavailable. Reauthenticate."));
    });
    startJob(job);
}

void RHQtKeychainStore::remove(int index, Completion done)
{
    auto *job = new QKeychain::DeletePasswordJob(service, this);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(keyFor(index));
    connect(job, &QKeychain::DeletePasswordJob::finished, this,
            [job, done = std::move(done)]() mutable {
        const bool success = job->error() == QKeychain::NoError
                          || job->error() == QKeychain::EntryNotFound;
        completeJob(job, done, success,
                    success ? QString() : QStringLiteral("System keychain could not remove this session."));
    });
    startJob(job);
}
