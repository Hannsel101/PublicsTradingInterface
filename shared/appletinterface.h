#pragma once

#include <QObject>
#include <QString>

class QQmlApplicationEngine;

// The only runtime contract the launcher requires of an applet.
class AppletInterface : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~AppletInterface() override;

    virtual QString id() const = 0;
    virtual void initialize(QQmlApplicationEngine &engine) = 0;
    virtual void start() = 0;
    virtual bool canStop() const = 0;
    virtual void stop() = 0;

signals:
    void navigationAvailabilityChanged();
};
