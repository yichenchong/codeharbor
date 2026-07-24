#pragma once

#include <QString>

namespace ch {

// Access to the authoritative SQLite workspace database (SPEC 11.1). The
// database itself lives on the remote server; the client reaches it via
// codeharbord RPC. This type owns schema migration and query construction.
//
// Bootstrap placeholder: only the schema version constant is defined. See
// docs/PLAN.md workstream P.
class WorkspaceDb {
public:
    // Bump when the schema in schema.sql changes; migrations run server-side.
    // remote/sql/schema.sql is the authoritative DDL for this version.
    static constexpr int kSchemaVersion = 1;

    static QString schemaVersionString();
};

} // namespace ch
