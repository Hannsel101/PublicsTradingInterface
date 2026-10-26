#include "rhcredentialstore.h"

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
}

RHQtKeychainStore::RHQtKeychainStore(QObject *parent) : RHCredentialStore(parent) {}

void RHQtKeychainStore::write(int index, const QString &token, Completion done)
{
    auto *job = new QKeychain::WritePasswordJob(service, this);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(keyFor(index));
    job->setTextData(token);
    connect(job, &QKeychain::WritePasswordJob::finished, this,
            [job, done = std::move(done)]() {
        const bool success = job->error() == QKeychain::NoError;
        done(success, success ? QString() : QStringLiteral("System keychain could not save this session."));
        job->deleteLater();
    });
    job->start();
}

void RHQtKeychainStore::read(int index, ReadCompletion done)
{
    auto *job = new QKeychain::ReadPasswordJob(service, this);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(keyFor(index));
    connect(job, &QKeychain::ReadPasswordJob::finished, this,
            [job, done = std::move(done)]() {
        const bool found = job->error() == QKeychain::NoError;
        const bool success = found || job->error() == QKeychain::EntryNotFound;
        done(success, found ? job->textData() : QString(),
             success ? QString() : QStringLiteral("Saved session token is unavailable. Reauthenticate."));
        job->deleteLater();
    });
    job->start();
}

void RHQtKeychainStore::remove(int index, Completion done)
{
    auto *job = new QKeychain::DeletePasswordJob(service, this);
    job->setAutoDelete(false);
    job->setInsecureFallback(false);
    job->setKey(keyFor(index));
    connect(job, &QKeychain::DeletePasswordJob::finished, this,
            [job, done = std::move(done)]() {
        const bool success = job->error() == QKeychain::NoError
                          || job->error() == QKeychain::EntryNotFound;
        done(success, success ? QString() : QStringLiteral("System keychain could not remove this session."));
        job->deleteLater();
    });
    job->start();
}
