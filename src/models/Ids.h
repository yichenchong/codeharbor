#pragma once

#include <QString>

// Strongly-typed identifier wrappers for the core data model. Using distinct
// types prevents accidentally passing, e.g., a TerminalId where a DevSessionId
// is expected. Values are opaque UUID strings generated server-side.
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
