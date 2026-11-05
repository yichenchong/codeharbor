#pragma once

#include <QHashFunctions>
#include <QString>

// Strongly-typed identifier wrappers for the core data model. Using distinct
// types prevents accidentally passing, e.g., a TerminalId where a DevSessionId
// is expected. GroupId, DevSessionId, ViewerPaneId and TerminalId are opaque
// UUID strings minted server-side (remote/src/workspace.ts). ServerId instead
// names WHICH codeharbord host a row belongs to (SPEC 3.5): it is the opaque
// identity reported by that server's server.info response.
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

inline size_t qHash(const GroupId &id, size_t seed = 0) noexcept
{
    return ::qHash(id.value, seed);
}

inline size_t qHash(const DevSessionId &id, size_t seed = 0) noexcept
{
    return ::qHash(id.value, seed);
}

inline size_t qHash(const ViewerPaneId &id, size_t seed = 0) noexcept
{
    return ::qHash(id.value, seed);
}

inline size_t qHash(const TerminalId &id, size_t seed = 0) noexcept
{
    return ::qHash(id.value, seed);
}

inline size_t qHash(const ServerId &id, size_t seed = 0) noexcept
{
    return ::qHash(id.value, seed);
}

} // namespace ch
