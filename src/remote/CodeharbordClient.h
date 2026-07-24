#pragma once

#include <QString>

namespace ch {

// Client-side RPC peer for the remote `codeharbord` service (SPEC 10). Speaks
// newline-delimited JSON over a dedicated SSH channel from the connection pool.
// Distinct from the server-side implementation in remote/.
//
// Bootstrap placeholder exposing the RPC framing constant. See docs/PLAN.md
// workstream P/R.
class CodeharbordClient {
public:
    // Launch command used to start the service over SSH (SPEC 10.1).
    static QString launchCommand();
};

} // namespace ch
