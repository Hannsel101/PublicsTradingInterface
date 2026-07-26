#include "publicapiworker.h"

// PublicApiWorker::PublicApiWorker(QObject *parent)
//     : QObject{parent}
// {}

void PublicApiWorker::processToken(QString newToken)
{
    m_token = newToken;
    fetchAllSubaccountsConcurrently();
}
