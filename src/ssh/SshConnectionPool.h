#pragma once

#include <QString>

namespace ch {

// One authenticated SSH connection per configured server, multiplexing many
// independent channels: terminal PTYs, the codeharbord RPC channel, and the
// agent-status channel (SPEC 5.3). Owns host-key verification (SPEC 12.1).
//
// Bootstrap placeholder. See docs/PLAN.md workstream S.
class SshConnectionPool {
public:
    static bool libsshAvailable();
};

} // namespace ch
