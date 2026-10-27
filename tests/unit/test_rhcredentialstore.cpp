#include "rhcredentialstore.h"

#if __has_include(<qt6keychain/keychain.h>)
#include <qt6keychain/keychain.h>
#elif __has_include(<qtkeychain/keychain.h>)
#include <qtkeychain/keychain.h>
#else
#include <keychain.h>
#endif

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QtTest>
#include <memory>

class TestRHCredentialStore : public QObject
{
    Q_OBJECT
private slots:
    void destructiveCompletion_data()
    {
        QTest::addColumn<int>("operation");
        QTest::newRow("read") << 0;
        QTest::newRow("write") << 1;
        QTest::newRow("remove") << 2;
    }

    void destructiveCompletion()
    {
        QFETCH(int, operation);
        QPointer<QKeychain::Job> job;
        int offlineStarts = 0;
        int destroyed = 0;
        int callbacks = 0;
        auto *owner = new QObject;
        auto *store = new RHQtKeychainStore([&](QKeychain::Job *created) {
            ++offlineStarts;
            job = created; // Deliberately never call Job::start(): no OS backend.
            connect(created, &QObject::destroyed, this, [&] { ++destroyed; });
        }, owner);
        QPointer<RHQtKeychainStore> storeGuard(store);
        auto reservation = std::make_shared<int>(1);
        std::weak_ptr<int> reservationGuard(reservation);
        RHCredentialStore::Completion done = [&, reservation](bool ok, const QString &error) {
            ++callbacks;
            QVERIFY(ok);
            QVERIFY(error.isEmpty());
            delete owner; // Also deletes the store and any still-parented job.
            owner = nullptr;
            QVERIFY(storeGuard.isNull());
            QVERIFY2(job, "completion must detach the job before destroying its store");
            // A completion may spin an event loop: do not queue job deletion yet.
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QVERIFY(job);
            QCOMPARE(destroyed, 0);
        };
        if (operation == 0)
            store->read(7, [done](bool ok, QString token, QString error) {
                QVERIFY(token.isEmpty());
                done(ok, error);
            });
        else if (operation == 1)
            store->write(7, QStringLiteral("offline-test-token"), done);
        else
            store->remove(7, done);
        done = {};
        reservation.reset();
        QCOMPARE(offlineStarts, 1);
        QVERIFY(job);
        QCOMPARE(job->parent(), store);
        QCOMPARE(job->service(), QStringLiteral("PublicsTradingInterface.RobinHood"));
        QCOMPARE(job->key(), QStringLiteral("RobinHoodAuth7"));
        QVERIFY(!job->autoDelete());
        QVERIFY(!job->insecureFallback());

        // Exercise upstream emitFinished(), including its post-signal d access.
        job->emitFinishedWithError(QKeychain::NoError, QString());
        QCOMPARE(callbacks, 1);
        QVERIFY(job);
        QCOMPARE(destroyed, 0);
        QVERIFY(reservationGuard.expired());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(job.isNull());
        QCOMPARE(destroyed, 1);
        QCOMPARE(offlineStarts, 1);
    }
};

QTEST_GUILESS_MAIN(TestRHCredentialStore)
#include "test_rhcredentialstore.moc"
