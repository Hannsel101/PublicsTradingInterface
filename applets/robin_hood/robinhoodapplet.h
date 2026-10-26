#pragma once

#include "appletinterface.h"
#include <memory>

class RobinHoodApplet final : public AppletInterface
{
    Q_OBJECT
public:
    explicit RobinHoodApplet(QObject *parent = nullptr);
    ~RobinHoodApplet() override;
    static void prepareBrowserRuntime();
    static void initializeBrowserRuntime();
    QString id() const override;
    void initialize(QQmlApplicationEngine &engine) override;
    void start() override;
    bool canStop() const override;
    void stop() override;

private:
    struct State;
    std::unique_ptr<State> m_state;
};
