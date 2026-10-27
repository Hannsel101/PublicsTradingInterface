#define RH_NATIVE_SPIKE_UI_TEST
#include "main.cpp"
#include <QtTest/QtTest>
#include <QScrollBar>

class NativeAuthUiTest final : public QObject {
    Q_OBJECT
private slots:
    void credentialsRequireExplicitConsent()
    {
        NativeAuthClient client;
        SpikeWindow window(client, QUuid::createUuid().toString(QUuid::WithoutBraces));
        auto *email = window.findChild<QLineEdit *>("email");
        auto *password = window.findChild<QLineEdit *>("password");
        auto *consent = window.findChild<QCheckBox *>("consent");
        auto *login = window.findChild<QPushButton *>("login");
        QVERIFY(email && password && consent && login);
        QCOMPARE(client.state(), NativeAuthClient::State::Idle);
        QVERIFY(!login->isEnabled());
        email->setText("fixture@example.test");
        password->setText("fixture-password");
        QVERIFY(!login->isEnabled());
        consent->setChecked(true);
        QVERIFY(login->isEnabled());
        email->clear();
        QVERIFY(!login->isEnabled());
        QCOMPARE(client.state(), NativeAuthClient::State::Idle);
        QCOMPARE(password->echoMode(), QLineEdit::Password);
    }
    void compactLayoutAndSafeInitialState()
    {
        NativeAuthClient client;
        SpikeWindow window(client, QUuid::createUuid().toString(QUuid::WithoutBraces));
        window.resize(360, 640);
        window.show();
        QTest::qWait(30);
        auto *scroll = window.findChild<QScrollArea *>();
        QVERIFY(scroll);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QVERIFY(window.width() <= 360);
        auto *status = window.findChild<QLabel *>("status");
        auto *summary = window.findChild<QLabel *>("grantSummary");
        auto *code = window.findChild<QLineEdit *>("challengeCode");
        QVERIFY(status && summary && code);
        QCOMPARE(status->textFormat(), Qt::PlainText);
        QCOMPARE(summary->textFormat(), Qt::PlainText);
        QVERIFY(summary->text().isEmpty());
        QVERIFY(!code->isEnabled());
        QVERIFY(!client.busy());
        QCOMPARE(client.state(), NativeAuthClient::State::Idle);
        window.close();
        QVERIFY(!client.busy());
    }
};
QTEST_MAIN(NativeAuthUiTest)
#include "test_native_auth_ui.moc"
