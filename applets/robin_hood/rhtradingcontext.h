#pragma once
#include "rhtradecontroller.h"

class RHSessionManager;

// Both QObjects remain on the same thread; the session manager outlives the controller.
RHTradeController::Dependencies makeRHTradingDependencies(RHSessionManager &sessions);
