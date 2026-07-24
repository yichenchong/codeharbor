#pragma once

#include "SessionState.h"

namespace ch {

// Owns input, output, buffering, state, and reconnect logic for one terminal
// pane, independently of the xterm.js view that may render it (SPEC 5.4). The
// controller stays connected and drains/buffers output even when hidden.
//
// Bootstrap placeholder exposing only the reconnect backoff policy (SPEC 5.6)
// so it can be unit-tested. See docs/PLAN.md workstream T.
class TerminalController {
public:
    // Retry delay in seconds for the Nth (0-based) automatic reconnect attempt:
    // 1, 2, 5, 10, 30, then 60 thereafter.
    static int reconnectDelaySeconds(int attempt);
};

} // namespace ch
