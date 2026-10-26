#pragma once

#include "appletinterface.h"

#include <memory>

class PublicsApplet final : public AppletInterface
{
    Q_OBJECT
public:
    explicit PublicsApplet(QObject *parent = nullptr);
    ~PublicsApplet() override;

    QString id() const override;
    void initialize(QQmlApplicationEngine &engine) override;
    void start() override;
    bool canStop() const override;
    void stop() override;

private:
    struct State;
    std::unique_ptr<State> m_state;
};
