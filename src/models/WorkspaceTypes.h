#pragma once

#include "Ids.h"

#include <QString>

// Core data-model value types (SPEC 3.1-3.4, workstream M). These carry the
// identifying/domain columns of the corresponding remote/sql/schema.sql tables
// plus server_id (SPEC 3.5, multi-server future-proofing); persistence-only
// bookkeeping (created_at/updated_at) stays in the persistence layer, and
// runtime/connection state lives with the sidebar model. Ids use the
// strongly-typed wrappers from Ids.h.
namespace ch {

// A collapsible sidebar container (SPEC 3.1).
struct Group {
    GroupId id;
    ServerId serverId;
    QString name;
    int position = 0;
    bool collapsed = false;

    bool operator==(const Group &) const = default;
};

// A saved remote development workspace (SPEC 3.2). Corresponds to one repository
// or one task within a repository.
struct DevSession {
    DevSessionId id;
    ServerId serverId;
    GroupId groupId;
    QString name;
    QString repositoryRoot;
    QString defaultWorkingDirectory;
    QString taskDescription;
    int position = 0;
    bool archived = false;

    bool operator==(const DevSession &) const = default;
};

// A viewer pane displaying one URL or remote resource (SPEC 3.3).
struct ViewerPane {
    ViewerPaneId id;
    ServerId serverId;
    DevSessionId devSessionId;
    QString url;
    QString handler;
    QString title;
    int position = 0;

    bool operator==(const ViewerPane &) const = default;
};

// An xterm.js terminal attached to a remote tmux target (SPEC 3.4). Connection
// and attention state are runtime concerns held elsewhere, not persisted here.
struct TerminalPane {
    TerminalId id;
    ServerId serverId;
    DevSessionId devSessionId;
    QString name;
    QString workingDirectory;
    QString tmuxTarget;
    QString startupCommand;
    QString harness;
    int position = 0;

    bool operator==(const TerminalPane &) const = default;
};

} // namespace ch
