#include "native_auth_client.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

class SpikeWindow final : public QWidget {
public:
    SpikeWindow(NativeAuthClient &client, QString deviceToken)
        : m_client(client), m_deviceToken(std::move(deviceToken))
    {
        setWindowTitle(tr("Robinhood Native Auth — Compatibility Test"));
        resize(520, 720);
        auto *outer = new QVBoxLayout(this);
        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        auto *body = new QWidget(scroll);
        auto *layout = new QVBoxLayout(body);
        auto *title = label(tr("Native login compatibility test"), body);
        auto font = title->font();
        font.setBold(true);
        font.setPointSize(16);
        title->setFont(font);
        layout->addWidget(title);
        layout->addWidget(label(tr("Standalone experiment — not the trading app. No WebView, no orders, no saved passwords or tokens. Successful token acquisition does not verify identity or trading permission."), body));
        layout->addWidget(label(tr("This unofficial password-based flow is not recommended by modern OAuth standards. Robinhood may reject it or require verification. Nothing is sent until you explicitly press Test login."), body));
        auto *form = new QFormLayout;
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        m_email = input("email", body);
        m_email->setInputMethodHints(Qt::ImhEmailCharactersOnly | Qt::ImhNoPredictiveText);
        m_password = input("password", body);
        m_password->setEchoMode(QLineEdit::Password);
        m_password->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
        m_mfa = input("mfa", body);
        m_mfa->setEchoMode(QLineEdit::Password);
        m_mfa->setMaxLength(32);
        m_mfa->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
        form->addRow(tr("Email / username"), m_email);
        form->addRow(tr("Password"), m_password);
        form->addRow(tr("Authenticator code (optional)"), m_mfa);
        layout->addLayout(form);
        layout->addWidget(label(tr("Test login sends your credentials directly to Robinhood."), body));
        m_consent = new QCheckBox(tr("I consent to this credential test."), body);
        m_consent->setObjectName("consent");
        layout->addWidget(m_consent);
        m_login = new QPushButton(tr("Test login"), body);
        m_login->setObjectName("login");
        layout->addWidget(m_login);
        m_code = input("challengeCode", body);
        m_code->setEchoMode(QLineEdit::Password);
        m_code->setMaxLength(32);
        m_code->setPlaceholderText(tr("Email / SMS verification code"));
        m_code->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
        layout->addWidget(m_code);
        m_submit = new QPushButton(tr("Submit verification code"), body);
        layout->addWidget(m_submit);
        m_cancel = new QPushButton(tr("Cancel attempt"), body);
        layout->addWidget(m_cancel);
        m_status = label({}, body);
        m_status->setObjectName("status");
        m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(m_status);
        m_summary = label({}, body);
        m_summary->setObjectName("grantSummary");
        layout->addWidget(m_summary);
        layout->addWidget(label(tr("Only a non-secret device identifier is retained between runs. Credential memory clearing is best effort; Qt/network libraries may retain temporary copies. No raw responses, tokens or credentials are printed."), body));
        layout->addStretch();
        scroll->setWidget(body);
        outer->addWidget(scroll);
        connect(m_email, &QLineEdit::textChanged, this, [this] { updateControls(); });
        connect(m_password, &QLineEdit::textChanged, this, [this] { updateControls(); });
        connect(m_consent, &QCheckBox::toggled, this, [this] { updateControls(); });
        connect(&m_client, &NativeAuthClient::changed, this, [this] { updateControls(); });
        connect(m_login, &QPushButton::clicked, this, [this] {
            if (!m_login->isEnabled()) return;
            m_summary->clear();
            const bool accepted = m_client.start(m_email->text(), m_password->text(), m_deviceToken, m_mfa->text());
            if (accepted) { m_password->clear(); m_mfa->clear(); }
            updateControls();
        });
        connect(m_submit, &QPushButton::clicked, this, [this] {
            const auto code = m_code->text();
            m_code->clear();
            m_client.submitCode(code);
        });
        connect(m_cancel, &QPushButton::clicked, this, [this] {
            m_password->clear(); m_mfa->clear(); m_code->clear(); m_client.cancel();
        });
        const QPointer<SpikeWindow> guard(this);
        m_client.setGrantHandler([guard](RHOAuthGrant grant) {
            if (!guard) return;
            guard->m_summary->setText(tr("Full OAuth grant received. Access expiry: %1 UTC. Not identity-verified and not saved; closing this test discards it. No trading session was activated.")
                .arg(grant.expiresAt.toUTC().toString("yyyy-MM-dd HH:mm:ss")));
        });
        updateControls();
    }
    ~SpikeWindow() override { m_client.setGrantHandler({}); m_client.cancel(); }
protected:
    void closeEvent(QCloseEvent *event) override
    {
        m_password->clear(); m_mfa->clear(); m_code->clear();
        m_client.cancel();
        QWidget::closeEvent(event);
    }
private:
    static QLabel *label(const QString &text, QWidget *parent)
    {
        auto *value = new QLabel(text, parent);
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        return value;
    }
    static QLineEdit *input(const char *name, QWidget *parent)
    {
        auto *value = new QLineEdit(parent);
        value->setObjectName(QString::fromLatin1(name));
        value->setMaxLength(1024);
        return value;
    }
    void updateControls()
    {
        const bool busy = m_client.busy();
        m_email->setEnabled(!busy); m_password->setEnabled(!busy); m_mfa->setEnabled(!busy);
        m_consent->setEnabled(!busy);
        m_login->setEnabled(!busy && m_consent->isChecked() && !m_email->text().trimmed().isEmpty() && !m_password->text().isEmpty());
        const bool awaitingCode = m_client.state() == NativeAuthClient::State::WaitingForCode;
        m_code->setEnabled(awaitingCode); m_submit->setEnabled(awaitingCode);
        m_cancel->setEnabled(busy);
        m_status->setText(m_client.statusText());
    }
    NativeAuthClient &m_client;
    QString m_deviceToken;
    QLineEdit *m_email, *m_password, *m_mfa, *m_code;
    QCheckBox *m_consent;
    QPushButton *m_login, *m_submit, *m_cancel;
    QLabel *m_status, *m_summary;
};

#ifndef RH_NATIVE_SPIKE_UI_TEST
int main(int argc, char **argv)
{
#if defined(Q_OS_ANDROID)
    qputenv("ANDROID_OPENSSL_SUFFIX", "_3");
#endif
    QApplication app(argc, argv);
    app.setOrganizationName("PublicsTradingInterface");
    app.setApplicationName("RobinhoodNativeAuthSpike");
    const bool smoke = app.arguments().contains("--offline-smoke");
    QString device;
    if (smoke) device = QUuid::createUuid().toString(QUuid::WithoutBraces);
    else {
        QSettings settings;
        device = settings.value("device_token").toString();
        if (device.isEmpty()) {
            device = QUuid::createUuid().toString(QUuid::WithoutBraces);
            settings.setValue("device_token", device);
            settings.sync();
        }
        if (settings.status() != QSettings::NoError) return 2;
    }
    NativeAuthClient client;
    SpikeWindow window(client, device);
    window.show();
    if (smoke) QTimer::singleShot(100, &app, [&app, &client] { app.exit(client.busy() ? 3 : 0); });
    return app.exec();
}
#endif
