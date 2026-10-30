#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <memory>

class QProcess;

// Native, fail-closed proof of a positively app-recorded process, with a dead
// original creator and exact image/argv/birth/user/loopback-listener identity.
// Never executes a shell or exposes process arguments/output in diagnostics.
class TelegramInspectorProcess final
{
public:
    // Only records a running QProcess whose native parent is this exact app
    // instance. Commit failure returns empty; callers must stop that child.
    // Stores only identity metadata and an argv digest, never raw argv/secrets.
    static QByteArray recordStarted(const QProcess &child, quint16 inspectorPort,
        const QString &ownershipDirectory = {});
    // Compare-and-remove: a stopped child cannot erase another generation's record.
    static void forgetReceipt(quint16 inspectorPort, const QString &ownershipDirectory,
        const QByteArray &receipt);
    static std::unique_ptr<TelegramInspectorProcess> findOrphan(
        const QString &executable, const QStringList &arguments, quint16 inspectorPort,
        const QString &ownershipDirectory = {});
    ~TelegramInspectorProcess();
    TelegramInspectorProcess(const TelegramInspectorProcess &) = delete;
    TelegramInspectorProcess &operator=(const TelegramInspectorProcess &) = delete;
    // Revalidate identity, orphan status, executable, arguments and socket
    // immediately before either signal. false means no termination was attempted.
    bool terminate(bool force = false) const;
    // True only for the original process instance, not a recycled PID.
    bool alive() const;
    void forgetExitedReceipt() const;
private:
    struct State;
    explicit TelegramInspectorProcess(std::unique_ptr<State> state);
    std::unique_ptr<State> d;
};
