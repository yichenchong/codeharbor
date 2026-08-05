import { test } from "node:test";
import assert from "node:assert/strict";
import { DatabaseSync } from "node:sqlite";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { WORKSPACE_SCHEMA_VERSION, applyConnectionPragmas } from "../src/workspace.ts";

const schemaPath = fileURLToPath(new URL("../sql/schema.sql", import.meta.url));
const schemaSql = readFileSync(schemaPath, "utf8");

// The eight SPEC 11.1 domain tables plus the version table.
const DOMAIN_TABLES = [
    "groups",
    "dev_sessions",
    "viewer_panes",
    "terminal_panes",
    "session_layouts",
    "server_profiles",
    "server_settings",
    "app_settings",
];

// Every connection to a workspace database applies the shared pragmas first —
// that is the rule applyConnectionPragmas documents, and it holds for the raw
// DatabaseSync a test opens too. Following it here keeps this file honest about
// the connection the daemon actually runs the DDL on, instead of exercising a
// configuration that exists nowhere else.
function loadSchema(): DatabaseSync {
    const db = new DatabaseSync(":memory:");
    applyConnectionPragmas(db);
    db.exec(schemaSql);
    return db;
}

test("schema.sql loads into an in-memory database, on a properly configured connection", () => {
    const db = loadSchema();
    // The standing rule (applyConnectionPragmas): the busy timeout is in force
    // before any statement runs, DDL included. SQLite's default is 0, so this
    // fails outright if the helper ever stops applying the pragmas.
    const timeout = db.prepare("PRAGMA busy_timeout").get() as { timeout: number };
    assert.equal(timeout.timeout, 5000);
    db.close();
});

test("schema.sql defines all SPEC 11.1 tables plus schema_version and server_identity", () => {
    const db = loadSchema();
    const rows = db
        .prepare("SELECT name FROM sqlite_master WHERE type = 'table'")
        .all() as Array<{ name: string }>;
    const names = rows.map((r) => r.name);
    for (const table of [...DOMAIN_TABLES, "schema_version", "server_identity"]) {
        assert.equal(names.includes(table), true, `missing table: ${table}`);
    }
    db.close();
});

// The identity of the database itself is a singleton, not per-server config:
// the CHECK (id = 1) primary key is what makes two processes' INSERT OR IGNORE
// mints converge on one row instead of racing to two.
test("server_identity admits exactly one row and rejects a second id", () => {
    const db = loadSchema();
    const insert = db.prepare(
        "INSERT OR IGNORE INTO server_identity (id, server_id, created_at) VALUES (?, ?, ?)",
    );
    insert.run(1, "first", 1);
    insert.run(1, "second", 2);
    const rows = db
        .prepare("SELECT server_id FROM server_identity")
        .all() as Array<{ server_id: string }>;
    assert.deepEqual(rows.map((r) => r.server_id), ["first"]);
    // The CHECK pins the singleton to slot 1, so no writer can park a rival
    // identity in a second row. (Asserted with a plain INSERT: OR IGNORE would
    // swallow the CHECK violation exactly as it swallows the PK conflict.)
    assert.throws(() =>
        db
            .prepare("INSERT INTO server_identity (id, server_id, created_at) VALUES (?, ?, ?)")
            .run(2, "other-slot", 3),
    );
    const remaining = db
        .prepare("SELECT COUNT(*) AS n FROM server_identity")
        .get() as { n: number };
    assert.equal(remaining.n, 1);
    db.close();
});

test("schema_version seeds a single row at WORKSPACE_SCHEMA_VERSION, idempotently", () => {
    const db = loadSchema();
    // Re-applying the schema must not duplicate the version row (SPEC: the
    // initializer runs on every connection).
    db.exec(schemaSql);
    const row = db
        .prepare("SELECT version FROM schema_version")
        .get() as { version: number } | undefined;
    assert.notEqual(row, undefined);
    // The DDL's seed literal and the migration runner's target move together
    // (schema.sql header); a drift here means fresh databases claim a version
    // whose migrations never ran.
    assert.equal(row?.version, WORKSPACE_SCHEMA_VERSION);
    const count = db
        .prepare("SELECT COUNT(*) AS n FROM schema_version")
        .get() as { n: number };
    assert.equal(count.n, 1);
    db.close();
});
test("dev_sessions carries a durable pin bit defaulting to false", () => {
    const db = loadSchema();
    const columns = db
        .prepare("SELECT name, dflt_value, \"notnull\" AS nn FROM pragma_table_info('dev_sessions')")
        .all() as Array<{ name: string; dflt_value: string | null; nn: number }>;
    const pinned = columns.find((column) => column.name === "pinned");
    assert.equal(pinned?.name, "pinned");
    assert.equal(pinned?.dflt_value, "0");
    assert.equal(pinned?.nn, 1);

    db.exec(
        "INSERT INTO groups (id, server_id, name, position, collapsed, created_at, updated_at) VALUES ('g', 's', 'G', 0, 0, 1, 1)",
    );
    db.exec(
        "INSERT INTO dev_sessions (id, server_id, group_id, name, repository_root, position, archived, created_at, updated_at) VALUES ('d1', 's', 'g', 'S1', '/r', 0, 0, 1, 1)",
    );
    db.exec(
        "INSERT INTO dev_sessions (id, server_id, group_id, name, repository_root, position, archived, pinned, created_at, updated_at) VALUES ('d2', 's', 'g', 'S2', '/r', 1, 0, 1, 1, 1)",
    );
    const rows = db
        .prepare("SELECT id, pinned FROM dev_sessions ORDER BY id")
        .all() as Array<{ id: string; pinned: number }>;
    assert.deepEqual(
        rows.map((row) => ({ id: row.id, pinned: row.pinned })),
        [
            { id: "d1", pinned: 0 },
            { id: "d2", pinned: 1 },
        ],
    );
    db.close();
});


test("every domain table carries server_id (SPEC 3.5)", () => {
    const db = loadSchema();
    for (const table of DOMAIN_TABLES) {
        const cols = db
            .prepare(`SELECT name FROM pragma_table_info('${table}')`)
            .all() as Array<{ name: string }>;
        assert.equal(
            cols.some((c) => c.name === "server_id"),
            true,
            `table ${table} is missing a server_id column`,
        );
    }
    db.close();
});

// Two constraints on session_layouts that the store leans on directly: the CHECK
// is what lets Workspace.getLayouts key a { viewer, terminal } object straight
// off the stored string, and the UNIQUE pair is the conflict target of
// setLayout's upsert. Losing either turns a bad write into silent data instead
// of a rejection.
test("session_layouts pins one row per region and rejects an unknown region", () => {
    const db = loadSchema();
    db.exec(
        "INSERT INTO groups (id, server_id, name, position, collapsed, created_at, updated_at) VALUES ('g', 's', 'G', 0, 0, 1, 1)",
    );
    db.exec(
        "INSERT INTO dev_sessions (id, server_id, group_id, name, repository_root, position, archived, created_at, updated_at) VALUES ('d', 's', 'g', 'S', '/r', 0, 0, 1, 1)",
    );
    const insert = db.prepare(
        "INSERT INTO session_layouts (id, server_id, dev_session_id, region, tree, created_at, updated_at) VALUES (?, 's', 'd', ?, '{}', 1, 1)",
    );
    insert.run("l1", "viewer");
    insert.run("l2", "terminal");
    // A second viewer tree for the same Dev Session is a duplicate, not a second
    // layout: the region slot is single-valued.
    assert.throws(() => insert.run("l3", "viewer"));
    // Anything outside the two regions is refused outright.
    assert.throws(() => insert.run("l4", "sidebar"));
    const rows = db
        .prepare("SELECT region FROM session_layouts ORDER BY region")
        .all() as Array<{ region: string }>;
    assert.deepEqual(rows.map((r) => r.region), ["terminal", "viewer"]);
    db.close();
});

// The terminal-pane identity rules, spelled out in schema.sql's long comment
// and relied on by the store: a tmux target names ONE row (two panes on one
// target drive the same remote shell), an unbound pane is legal and there may
// be many of them, and a slot LABEL is not an identity, so one Dev Session may
// hold several rows wearing the same one.
test("terminal_panes constrains tmux_target only, and allows repeated labels", () => {
    const db = loadSchema();
    db.exec(
        "INSERT INTO groups (id, server_id, name, position, collapsed, created_at, updated_at) VALUES ('g', 's', 'G', 0, 0, 1, 1)",
    );
    db.exec(
        "INSERT INTO dev_sessions (id, server_id, group_id, name, repository_root, position, archived, created_at, updated_at) VALUES ('d', 's', 'g', 'S', '/r', 0, 0, 1, 1)",
    );
    const insert = db.prepare(
        "INSERT INTO terminal_panes (id, server_id, dev_session_id, name, tmux_target, position, created_at, updated_at) VALUES (?, 's', 'd', ?, ?, 0, 1, 1)",
    );
    insert.run("t1", "terminal-1", "ch_one");
    // A second row on the same target is refused...
    assert.throws(() => insert.run("t2", "terminal-2", "ch_one"));
    // ...but the same LABEL on a second row is fine, because a label is a
    // layout slot and not the pane's identity (schema v4 removed that rule).
    insert.run("t2", "terminal-1", "ch_two");
    // And any number of panes may have no remote session bound to them: SQLite
    // treats each NULL as distinct under UNIQUE.
    insert.run("t3", "terminal-1", null);
    insert.run("t4", "terminal-1", null);
    const rows = db
        .prepare("SELECT id FROM terminal_panes ORDER BY id")
        .all() as Array<{ id: string }>;
    assert.deepEqual(rows.map((r) => r.id), ["t1", "t2", "t3", "t4"]);
    db.close();
});

// The foreign keys are declared without ON DELETE, i.e. NO ACTION: they REJECT
// a delete that would orphan a child rather than cascading. That is exactly why
// Workspace.deleteGroup/deleteSession delete children first, by hand — if these
// ever became ON DELETE CASCADE the manual cascade would still be correct, but
// if they became unenforced, a delete would silently strand rows.
test("the child foreign keys reject an orphaning delete rather than cascading", () => {
    const db = loadSchema();
    db.exec(
        "INSERT INTO groups (id, server_id, name, position, collapsed, created_at, updated_at) VALUES ('g', 's', 'G', 0, 0, 1, 1)",
    );
    db.exec(
        "INSERT INTO dev_sessions (id, server_id, group_id, name, repository_root, position, archived, created_at, updated_at) VALUES ('d', 's', 'g', 'S', '/r', 0, 0, 1, 1)",
    );
    db.exec(
        "INSERT INTO viewer_panes (id, server_id, dev_session_id, url, position, created_at, updated_at) VALUES ('v', 's', 'd', 'http://x', 0, 1, 1)",
    );

    // A parent that still has children cannot be deleted, at either level.
    assert.throws(() => db.exec("DELETE FROM dev_sessions WHERE id = 'd'"), /FOREIGN KEY/);
    assert.throws(() => db.exec("DELETE FROM groups WHERE id = 'g'"), /FOREIGN KEY/);
    // Nor can a child be inserted under a parent that does not exist.
    assert.throws(
        () =>
            db.exec(
                "INSERT INTO dev_sessions (id, server_id, group_id, name, repository_root, position, archived, created_at, updated_at) VALUES ('d2', 's', 'nope', 'S', '/r', 0, 0, 1, 1)",
            ),
        /FOREIGN KEY/,
    );

    // Deepest-first is the order that works, which is the order the store uses.
    db.exec("DELETE FROM viewer_panes WHERE dev_session_id = 'd'");
    db.exec("DELETE FROM dev_sessions WHERE group_id = 'g'");
    db.exec("DELETE FROM groups WHERE id = 'g'");
    const left = db.prepare("SELECT COUNT(*) AS n FROM groups").get() as { n: number };
    assert.equal(left.n, 0);
    db.close();
});

// The two per-server key/value stores are single-valued per key, by two
// different spellings of the same rule (a UNIQUE pair on one, a composite
// PRIMARY KEY on the other). Without it a repeated write would silently leave
// two rows for one setting and a reader would get whichever it happened to hit
// first — and both are the reason server_identity needs a table of its own,
// since neither offers a conflict target for the server's OWN id.
test("the per-server key/value tables hold at most one row per (server, key)", () => {
    const db = loadSchema();

    const setting = db.prepare(
        "INSERT INTO server_settings (id, server_id, key, value) VALUES (?, ?, ?, ?)",
    );
    setting.run("s1", "srv-a", "theme", "dark");
    // Same key under a DIFFERENT server is a different setting.
    setting.run("s2", "srv-b", "theme", "light");
    // Same (server, key) is a duplicate, even under a fresh row id.
    assert.throws(() => setting.run("s3", "srv-a", "theme", "light"), /UNIQUE constraint failed/);

    const app = db.prepare("INSERT INTO app_settings (server_id, key, value) VALUES (?, ?, ?)");
    app.run("srv-a", "font", "12");
    app.run("srv-b", "font", "14");
    assert.throws(() => app.run("srv-a", "font", "13"), /UNIQUE constraint failed/);

    // An upsert on that key is how a caller actually rewrites a setting.
    db.prepare(
        "INSERT INTO app_settings (server_id, key, value) VALUES (?, ?, ?) " +
            "ON CONFLICT (server_id, key) DO UPDATE SET value = excluded.value",
    ).run("srv-a", "font", "13");
    const rows = db
        .prepare("SELECT server_id, value FROM app_settings ORDER BY server_id")
        .all() as Array<{ server_id: string; value: string }>;
    assert.deepEqual(
        rows.map((r) => [r.server_id, r.value]),
        [
            ["srv-a", "13"],
            ["srv-b", "14"],
        ],
    );

    db.close();
});
