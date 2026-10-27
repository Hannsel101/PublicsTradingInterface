#include "rhtradingcontext.h"
#include "rhsessionmanager.h"
#include <utility>

RHTradeController::Dependencies makeRHTradingDependencies(RHSessionManager &sessions)
{
    RHTradeController::Dependencies dependencies;
    dependencies.acquire = [&sessions](auto done) {
        sessions.requestTradingSession([done = std::move(done)]
            (bool ok, RHSessionManager::TradingSession snapshot, QString reason) mutable {
            RHTradeController::Session context;
            context.sessionIndex = snapshot.sessionIndex;
            context.generation = snapshot.generation;
            context.profileId = std::move(snapshot.profileId);
            context.token = std::move(snapshot.token);
            context.expiresAt = snapshot.expiresAt;
            // The manager grants a lease only for a verified primary/OAuth session.
            context.authKind = RHNetworkEngine::AuthKind::OAuthBearer;
            context.accounts = std::move(snapshot.accounts);
            done(ok, std::move(context), std::move(reason));
        });
    };
    dependencies.current = [&sessions](const RHTradeController::Session &context) {
        RHSessionManager::TradingSession snapshot;
        snapshot.sessionIndex = context.sessionIndex;
        snapshot.generation = context.generation;
        snapshot.profileId = context.profileId;
        snapshot.expiresAt = context.expiresAt;
        return sessions.isTradingSessionCurrent(snapshot);
    };
    const QPointer<RHSessionManager> guard(&sessions);
    dependencies.currentLocal = [guard](const RHTradeController::Session &context) {
        RHSessionManager::TradingSession snapshot;
        snapshot.sessionIndex = context.sessionIndex;
        snapshot.generation = context.generation;
        snapshot.profileId = context.profileId;
        snapshot.expiresAt = context.expiresAt;
        return guard && guard->isTradingSessionLocallyCurrent(snapshot);
    };
    dependencies.currentAsync = [guard](const RHTradeController::Session &context, auto done) {
        if (!guard) { done(false); return; }
        RHSessionManager::TradingSession snapshot;
        snapshot.sessionIndex = context.sessionIndex;
        snapshot.generation = context.generation;
        snapshot.profileId = context.profileId;
        snapshot.expiresAt = context.expiresAt;
        guard->checkTradingOwnershipAsync(std::move(snapshot), std::move(done));
    };
    dependencies.release = [guard] { if (guard) guard->releaseTradingSession(); };
    dependencies.availabilityReason = [&sessions] { return sessions.tradingUnavailableReason(); };
    return dependencies;
}
