#include "launchercontroller.h"

#include <QQmlApplicationEngine>

#include <algorithm>

LauncherController::LauncherController(QQmlApplicationEngine &engine,
                                       const QList<AppletInterface *> &applets,
                                       QObject *parent)
    : QObject(parent), m_engine(engine), m_applets(applets)
{
    for (AppletInterface *applet : m_applets) {
        connect(applet, &AppletInterface::navigationAvailabilityChanged, this, [this, applet]() {
            if (m_active == applet)
                emit canReturnHomeChanged();
        });
    }
}

QString LauncherController::activeAppletId() const
{
    return m_active ? m_active->id() : QString();
}

bool LauncherController::canReturnHome() const
{
    return m_active && m_active->canStop();
}

bool LauncherController::activate(const QString &id)
{
    if (m_active)
        return m_active->id() == id;

    const auto it = std::find_if(m_applets.cbegin(), m_applets.cend(), [&id](AppletInterface *applet) {
        return applet->id() == id;
    });
    if (it == m_applets.cend())
        return false;

    AppletInterface *applet = *it;
    if (!m_initialized.contains(id)) {
        applet->initialize(m_engine);
        m_initialized.insert(id);
    }
    m_active = applet;
    applet->start();
    emit activeAppletIdChanged();
    emit canReturnHomeChanged();
    return true;
}

bool LauncherController::returnHome()
{
    if (!canReturnHome())
        return false;

    m_active->stop();
    m_active = nullptr;
    emit activeAppletIdChanged();
    emit canReturnHomeChanged();
    return true;
}
