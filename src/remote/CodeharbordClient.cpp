#include "CodeharbordClient.h"

namespace ch {

QString CodeharbordClient::launchCommand()
{
    return QStringLiteral("codeharbord rpc --stdio");
}

} // namespace ch
