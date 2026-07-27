import { test } from "node:test";
import assert from "node:assert/strict";
import { DatabaseSync } from "node:sqlite";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

import { WORKSPACE_SCHEMA_VERSION } from "../src/workspace.ts";

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

function loadSchema(): DatabaseSync {
    const db = new DatabaseSync(":memory:");
    db.exec(schemaSql);
    return db;
}

test("schema.sql loads into an in-memory database", () => {
    const db = loadSchema();
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
