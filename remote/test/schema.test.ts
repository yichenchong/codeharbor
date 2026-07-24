import { test } from "node:test";
import assert from "node:assert/strict";
import { DatabaseSync } from "node:sqlite";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

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

test("schema.sql defines all SPEC 11.1 tables plus schema_version", () => {
    const db = loadSchema();
    const rows = db
        .prepare("SELECT name FROM sqlite_master WHERE type = 'table'")
        .all() as Array<{ name: string }>;
    const names = rows.map((r) => r.name);
    for (const table of [...DOMAIN_TABLES, "schema_version"]) {
        assert.equal(names.includes(table), true, `missing table: ${table}`);
    }
    db.close();
});

test("schema_version holds a single row equal to 1, idempotently", () => {
    const db = loadSchema();
    // Re-applying the schema must not duplicate the version row (SPEC: the
    // initializer runs on every connection).
    db.exec(schemaSql);
    const row = db
        .prepare("SELECT version FROM schema_version")
        .get() as { version: number } | undefined;
    assert.notEqual(row, undefined);
    assert.equal(row?.version, 1);
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
