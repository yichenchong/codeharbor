#pragma once

#include "Ids.h"

#include <QString>

// Core data-model value types (SPEC 3.1-3.4, workstream M). These are plain
// persisted-shape structs mirroring the columns in remote/sql/schema.sql; they
// carry no runtime/connection state (that lives with the sidebar model). Ids use
// the strongly-typed wrappers from Ids.h.
namespace ch {

// A collapsible sidebar container (SPEC 3.1).
struct Group {
    GroupId id;
    QString name;
    int position = 0;
    bool collapsed = false;

    bool operator==(const Group &) const = default;
};

// A saved remote development workspace (SPEC 3.2). Corresponds to one repository
// or one task within a repository.
struct DevSession {
    DevSessionId id;
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
