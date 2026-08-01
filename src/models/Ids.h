#pragma once

#include <QString>

// Strongly-typed identifier wrappers for the core data model. Using distinct
// types prevents accidentally passing, e.g., a TerminalId where a DevSessionId
// is expected. GroupId, DevSessionId, ViewerPaneId and TerminalId are opaque
// UUID strings minted server-side (remote/src/workspace.ts). ServerId is the
// exception: it names WHICH codeharbord host a row belongs to (SPEC 3.5) and is
// chosen client-side, so it is an arbitrary opaque string rather than a UUID.
namespace ch {

struct GroupId {
    QString value;
    bool operator==(const GroupId &) const = default;
};

struct DevSessionId {
    QString value;
    bool operator==(const DevSessionId &) const = default;
};

struct ViewerPaneId {
    QString value;
    bool operator==(const ViewerPaneId &) const = default;
};

struct TerminalId {
    QString value;
    bool operator==(const TerminalId &) const = default;
};

struct ServerId {
    QString value;
    bool operator==(const ServerId &) const = default;
};

} // namespace ch
