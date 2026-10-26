#pragma once

#include "appletinterface.h"

#include <QList>
#include <QSet>

class QQmlApplicationEngine;

class LauncherController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString activeAppletId READ activeAppletId NOTIFY activeAppletIdChanged FINAL)
    Q_PROPERTY(bool canReturnHome READ canReturnHome NOTIFY canReturnHomeChanged FINAL)
public:
    LauncherController(QQmlApplicationEngine &engine, const QList<AppletInterface *> &applets,
                       QObject *parent = nullptr);

    QString activeAppletId() const;
    bool canReturnHome() const;
    Q_INVOKABLE bool activate(const QString &id);
    Q_INVOKABLE bool returnHome();

signals:
    void activeAppletIdChanged();
    void canReturnHomeChanged();

private:
    QQmlApplicationEngine &m_engine;
    QList<AppletInterface *> m_applets;
    QSet<QString> m_initialized;
    AppletInterface *m_active = nullptr;
};
