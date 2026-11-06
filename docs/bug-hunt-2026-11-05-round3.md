# CodeHarbor comprehensive bug-finding and repair report — round 3

## Result and reading guide

This round contains **90 uniquely labeled corrections**, covering product bugs, latent failures, test-fixture defects, missing behavioral coverage, documentation errors, and small cleanups. They are not 90 claimed production vulnerabilities. Every correction is described below with its context, cause, change, affected files, reasoning, and evidence limits.

The audit started from commit `3b0f07cdeca33f820d50416e4c6f3961441447d7` on `main`. The release remains **0.4.3**; no release bump or tag was requested. The final remote RPC schema and minimum accepted RPC schema are **10**. The workspace database schema is **6**, including the ordered repair migration described under TWS4. The project's minimum Qt version is **6.10**; this host built and ran Qt **6.10.2**, GCC **15.2.0**, Node **24.19**, and libssh **0.11.3**. References to an API first appearing in Qt 6.9 are API-history notes, not a claim that the current project supports Qt 6.9.

The final complete native run passed **65/65 targets with two parallel jobs**. The complete remote Node suite passed **537 tests with one paid-model test skipped because its explicit opt-in was not enabled**, and the three embedded web suites passed **179 tests**. The verification section distinguishes those executed results from source-derived risks and platform branches that could not run here. QML lint completed without errors but was **not warning-free**.

The label prefixes identify the sections in this third round. For example, TSH4 means the fourth SSH finding in this report, not a reference to an earlier audit's SSH issue. Labels are unique within this round. Cross-references connect fixes that share a user-visible scenario without counting the same correction twice.

## How the work was divided and checked

The repository was mapped first, then each major section below was assigned to a dedicated implementation worker. Independent reviews checked the repairs, and integration failures were routed back to the relevant owner. Two additional workers handled the late cross-test disconnect and callback-lifetime findings after the final parallel run exposed them. Workers were instructed to stay within their assigned files, route incidental findings to the parent, use only the repository's virtual-environment Python, and keep all imports at the top level. Builds, lint, and runtime verification were coordinated after implementation rather than run against partially edited shared files.

The parent integrated cross-section repairs and exercised real application paths as well as the automated suites. Those checks used private client settings, databases, runtime directories, data directories, SSH listeners, keys, known-hosts files, tmux servers, and display/bus services. The normal user workspace, installed service, SSH agent, and browser state were not used as disposable fixtures. A generated private listener can still have multiple test-owned connections; TTC5 explains the additional ownership mistake that the final parallel run found there.

Each item's coverage paragraph describes the behavior a regression checks. A test description alone is not a claimed before-fix reproduction. Explicit “observed” sections identify actual observations; source-derived risks and unavailable platform checks remain marked as such. The final verification section records the integrated execution, including the previously failing concurrent pair before and after its fix. No test lock, serialization rule, retry, broad warning suppression, or removal of meaningful protocol-parity gates was used to make that pair pass.

## Terms used in the findings

- **Remote daemon:** the Node/TypeScript process on the SSH server that owns workspace records and handles remote operations.
- **Connection profile:** saved client settings for connecting to a server. Its profile ID is different from the server identity stored in a remote workspace database.
- **Session:** a server-owned development workspace containing its terminal and viewer layout. A tmux session is the separate remote terminal process container and is named explicitly where that distinction matters.
- **Pane:** one terminal or viewer area in a layout. Its ID and the database row that owns it must not be confused with a visible label or tmux name.
- **Accepted server identity:** the server identity the client has validated for its current connection. A newly received reply does not automatically have authority to change current state.
- **Generation or request token:** a counter or identifier captured when asynchronous work starts. A reply from an earlier request must not overwrite a newer document, connection, or layout.
- **Retained unsaved document:** the existing in-memory editor and text kept visible when its old layout or session stops being current. Keeping that text does not keep the old session active, grant permission to save it, or create fake backend rows.
- **Revision:** the remote file version used to detect conflicting edits. File identity and metadata are part of this check; it is not a universal content hash or a guarantee against every change after the final check.

## Audit coverage map


| Section | Main code | Finding labels | Corrections |
| --- | --- | --- | --- |
| Remote daemon and private control socket | `remote/src/bridge.ts, control.ts, control-client.ts` | TDA1–TDA3 | 3 |
| Remote and native workspace databases | `remote/src/workspace.ts, src/persistence/` | TWS1–TWS5 | 5 |
| Remote files, revisions, reads, and watchers | `remote/src/files.ts` | TFS1–TFS4 | 4 |
| Agent integration, MCP tools, and live model tests | `remote/src/mcp/, remote/test/oh-my-pi-live.test.ts` | TIN1–TIN3 | 3 |
| RPC transport and terminal-reply ownership | `src/remote/CodeharbordClient.*` | TRP1–TRP2 | 2 |
| SSH authentication, transport lifetime, and live fixtures | `src/ssh/` | TSH1–TSH5 | 5 |
| Remote service bootstrap and update safety | `src/app/SessionBootstrap.*` | TSB1–TSB6 | 6 |
| Application coordination and persisted settings | `src/app/AppController.*, SessionLayouts.*, main.cpp` | TAC1–TAC8 | 8 |
| Settings, profile, model, and native chrome observers | `src/app/, src/models/` | TMS1–TMS4 | 4 |
| tmux session discovery and deletion | `remote/src/tmux.ts` | TAG1 | 1 |
| Terminal creation, embedded pages, scaling, and live reconnect | `src/terminal/, src/web/terminal/` | TTC1–TTC5 | 5 |
| Desktop editor recovery and document ownership | `src/editor/, src/qml/EditorPaneView.qml` | TED1–TED5 | 5 |
| Desktop viewer navigation, directory requests, and profiles | `src/viewers/, src/qml/Viewer*` | TVW1–TVW3 | 3 |
| Desktop layout, dialogs, and retained unsaved work | `src/qml/` | TUI1–TUI6 | 6 |
| Mobile connection, key storage, and native theme | `src/mobile/` | TMC1–TMC5 | 5 |
| Native mobile viewers and resource lifetime | `src/mobile/qml/Viewer*, MobileViewerService.*` | TMV1–TMV7 | 7 |
| Native mobile Markdown model and layout | `src/mobile/MarkdownModel.*` | TMM1–TMM5 | 5 |
| Terminal escape-sequence screen state | `src/vt/` | TVT1–TVT4 | 4 |
| Web bundle build and release tools | `src/web/*/build.mjs, .omp/skills/bump-version/` | TBR1–TBR4 | 4 |
| Public documentation and viewer tool instructions | `README.md, docs/, three viewer skills` | TDC1–TDC5 | 5 |

## Remote daemon and private control socket

### TDA1 — Refusing a malformed control version must not exhaust the daemon's stack

**Category / impact:** Input-validation reliability. A request that should receive a small refusal instead caused the production parser to throw. A daemon-wide interruption is a source-derived risk, not a separately observed clean process-crash result.

**Context:** Viewer commands reach CodeHarbor through a private control socket. Each request carries an envelope, including a control-protocol version. The listener must reject unsupported versions before relaying a command, and should remain available for later valid requests. The 64 KiB request limit does not prevent a small JSON value from containing thousands of nested arrays.

**Before-case / root cause:** Main's supplied proof, `local://round3-control-proof.txt`, records the same otherwise-valid request with 10,000 nested arrays in its version field being passed to baseline `3b0f07c` and current production `parseControlRequest`. The baseline threw `RangeError: Maximum call stack size exceeded`; the current parser returned `ok:false` with `error.code:"bad_request"`. The old unsupported-version diagnostic called recursive `JSON.stringify` on the untrusted version value while trying to explain its rejection. [INFERENCE] Because `startControlListener` calls this parser directly from a socket data callback without an exception guard around that call, the exception could escape the listener and interrupt the daemon. The attempted baseline socket regression is not stronger evidence than that: after an initial overlong test socket path was corrected, it reported an uncaught exception but kept a pending socket alive until a 15-second process timeout. It was not a cleanly completed daemon-crash reproduction.

**Fix:** The unsupported-version refusal describes the unsupported revision and the revision supported by the server without serializing the arbitrary supplied value. Validation still rejects the malformed request before relaying anything. The correction removes the recursive work from the rejection path rather than catching a recursion exception or adding a special maximum-depth exception for this one input.

**Why this choice / limits:** The protocol needs to identify an unsupported revision, not reproduce an attacker-controlled JSON structure in an English diagnostic. Valid control revision 1 and the deliberately supported absent-version case retain their behavior. No request field, response field, exported API or RPC schema changes under this issue. The observed parser result and the source-derived listener consequence are intentionally distinguished.

**Regression coverage:** `a deeply nested unsupported version is refused without taking down the listener` in `remote/test/control.test.ts` sends the nested request over a real private Unix socket, requires a correlated `bad_request` without a relayed command, and then completes a valid list command through the same listener. Those assertions cover continued service, not merely a parser return value. Main owns the final integrated verification record; this section makes no worker-test passing claim.

**Files / symbols:** `remote/src/control.ts` — `parseControlRequest`, `startControlListener`; `remote/test/control.test.ts` — the deep-version listener regression. Observed direct-parser evidence: `local://round3-control-proof.txt` and `build/dev/.tmp/hunt-r3/control-before-after.mjs`.

### TDA2 — Stale bridge-lock recovery must preserve another starter's replacement lock

**Category / impact:** Concurrent startup and filesystem ownership. Recovery could remove a live starter's ownership record while trying to remove a dead starter's record.

**Context:** The agent-event bridge uses a filesystem lock to establish which process owns startup and the listener. When a lock appears stale, a new starter reads its recorded process ID and probes whether that process still exists. Several starters can perform this recovery at once. A pathname is only a name: between two operations, it can refer to a different file owned by a different starter.

**Before-case / root cause:** [INFERENCE] `acquireBridgeLock` read the old lock's process ID, probed it, and then unlinked the lock pathname without checking that the pathname still represented the file it had inspected. Another starter could reclaim the dead record and install its own live lock during the probe. The first starter would then delete the replacement, undermining the bridge's ownership bookkeeping rather than merely removing stale state. No observed production occurrence is claimed.

**Fix:** Capture the original lock file's filesystem identity before reading its ownership record, and compare both device and inode immediately before stale removal. These two fields identify a file within and across mounted filesystems. If the pathname now identifies a different file, restart acquisition and inspect the current record instead of deleting it. This uses the existing ownership-checking convention already used by `unlinkOwnedPath` and stale socket cleanup.

**Why this choice / limits:** Rechecking ownership addresses the concrete replacement-during-process-probe sequence without guessing from pathname spelling or process ID alone. It does not add a new lock protocol or exported interface. Portable pathname checks are not an atomic kernel compare-and-unlink operation: no guarantee is made against arbitrary malicious same-user replacement between the final synchronous identity check and unlink.

**Regression coverage:** `stale lock reclamation preserves a replacement published during the owner probe` in `remote/test/bridge.test.ts` exercises the real `startBridge` acquisition path in an owned temporary directory. A narrowly scoped `process.kill` test hook replaces the supposedly dead owner's lock with a live record for the test process during the probe. The subsequent real probe targets only that test process. Assertions require acquisition to be refused, the replacement inode to remain, and no listener socket to be bound. This is behavior coverage, with

**Files / symbols:** `remote/src/bridge.ts` — `acquireBridgeLock`, using the existing `unlinkOwnedPath` ownership convention; `remote/test/bridge.test.ts` — dedicated lock-replacement regression.

### TDA3 — Response tests should protect JSON framing, not property order or English wording

**Category / impact:** Test quality and maintainability; no production RPC behavior changes.

**Context:** The daemon sends each JSON response as one newline-terminated transport frame. Object-property order and incidental English error-message wording are not the transport contract. The same test file also contains important TypeScript/C++ agreement checks for method names, error values, versions and frame limits; those serve a different purpose and remain required.

**Before-case / root cause:** [INFERENCE] An exact serialized-string assertion coupled a response test to JSON object-key order, so a harmless serialization-order change could fail it without breaking communication. Four neighboring checks similarly required English message fragments even though their tests already checked bounded frames, response correlation and error codes. Those checks did not directly establish the embedded-newline boundary that matters to the line-oriented transport.

**Fix:** Replace the exact JSON-string comparison with a response whose text contains an embedded newline. Require the only actual newline to be the terminating frame delimiter, then decode the frame and compare the recovered payload. Remove the four incidental English-message regular-expression assertions while retaining the existing size, response-ID, error-code and error-handling assertions. Shorten the adjacent keepalive comment without removing its real dispatch behavior test.

**Why this choice / limits:** The new assertion can detect a newline escaping failure that would split one response into multiple frames, without depending on property order. This is a deliberately narrow change. The original `remote/test/rpc-mirror.test.ts` filename and helpers remain, as do all original RPC-name, error, version and frame-cap parity gates and the native `schemaVersionMatchesTheRemoteSchema` check. The broader proposed deletion of those gates was rejected; no test-file rename, compatibility alias removal or native parity-test deletion is part of this issue. Current values remain release 0.4.3, RPC schema/current client minimum 10, and SQLite workspace schema 6. `server.info.schemaVersion` describes RPC compatibility, not the database migration number. TDA3 changes none of those policies or values.

**Regression coverage:** `a response escapes embedded newlines without creating another frame` in `remote/test/rpc-mirror.test.ts` asserts the sole newline position and round-trip payload. The neighboring tests retain bounded over-cap refusals, null replacement for an oversized response ID, payload-versus-delimiter limits, and a correlated answer for unserializable results. Cross-language parity and the real keepalive dispatch checks remain separate coverage.

**Files / symbols:** `remote/test/rpc-mirror.test.ts` — `responseLine` regression assertions and adjacent comment. `src/persistence/tests/tst_workspacedb.cpp` retains `schemaVersionMatchesTheRemoteSchema`; its proposed unrelated cleanup was restored, leaving its separate TRP2 changes intact.

## Remote and native workspace databases

### TWS1 — Reject corrupt stored migration numbers before changing a workspace

**Category / impact:** Database integrity and migration validation. A malformed version marker could be accepted as current or changed to a current-looking value without completing the corresponding structural upgrades.

**Context:** The remote daemon owns SQLite workspace migrations. Its `schema_version` row tells it which ordered upgrades a database still needs. SQLite's ordinary INTEGER affinity is not a runtime type guarantee: a non-STRICT table can contain text or a fractional value in that column. Missing version metadata intentionally means an uninitialized database, while stored zero is an existing supported compatibility case.

**Before-case / root cause:** [INFERENCE] `schemaVersion` cast the queried cell to a TypeScript number without validating its actual value. JavaScript comparisons with text such as `not-a-version` can skip every migration step, after which the old runner still recorded version 5. A legacy database could consequently be marked upgraded while still missing columns or constraints. Negative and fractional values also entered migration selection despite never naming supported migration steps. Main reports substantive baseline evidence in `build/dev/.tmp/hunt-r3/before-schema-preservation.txt`: all eight malformed-version cases failed against `3b0f07c`. Four fully current fixtures unexpectedly opened; four genuine legacy fixtures changed the stored marker to 5 before failing preservation checks. These were acceptance/state-mutation failures, not disagreements over diagnostic wording.

**Fix:** Read the cell as `unknown` and accept only a nonnegative safe integer. Return zero only for a genuinely missing row or the actual missing-`schema_version`-table query error. Validate the returned value outside the SQL exception handler. Narrow the missing-table exception match so a corrupt marker containing the words `no such table`, or a broken view referring to a different missing table, cannot masquerade as an uninitialized workspace.

**Why this choice / limits:** Migration decisions must use a valid version before writing schema or data. Existing stored-zero handling and valid ordered upgrades remain intact, and valid integer versions newer than the build still receive the existing newer-build refusal. Connection setup may change SQLite journaling metadata before validation; the guarantee is no schema/data migration on an invalid marker, not byte-for-byte identity of the database file. Current workspace schema is 6; that storage number is separate from unchanged RPC schema 10 and is not the client RPC compatibility gate.

**Regression coverage:** `migration rejects invalid stored schema versions without advancing or altering the database` in `remote/test/workspace.test.ts` combines fully current and genuine legacy-v1-shaped private databases with `not-a-version`, `no such table`, `4.5` and `-1`. Each case must throw without depending on English wording, and must preserve the stored version, complete `sqlite_schema` catalog and group/session/pane/layout rows. Unexpectedly accepted workspaces are closed before the assertion fails. No current passing-run claim is made here.

**Files / symbols:** `remote/src/workspace.ts` — `schemaVersion`, `migrate`; `remote/test/workspace.test.ts` — malformed-version regression. Migration contracts are documented in `docs/SPEC.md` and `docs/DEVELOPMENT.md` with the coordinated TWS changes.

### TWS2 — A partial unique index is not a global terminal-identity guarantee

**Category / impact:** Persistent terminal identity and migration correctness. A database could appear to enforce unique terminal targets while allowing unprotected rows to share one shell.

**Context:** Each terminal pane has a durable database row and a `tmux_target` identifying its remote shell. Two rows sharing a target attach to the same shell rather than independent terminals. Migration code inspects SQLite's index catalog instead of assuming one particular index name, because valid older databases can enforce the same rule through a named index or a table-level unique constraint. A partial unique index applies only to rows selected by its `WHERE` clause.

**Before-case / root cause:** [INFERENCE] The old uniqueness detector checked that an index was unique and listed the expected columns, but ignored SQLite's `partial` flag. A legacy rule protecting targets only for selected pane names was therefore mistaken for complete target enforcement. Migration v3 could skip the full rule, leaving other rows free to share a target. Main's supplied workspace report attributes an observed missing-target-uniqueness baseline failure to `build/dev/.tmp/hunt-r3/before-workspace.txt`; this section does not promote the broader source-derived consequences into an observed shell-sharing incident.

**Fix:** `terminalPaneUniqueIndexesOver` ignores partial indexes when checking whether a global rule exists. `ensureTerminalPaneUniqueIndex` first reuses an existing complete rule; otherwise it creates one under an available name. The canonical migration name is a preference, not proof that a matching rule exists and not permission to overwrite an existing index. It checks the SQLite catalog case-insensitively and selects an unused suffixed name under the migration write lock. Both v3 constraint consumers use this helper.

**Why this choice / limits:** Independent review found that the first partial-index correction would collide when a partial or unrelated index already occupied `terminal_panes_tmux_target_unique` or `terminal_panes_address_unique`. Choosing a free name preserves those definitions while actually enforcing the intended global rule; `CREATE ... IF NOT EXISTS` under the occupied name could silently retain only the inadequate rule. Existing partial and unrelated indexes are not discarded. Obsolete label constraints are intentionally handled differently under TWS3. TWS4 provides the ordered delivery path for databases already marked version 5; editing historical v3 alone cannot reach them.

**Regression coverage:** `the v3 migration enforces target uniqueness beyond a legacy partial unique index` upgrades a private v2 database, preserves its original target, and requires both a direct SQL collision and an API adoption attempt to be refused without changing the second pane. `the v3 migration preserves occupied canonical index names while enforcing global terminal identity` covers partial indexes using canonical names and unrelated constraints occupying those names. It requires preserved state, fresh same-label terminal identity, oldest-label lookup, rejected target collisions and continuing enforcement of the unrelated group/viewer rules.

**Files / symbols:** `remote/src/workspace.ts` — `terminalPaneUniqueIndexesOver`, `ensureTerminalPaneUniqueIndex`, historical v3 migration; `remote/test/workspace.test.ts` — partial-index and occupied-name regressions. Shared migration documentation: `docs/SPEC.md`, `docs/DEVELOPMENT.md`.

### TWS3 — Remove every obsolete rule that treats a reusable terminal label as identity

**Category / impact:** Migration correctness and terminal creation. A valid replacement terminal could be refused because an obsolete uniqueness constraint remained installed.

**Context:** A terminal's layout label, such as its slot name, is not its durable row identity or its remote shell target. Schema 4 intentionally allowed a fresh terminal row to reuse an older row's label. Older databases can express the obsolete `UNIQUE(dev_session_id, name)` rule in several equivalent ways: table-level constraints, multiple named indexes, reversed column order, or partial indexes applying only to some rows.

**Before-case / root cause:** [INFERENCE] The old migration stopped after dropping one matching named index and recognized only one column order. Additional equivalent rules could remain after the database was stamped version 5. Reversed-key rules escaped discovery entirely; partial rules could still reject selected legitimate labels. Main's workspace report records an observed surviving-label-uniqueness failure against baseline production in `build/dev/.tmp/hunt-r3/before-workspace.txt`. The separately supplied already-upgraded proof under TWS4 demonstrates why fixing this historical step also needed a new delivery migration.

**Fix:** The index detector recognizes the exact set of unique key columns regardless of order. The address-constraint removal path explicitly includes partial indexes and removes every matching named rule. If a table-level autoindex represents the old rule, a table rebuild is required because that autoindex cannot be dropped independently. The final shared helper, `dropNamedTerminalPaneAddressIndexes`, continues through all named variants even when it finds an autoindex, and reports whether a rebuild is needed. Global target uniqueness remains enforced. The collision-safe index creation helper described under TWS2 also protects the historical v3 path leading into this migration.

**Why this choice / limits:** Remove the obsolete constraint, not the data that exposes it. No pane is deleted or given a new target merely to make a label available. A unique index over the same columns in reverse order enforces the same forbidden rule; a partial version is still obsolete for the subset it selects. Necessary table rebuilds preserve unrelated named indexes and triggers through the TWS5 correction. Existing orphan rows retain the established migration treatment: preserve them rather than silently delete them or invent parents, while normal mutations continue enforcing foreign keys.

**Regression coverage:** `the v4 migration removes every legacy full and partial label uniqueness constraint` covers file-backed fixtures with and without the table-level constraint, alongside redundant named, reversed-key and partial rules. After upgrade, same-label creation must produce a distinct row and target while preserving the original row/target; legacy by-label lookup must still choose the oldest row; direct SQL target collision must still fail. The occupied-canonical-name regression described under TWS2 also checks successful same-label creation after its v3/v4 upgrade.

**Files / symbols:** `remote/src/workspace.ts` — `terminalPaneUniqueIndexesOver`, `dropNamedTerminalPaneAddressIndexes`, `migrateDropTerminalPaneAddressUnique`, `rebuildTerminalPanesWithoutAddressUnique`; `remote/test/workspace.test.ts` — exhaustive label-rule regression. Shared migration documentation: `docs/SPEC.md`, `docs/DEVELOPMENT.md`.

### TWS4 — Deliver the terminal-constraint repairs to databases already marked upgraded

**Category / impact:** Upgrade delivery and persistent data integrity. Correcting old migration functions was insufficient for users whose databases had already passed through those functions and been stamped version 5.

**Context:** The migration runner deliberately returns cheaply when the stored version equals the build's current database version. It does not replay every historical migration on every open. That is important for ordinary opens, but means a newly corrected v3 or v4 implementation cannot repair an already-version-5 file by itself. This affects both obsolete label rules left by v4 and incomplete target enforcement accepted by v3.

**Before-case / root cause:** Main's actual private-fixture investigation, recorded in `local://round3-already-upgraded-before.txt`, used baseline production code to upgrade a schema-3 database containing two old label-unique indexes. Baseline dropped only one and recorded 5. Opening that file with the first corrected historical-migration code still reported 5, and normal same-label creation still failed with `UNIQUE constraint failed: terminal_panes.dev_session_id, terminal_panes.name`. The fields `oldMigrationVersion:5`, `currentReopenVersion:5` and `legitimateLabelReuseStillRejected:true` establish a still-broken delivery path. The investigation's exit 0 meant that it confirmed the defect, not that the product behavior was correct.

**Observed after-case:** Main's subsequent production-to-production smoke is recorded in `local://round3-already-upgraded-after.txt`. Baseline production first genuinely upgraded the private historical fixture to version 5; current production then repaired it to 6. A normal API call successfully created a terminal reusing the original label, the original decoded terminal record remained exactly equal, the new row and target were independent, and a second open preserved both accepted rows. The recorded output has `oldMigrationVersion:5`, `repairedVersion:6`, `labelReuseAccepted:true`, `originalRowPreserved:true`, `independentTargets:true` and `secondOpenStable:true`, with exit 0. The fixture files were removed, and no production database or real tmux process was involved. This establishes the already-upgraded label-reuse repair case specifically; it does not stand in for the separate partial-target, rollback, audit-trigger or full-suite results.

**Fix:** Add a real ordered migration to SQLite workspace schema 6. `WORKSPACE_SCHEMA_VERSION`, the SQL seed, and informational `WorkspaceDb::kSchemaVersion` now agree on 6. The version-6 step runs within the existing migration transaction: acquire `BEGIN IMMEDIATE`, reread the stored version under the lock, apply needed steps, record the new version, and commit or roll back together. Already-current opens keep the cheap pre-check rather than taking an unconditional repair write lock.

The repair separates target repair from v3's historical label deduplication. Version 6 must not replay that label-changing path: duplicate labels are legitimate now. It first removes all obsolete named label rules, remembers whether a table-level autoindex requires a rebuild, repairs targets only if no complete global target rule exists, and then performs any required rebuild. Names, IDs, metadata and layouts remain intact; valid targets are not regenerated. For target conflicts, the oldest holder keeps its target and later rows follow the existing reserved-canonical-target/NULL policy. Replacements are planned before writes, and all changing targets are first set to permitted NULL values before final assignment inside the transaction.

**Why this choice / limits:** Independent review identified a necessary ordering correction: an obsolete label index filtered by `tmux_target IS NULL` could reject the second temporarily vacated row if target changes preceded named-index removal. Therefore named obsolete rules are removed first, while the table rebuild waits until targets satisfy global uniqueness. Review also identified that the shared target-repair helper introduced this same temporary-NULL conflict in the historical v3 path from stored version 2; baseline's direct non-NULL replacement assignments did not introduce that transient conflict. Version 3 now removes obsolete named label predicates before invoking shared target repair, then retains its historical distinct-label repair and temporary complete label constraint. Version 4 subsequently removes that constraint through the established preservation-aware path. Version 6, in contrast, must preserve valid duplicate labels and never replay v3 label deduplication. Both ordering corrections complete TWS4, not additional issue labels. TWS5 ensures necessary rebuilds retain unrelated named rules and triggers. No migration contacts, kills or renames a real tmux process. Existing foreign-key restoration, ownership, row ordering and orphan preservation remain. This is a versioned compatibility repair, not a general repair pass for arbitrary future manual schema edits.

The release remains 0.4.3. RPC schema and current client minimum remain 10; `server.info.schemaVersion` still means RPC 10. Database 6 is storage metadata, not an RPC compatibility gate. The historical statement that pinned sessions arrived in database version 5 remains correct.

**Regression coverage:** Four named tests in `remote/test/workspace.test.ts` cover the delivery and transition boundaries:

- `the v6 migration repairs obsolete label constraints already stamped v5 and preserves current fast opens`: named and autoindex leftovers, complete workspace/orphan preservation, fresh same-label identity, retained target uniqueness, and a current reopen while another connection holds `BEGIN IMMEDIATE`.
- `the v6 migration repairs partial-only v5 targets without renaming valid duplicate labels`: arbitrary/canonical partial-index names, preserved names/IDs/metadata/layouts, oldest target retention, separated later targets and stable reopen/oldest-label lookup.
- `the v6 migration rolls back target repairs and constraint changes if version recording fails`: an injected final version-write failure must leave the complete original catalog, rows and version 5 intact; removing only the fixture's failure trigger permits a retry with the intended constraints.
- `workspace migrations remove NULL-filtered label rules before vacating duplicated targets`: private stored-version-2 and stored-version-5 fixtures each contain three same-label/shared-target rows, a partial-only target rule and a NULL-filtered obsolete label rule. Both upgrade paths must remove the named predicate before temporary NULL assignments, preserve pane IDs/other metadata/layouts, allow later same-label creation, enforce global SQL target uniqueness and retain accepted rows/targets plus oldest-label lookup after reopen. The version-2 path must retain historical v3 distinct-label repair; the version-5 path must preserve its legitimate duplicate labels.

These describe assertions, not current passing-run claims.

**Files / symbols:** `remote/src/workspace.ts` — `WORKSPACE_SCHEMA_VERSION`, `MIGRATIONS`, `migrate`, `migrateTerminalPaneTargets`, `migrateTerminalPaneConstraints`, `dropNamedTerminalPaneAddressIndexes`; `remote/sql/schema.sql` — seed 6 and migration explanation; `remote/test/workspace.test.ts`; `src/persistence/WorkspaceDb.h` — informational `kSchemaVersion`; coordinated current-version fixtures/parity coverage in `src/persistence/tests/tst_workspacedb.cpp`; `docs/SPEC.md`, `docs/DEVELOPMENT.md` — ordered repair and version distinctions.

### TWS5 — Rebuilding the terminal table must retain its unrelated indexes and triggers

**Category / impact:** Migration data integrity and preservation of database-maintenance behavior. Removing one obsolete table constraint must not silently remove other constraints or future-write auditing.

**Context:** SQLite implements a table-level UNIQUE rule using an autoindex. Removing that old label rule requires copying terminal rows into a replacement table, dropping the old table, and renaming the replacement. Named indexes and triggers belong to their table: `DROP TABLE` removes them as well. An index can enforce an independent maintenance rule; a trigger can record future inserts, updates or deletes in an audit table.

**Before-case / root cause:** [INFERENCE] The historical rebuild reconstructed the canonical terminal columns and target rule but did not preserve other named indexes or triggers owned by `terminal_panes`. A valid old database could additionally enforce unique `startup_command` values and maintain write-audit triggers. Rebuilding it to remove label uniqueness would silently lose those definitions, allowing later conflicting writes and stopping future auditing. This affected historical v4 rebuilds and the new v6 repair of version-5 leftovers. Earlier tests preserving unrelated constraints on other tables did not cover objects owned by the table being dropped. The finding came from independent source review; no separate observed before-run is claimed.

**Fix:** After intentionally removing obsolete named label indexes, capture the remaining terminal table's named index and trigger SQL definitions from `sqlite_schema`. Copy and swap the table, then replay those original definitions inside the same migration transaction, with indexes before triggers. Replay the catalog SQL verbatim so quoted names, partial-index predicates, expressions and trigger bodies retain their original meaning. Intentionally removed identifiers still use `quoteIdentifier`.

If a named complete target-unique rule is being preserved, do not add a second implicit full target index to the replacement table; otherwise restore the canonical table-level target rule. Canonical primary-key and other canonical autoindexes are supplied by the recreated table constraints. A replay failure or final version-recording failure rolls back schema objects, rows and the version marker together.

**Why this choice / limits:** Recreate triggers after the copy so copying existing rows does not pretend they are new future-write INSERT events. Actual target repairs remain real updates and may legitimately invoke existing update triggers; the fix does not suppress those. Preservation is for existing named terminal indexes and triggers, not arbitrary extra columns or custom table-level constraints beyond the canonical schema. Existing orphan rows remain preserved through the established migration framework. No real terminal service is contacted. This correction belongs to the same rebuild helper used by TWS3/TWS4, not a second parallel migration convention.

**Regression coverage:** `terminal table rebuilds retain maintenance constraints and future-write triggers without auditing the copy` covers both stored-version-3 and already-version-5 autoindex fixtures. Quoted-name startup uniqueness, a named complete target rule and quoted insert/update/delete audit triggers must survive; original state must remain; migration must add no copy audit events; exactly one complete target rule must remain; conflicting creates/updates must fail; real later writes must produce the expected audit events. `terminal table rebuild rollback restores maintenance constraints and audit triggers` injects a final version-write failure, requires unchanged catalog/workspace/version 5 and no copied-row audit events, then retries after removal of only the owned failure trigger and checks continuing uniqueness and a real insert audit event.

**Files / symbols:** `remote/src/workspace.ts` — `rebuildTerminalPanesWithoutAddressUnique`, `terminalPaneUniqueIndexesOver`, `quoteIdentifier`; `remote/test/workspace.test.ts` — maintenance-rule, auditing and rollback regressions; `docs/SPEC.md`, `docs/DEVELOPMENT.md` — preservation, ordering and limits.

## Remote files, revisions, reads, and watchers

### TFS1 — File revisions and watch identity must include the filesystem device

**Category / impact:** Save-conflict detection and file-watch correctness. Equal inode numbers on different filesystems must not make different files appear to be the same save target.

**Context:** An editor reads a file and later supplies the returned opaque revision when saving. The file service uses filesystem metadata to detect intervening changes. A symlink can be retargeted between read and save. Inodes identify files only within one filesystem; a device ID identifies which filesystem the inode belongs to. File-watch handles also need to follow the actual target, not just a coincidentally equal inode number.

**Before-case / root cause:** [INFERENCE] `revisionFrom` included inode, size, modification time and change time but omitted device. Mounted clones or snapshots can expose distinct files with matching values for all four old fields. Retargeting a link between those files could let the old revision authorize a write to the new target. Watch rearming separately compared only `watchedIno` and could leave a handle on the former filesystem. Main reports an actual baseline failure of the authored cloned-device fixture: the write was unexpectedly accepted. That deterministic fixture uses real files/symlinks and a stat hook; it does not claim a privileged real-mount reproduction.

**Fix:** Require and include `Stats.dev` in `revisionFrom`. Preserve and compare both device and inode during initial watch setup, watcher arming and change reconciliation, and clear both when a handle is dropped or errors. Remove an unrelated exact revision-format assertion instead of replacing it with a new incidental token spelling. Real callers already supply full `fs.Stats`; the only manual four-field fixture was that removed format assertion.

**Why this choice / limits:** The revision remains an opaque string with unchanged wire shape and no compatibility alias. Device identity closes this cross-filesystem identity gap; it does not turn metadata into a content hash or prevent mutations after the final revision check. Existing-file publication remains the documented check-then-rename operation.

**Regression coverage:** `writeFile rejects a symlink retargeted to a cloned inode on another device` in `remote/test/files.test.ts` requires a revision mismatch, unchanged bytes in both targets and no staging leftovers. This directly checks save behavior rather than token formatting. Main's `build/dev/.tmp/hunt-r3/before-files.txt` records this baseline failure along with the other three TFS cases; no current passing-run claim is made here.

**Files / symbols:** `remote/src/files.ts` — `revisionFrom`, `FileWatchService.watch`, `armWatcher`, `diffAndEmit`; `remote/test/files.test.ts`; `docs/SPEC.md` — opaque revision/watch identity contract.

### TFS2 — Do not normalize a missing directory away before saving through `..`

**Category / impact:** Filesystem path correctness. A save could create a different file from the path that the operating system could actually traverse.

**Context:** A save can legitimately target a new file or follow a dangling symlink whose eventual file does not exist yet. When `realpath` cannot resolve such a path, `resolveLinkChain` resolves components itself. Unlike purely textual normalization, kernel traversal of `missing/../target.txt` requires `missing` to exist as a directory before the parent traversal can succeed.

**Before-case / root cause:** [INFERENCE] The fallback accepted an absent component as a prospective parent directory, then immediately applied `dirname` on `..`, forgetting that unresolved component. A dangling link with `missing/../target.txt` could therefore lead a create-only save to create a sibling `target.txt`, while reading through the original link still failed with ENOENT. Direct raw paths and longer missing-component sequences had the same problem; stat could report misleading writability using the wrong sibling directory. Main's actual baseline fixture reported creation of a target that was required to remain absent. This is a wrong-effective-path bug, not a change to the service's intentional ability to access paths outside a repository.

**Fix:** Remember when traversal has encountered a missing component and reject with ENOENT if `..` would cross it. Do not erase that component, create directories during resolution or add extra filesystem calls. Continue following existing parent symlinks before applying `..`, and continue supporting ordinary new nested destinations without the invalid backwards traversal.

**Why this choice / limits:** A service should not claim a save succeeded at a path the kernel still cannot read. It also should not invent intermediate directories merely to make `missing/..` traversable. Ordinary dangling links to new nested files remain supported. No repository sandbox or new outside-root restriction is introduced.

**Regression coverage:** `writeFile does not collapse a missing directory before '..'` in `remote/test/files.test.ts` covers raw paths and dangling symlinks with one or multiple missing components. It requires ENOENT, non-writable status for the dangling link, unchanged symlink text and no unintended sibling files or invented directories. Main's `build/dev/.tmp/hunt-r3/before-files.txt` records the real baseline failure; no current passing-run claim is made here.

**Files / symbols:** `remote/src/files.ts` — `resolveLinkChain` and its stat/write consumers; `remote/test/files.test.ts`; `docs/SPEC.md` — missing-component/parent-traversal save contract.

### TFS3 — Watch teardown must reject subscriptions created inside its own callbacks

**Category / impact:** Resource lifetime and teardown. A callback could create a watcher that escaped the shutdown operation intended to remove every current subscription.

**Context:** `FileWatchService.closeAll` removes subscriptions and sends their `closed` notifications. Those subscriber callbacks run synchronously: a callback can call back into the service before `closeAll` finishes. Creating a watch reserves its subscription ID before its first asynchronous wait, so reentrant admission can happen immediately even though initialization finishes later.

**Before-case / root cause:** [INFERENCE] `closeAll` took a snapshot of subscription IDs and unwatched that snapshot. A closed subscriber could create a replacement ID not present in the snapshot; its later initialization could install a live OS watcher or polling timer after teardown returned. A nested `closeAll` call also means a simple guard must not be cleared by the nested invocation while the outer teardown remains active. Main reports the actual baseline regression admitted the reentrant replacement watch.

**Fix:** Gate new watch admission for the synchronous duration of `closeAll`. Nested teardown returns without resetting the outer guard, and a `finally` block restores normal service reuse when the outer operation finishes. Previously reserved in-flight watches still use their existing closed/subscription checks for cleanup. No new public error code or exported API is introduced.

**Why this choice / limits:** Teardown should not admit new resources through callbacks it is currently invoking. The service remains reusable afterward; this is not a permanently closed state and does not reject a later ordinary subscription. The guard addresses the reentrant admission window rather than repeatedly chasing newly created watches.

**Regression coverage:** `closeAll rejects reentrant watches but permits a later subscription` in `remote/test/files.test.ts` has a closed subscriber recursively call `closeAll` and attempt a replacement watch. Assertions require that replacement to reject, the original subscription to be absent, and a later normal watch/unwatch to succeed. The fixture removes its listener before final cleanup so its own teardown cannot generate a leaked replacement. Main's `build/dev/.tmp/hunt-r3/before-files.txt` records the observed baseline failure; no current passing-run claim is made here.

**Files / symbols:** `remote/src/files.ts` — `FileWatchService.watch`, `closeAll`, `unwatch`; `remote/test/files.test.ts` — reentrant teardown/reuse regression. Source comments explain the admission lifetime.

### TFS4 — An empty-file read must notice growth after it observes EOF

**Category / impact:** Read consistency and truthful completeness metadata. A stale empty result must not be advertised as a complete current document.

**Context:** Some procfs/sysfs-style pseudo-files report a stat size of zero even when reading them produces content. The file service therefore has a read-to-EOF path for initially zero-size files. Ordinary empty files enter that same path. A final descriptor metadata check is what can detect a writer changing the opened file during the read.

**Before-case / root cause:** [INFERENCE] `readFile` skipped its final metadata check for every initially zero-size file, not just the pseudo-file behavior that motivated the special path. If an external writer appended immediately after the read observed EOF, the response could contain empty content, its old revision and `truncated:false` despite the file now holding bytes. Main's real baseline regression specifically observed this failure: the file grew, but the returned read still had `truncated:false`. This is recorded in the TFS owner report and `build/dev/.tmp/hunt-r3/before-files.txt`, not merely inferred from source.

**Fix:** Always obtain final metadata from the opened descriptor. If the file initially had size zero and now has nonzero size, invalidate the read's completeness. For initially nonzero files, keep the existing full revision comparison. A pseudo-file whose size remains zero still ignores metadata-only restamping, retaining the established procfs behavior.

**Why this choice / limits:** The correction distinguishes meaningful ordinary-file growth from the intentionally tolerated timestamp behavior of zero-size pseudo-files. It does not overwrite the newly appended data, retry the read or claim to prevent changes after the final metadata check.

**Regression coverage:** `readFile marks an initially empty file as stale when it grows after EOF` in `remote/test/files.test.ts` uses a real descriptor and a narrow read hook to append bytes after EOF but before final metadata collection. It requires the returned empty content to be marked truncated and the actual appended bytes to remain intact. The top-level `FileHandle` type import supports that real-descriptor fixture. No current passing-run claim is made here.

**Files / symbols:** `remote/src/files.ts` — `readFile`; `remote/test/files.test.ts` — EOF-growth regression and top-level fixture type import. Source comments explain the zero-size distinction.

## Agent integration, MCP tools, and live model tests

### TIN1 — Refuse malformed advertised viewer endpoints before attempting a connection

**Category / impact:** Integration routing and configuration validation. Viewer commands must not select a different window merely because an agent runs from a different directory.

**Context:** The application advertises its per-window control socket through `CODEHARBOR_CONTROL_SOCKET`. Viewer tools use that route to send operations back to the window that launched them. A relative pathname depends on the caller's current directory, unlike the required absolute route. Embedded NUL is also not a valid advertised pathname.

**Before-case / root cause:** [INFERENCE] `sendControlRequest` checked only that the environment value was nonempty before passing it to `net.Socket.connect`. A reachable relative value could therefore contact a listener relative to the agent's repository directory, not an unambiguous advertised endpoint. NUL-containing values reached a low-level connection error rather than a configuration refusal. The event producer already rejected non-absolute explicit routes; the viewer path lacked that corresponding check. No production misrouted-window occurrence is claimed.

**Fix:** Import `node:path` at top level and reject non-absolute or NUL-containing endpoint values before request serialization or connection. Return `bad_request` with an actionable environment-variable diagnostic. No fallback route is attempted. Main also updates the pre-existing NUL-endpoint assertion in the shared control test from `failed` to `bad_request`, reflecting the intentional earlier refusal instead of preserving an obsolete connection-error expectation. That integration adjustment is part of TIN1, not another finding.

**Why this choice / limits:** The advertised route should be independent of current working directory. Valid absolute endpoints, operation identities and exported signatures remain unchanged. This validates route syntax, not peer authentication or ownership, and adds no guessed portable socket-path length limit.

**Regression coverage:** `invalid advertised control endpoints are refused instead of dialled (TIN1)` in `remote/test/viewctl.test.ts` binds a private real answering listener, supplies both its reachable relative spelling and a NUL-containing spelling, then requires JSON refusal/exit behavior and zero dispatched operations. The relative case would otherwise receive a real success, so it does not rely on connection timing or a mocked socket. `remote/test/control.test.ts` retains the existing NUL-route case with its corrected public error-code expectation. No current passing-run claim is made here.

**Files / symbols:** `remote/src/control-client.ts` — `sendControlRequest`, top-level path import; `remote/test/viewctl.test.ts`; `remote/test/control.test.ts` — shared NUL-endpoint assertion. The endpoint contract is coordinated into canonical integration documentation.

### TIN2 — Malformed MCP tool names must receive errors under the caller's request ID

**Category / impact:** Protocol error classification and response correlation. Malformed user input could turn a normal invalid-parameters reply into an internal error with an unrelated null ID.

**Context:** MCP clients call named tools through JSON messages. The server must validate user-controlled parameters and reply using the incoming request ID so the client can finish the correct pending call. An unknown string tool name and a non-string tool name are different validation cases.

**Before-case / root cause:** [INFERENCE] The `tools/call` branch looked up `params.name` and interpolated `String(params.name)` when lookup failed. JSON permits an object with a `toString` property set to null; converting that object to a string throws. The outer line handler then classified the exception as internal error `-32603` with ID null rather than invalid parameters `-32602` under the caller's ID. Other non-string names were also treated as unknown tools instead of malformed parameters. No observed external MCP incident is claimed.

**Fix:** Require a string tool name before lookup, and interpolate only that validated string in the unknown-tool diagnostic. Existing unknown-string behavior remains. The correction stays in parameter validation rather than suppressing arbitrary exceptions in the outer provider handler.

**Why this choice / limits:** A malformed name should be rejected without invoking object-to-string conversion at all. This preserves request correlation and does not change the tool catalog, schemas, exported interfaces, lifecycle, cancellation, concurrency limits or supported MCP revisions.

**Regression coverage:** `malformed tool names are invalid params with the caller's id (TIN2)` in `remote/test/mcp.test.ts` sends missing, null, boolean, number, array and object names through the real JSON-line handler, including the throwing-conversion object. Assertions require correlated invalid-params responses, then a successful ping conversation to establish continued handling. These are the intended assertions.

**Files / symbols:** `remote/src/mcp/server.ts` — `handleMessage` `tools/call` validation and its `handleLine` consumer; `remote/test/mcp.test.ts` — malformed-name conversation regression.

### TIN3 — Make the real paid-agent integration explicit and isolate its writable state

**Category / impact:** Test safety, cost control and developer-state isolation. An ordinary suite must not invoke a paid model or write into the developer's active agent/CodeHarbor state just because `omp` is installed.

**Context:** The Oh My Pi lifecycle fixture intentionally exercises a real harness, the shipped extension and a private bridge, rather than a mock that merely echoes expected events. It can make a real model call. Developers commonly run tests from an actual CodeHarbor terminal, with production event routes and ordinary profile, cache, history and configuration selectors already present in the environment.

**Before-case / root cause:** [INFERENCE] The fixture enabled itself whenever `omp` existed, inherited `process.env`, and overrode only the runtime directory and fake session coordinates. The event resolver gives inherited `CODEHARBOR_AGENT_SOCKET` priority over the runtime directory, so fake `sess-live`/`term-live` events could go to the developer's real daemon while the private bridge received none. The ordinary HOME/profile/current directory, extension and context discovery, and session persistence were also inherited, permitting a model request and normal state/history writes during a routine suite. No actual paid-path invocation, production-state write or model call is claimed as observed during this repair.

**Fix:** Require `CH_LIVE_OMP=1`; without it, do not even probe the executable. For opted-in execution, explicitly advertise the private fake bridge endpoint and use fixture-owned cwd/PWD, HOME, agent/session directories, XDG runtime/data/state/config/cache roots, CodeHarbor database, tmux temporary directory and general temporary directory. Clear the inherited control endpoint and `TMUX`, and direct shell history to `/dev/null`.

Select the default profile with the intentionally empty `OMP_PROFILE=""` and clear legacy `PI_PROFILE`. Pin `PI_CONFIG_DIR` to the private-home default `.omp` and clear `PI_CONFIG_FILES` overlays. Relocate documented writable native extraction, authentication-broker snapshot, autoresearch database, worktree, GitHub cache, Claude configuration, Cursor debug and GitLab trace destinations into the fixture; disable TUI write logging. Invoke the real harness with session persistence, tools, discovered extensions, skills, rules, context files, title generation and LSP disabled. The explicit shipped extension under test remains enabled.

**Why this choice / limits:** Independent review showed why private HOME and merely clearing `OMP_PROFILE` were insufficient: inherited `PI_PROFILE=work` could select a named profile that ignores `PI_CODING_AGENT_DIR`, and inherited `XDG_STATE_HOME` could route logging initialization and pruning to ordinary state. An empty modern selector takes precedence over the legacy selector; both profile selection and writable state roots are now handled explicitly. This conclusion came from upstream `resolveProfileEnv`, `DirResolver` and `getLogsDir` source plus documented environment selectors, not a production-profile experiment. Provider credentials and transport settings may intentionally remain inherited so an explicitly opted-in real model run can authenticate. Asset paths and proxy settings are not treated as writable state selectors. This is not a general sandbox against a malicious executable or undocumented future variables.

**Regression coverage:** The existing `a real omp agent reports its whole lifecycle to the bridge` test in `remote/test/oh-my-pi-live.test.ts` remains the real behavior test; it was not replaced with a mock or a source-text isolation test. When deliberately enabled, its assertions require the real process to exit successfully, events to carry the intended harness/session/terminal identities, `starting` and `session_start` first, a running phase before `idle_unseen`, and no error state. By default the test is skipped, including its executable probe. Main may run the paid path only deliberately with `CH_LIVE_OMP=1` and environment-based model credentials. No real-model execution or passing paid-path result is claimed here.

**Files / symbols:** `remote/test/oh-my-pi-live.test.ts` — `SKIP`, `ompAvailable`, the child process arguments/environment and lifecycle assertions. Upstream correction sources: [profile/directory resolver](https://github.com/can1357/oh-my-pi/blob/main/packages/utils/src/dirs.ts) and [documented environment selectors](https://github.com/can1357/oh-my-pi/blob/main/docs/environment-variables.md#6-storage-and-config-root-paths). Existing fixture-isolation guidance was consulted in `tests/live/generate-fixture.sh` and `tests/live/check-fixture-isolation.sh`; those files are not new production changes under TIN3.

## RPC transport and terminal-reply ownership

### TRP1 — A transport-bound notification must identify the connection that produced it

**Category / impact:** Native connection lifetime and reentrant callback correctness. An obsolete connection notification could initialize the final peer twice, including duplicate editor file-watch setup.

**Context:** `CodeharbordClient` changes the transport used for daemon RPC and notifies consumers when a binding is ready. Qt direct signal callbacks run synchronously, one after another; a callback can change the transport before later callbacks receive the original signal. A generation number identifies a particular binding even if the client object itself stays the same.

**Before-case / root cause:** [INFERENCE] `finishTransportBinding` emitted an argumentless `transportBound()`. An early listener could replace the intermediate transport, causing a nested bound signal for the final transport before the original broadcast resumed. A later listener then received the final event followed by the retired intermediate event, with no identity to distinguish them. The editor's `onTransportBound` re-establishes peer-owned watches, so the stale event could supersede watch setup already started for the final peer and establish another one. A generation check before emitting cannot protect later callbacks after another callback has reentered during that same broadcast. No observed production duplicate-watch incident is claimed.

**Fix:** Change the signal to `transportBound(quint64 generation)` and emit the binding generation captured before draining. Require consumers to ignore a notification whose argument differs from `transportGeneration()`, matching the existing changing/closed identity convention. Migrate the production `EditorController::onTransportBound(quint64 generation)` consumer to that guard. No argumentless compatibility signal or alias remains. The corresponding editor regression is coordinated with this API change rather than counted as another issue.

**Why this choice / limits:** Qt cannot retract the remaining callbacks of a signal broadcast already in progress; consumers need enough identity to recognize the obsolete notification. Capturing the generation identifies the event that actually began, rather than relabeling it with whichever transport happens to be current later. Existing same-live-device no-op behavior and synchronous request/response behavior remain unchanged. Test spies and callbacks that accept fewer signal arguments remain valid Qt consumers; generation-aware production behavior is explicit.

**Regression coverage:** `TstRpcClient::transportBoundGenerationPreventsDuplicatePeerInitialization` installs an early replacing listener and a later generation-aware request issuer. Assertions require the final peer to receive one workspace request, its real response to complete once, and no pending request to remain. `TstEditorController::reentrantTransportBindKeepsOneCurrentWatch` exercises the actual editor consumer with private local sockets: one watch and follow-up stat complete on the final peer, no duplicate request follows, and teardown releases exactly `final-watch` on that peer. These assertions cover the coordinated consumer cutover, not just signal argument shape.

**Files / symbols:** `src/remote/CodeharbordClient.h` and `src/remote/CodeharbordClient.cpp` — `transportBound`, `finishTransportBinding`, `transportGeneration`; `src/remote/tests/tst_rpcclient.cpp`; `src/editor/EditorController.h` and `src/editor/EditorController.cpp` — `onTransportBound`; `src/editor/tests/tst_editorcontroller.cpp` — actual watch regression.

### TRP2 — Validate that a resolved terminal reply belongs to the requested row and workspace

**Category / impact:** Native RPC reply validation and authoritative terminal identity. A well-typed success response must not be accepted for a different pane, session or server.

**Context:** The native `WorkspaceDb` facade asks the daemon to resolve a terminal pane. Modern requests prefer a durable row ID; legacy requests can use a reusable slot label within a Dev Session. The returned `tmuxTarget` is authoritative and is consumed by terminal creation. Correct field types alone do not prove that a reply answers the address requested.

**Before-case / root cause:** [INFERENCE] `resolveTerminalPane` chose the right outgoing row-first address but passed every successful result to the generic `parseTerminalPane` decoder without comparing ownership or identity. That decoder checked a nonempty record ID and field types, while absent scope fields could become empty strings. A reply for another row, server or session could therefore be delivered as successful terminal resolution, potentially directing the consumer to the returned unrelated shell. This is a source-derived risk, not an observed user shell mix-up. The separate terminal-factory cache-key issue remains under its original terminal-owner label and is not duplicated here.

**Fix:** Add method-specific response validation in `WorkspaceDb::resolveTerminalPane`: require the exact requested server and Dev Session, then exact row ID for a row-addressed lookup or exact slot label for a legacy lookup. Missing required ownership fields and mismatches return the facade's existing malformed-result error `-32603` without adopting a pane. Update the malformed-record diagnostic and facade API comments to describe identity checks as well as structural decoding. Existing server-side errors still flow through unchanged.

**Why this choice / limits:** A row ID remains identity even if the pane has been renamed. Therefore a row-addressed reply with the right owner and row may have a different label; a supplied old label must not wrongly reject that legitimate rename. A legacy label lookup still needs the requested label because it has no durable row address. The fix does not invent IDs or tmux targets, broaden generic record models, change unrelated nullable/default fields, add retries or alter the exported facade API.

**Regression coverage:** Data-driven `TstWorkspaceDb::resolveTerminalPaneRejectsUnrelatedRows` drives real `WorkspaceDb` requests through `CodeharbordClient` and private `QLocalSocket` request/response handling. Cases cover wrong row, wrong or missing server/session, legacy wrong or missing label, and legacy wrong owner. Positive cases retain server-created tmux targets and accept renamed row-addressed panes. Assertions therefore cover the facade callback's accepted/rejected result, not a copied decoder or source-text comparison.

**Files / symbols:** `src/persistence/WorkspaceDb.cpp` — `WorkspaceDb::resolveTerminalPane`, use of `parseTerminalPane`, malformed-result diagnostic; `src/persistence/WorkspaceDb.h` — resolution identity contract; `src/persistence/tests/tst_workspacedb.cpp` — data-driven reply-ownership coverage. The informational database-version update in `WorkspaceDb.h` belongs to TWS4, not a third TRP finding.

**Caller-test migration after integration:** Successful terminal-factory responses now retain the server ID captured with their original request, including delivery after a switch; explicit foreign/malformed reply overrides remain for negative cases. The shell-quoting fixture requests the same adversarial session/row IDs it returns rather than accidentally testing a foreign row. AppController legacy fixtures keep the requested label `terminal-1` separate from persistent row ID `born-row`; reconnect fixtures use the captured request’s server/session/row ownership. The stricter production decoder was not weakened to accommodate invalid canned replies.

## SSH authentication, transport lifetime, and live fixtures

### TSH1 — An unfinished SSH authentication exchange could invent the server's supported methods

**Category/impact:** Authentication failure handling and diagnostic correctness. An incomplete exchange could produce misleading advice and, in one branch, continue into identity or credential handling without a completed server response.

**Context:** Before choosing a key, password, or keyboard-interactive prompt, the SSH connection pool asks libssh to perform the initial `none` authentication exchange. A completed response can establish which methods the server offers. An error or a request still waiting for completion cannot establish that information.

**Before-case/root cause:** [INFERENCE, source-derived baseline behavior.] `authenticate()` reset its public-key observation before calling `ssh_userauth_none()`. An `SSH_AUTH_ERROR` stopped authentication, but failure formatting interpreted the missing observation as an affirmative public-key refusal and password-only offer. `SSH_AUTH_AGAIN` was worse: it fell through the result classifier as a refusal, queried an unestablished method list, and could use the legacy empty-list fallback to proceed toward identity scanning or credential callbacks. Absence of a completed response was being confused with a completed denial whose method list happened to be unspecified.

**Fix:** A separate `m_authMethodsKnown` flag records whether method information is meaningful. Only `SSH_AUTH_SUCCESS`, `SSH_AUTH_DENIED`, and `SSH_AUTH_PARTIAL` allow the initial exchange to reach method discovery and the normal authentication sequence. ERROR, AGAIN, and other incomplete or unexpected outcomes stop before identity scanning, inferred methods, or credential callbacks. Failure text retains libssh's actual nonempty diagnostic and explains that the initial exchange did not complete, without inventing method offers, agent attempts, or private-key attempts. The wording does not claim that every AGAIN result means the transport has disappeared.

**Why this choice/limits:** Completed responses retain the existing method selection, credential types, caching, per-operation timeouts, partial-factor behavior, and legacy zero-mask fallback. The fix neither retries incomplete initial exchanges nor adds an overall handshake deadline.

**Regression coverage:** `tst_sshconnectionpool::aFailedAuthenticationExchangeDoesNotInventServerMethods` uses an owned Unix socket pair and an invalid overlong banner to create a genuinely failed libssh session. It checks authentication failure, zero credential callbacks, preservation of the actual libssh diagnostic, and absence of invented password-only, public-key, agent-environment, or private-key-file advice. It reads only an explicitly supplied empty configuration and skips this transport-specific fixture on non-Unix platforms. Main's supplied `local://round3-auth-proof.txt` records focused current-code observations on Qt 6.10.2/libssh 0.11.3: the native failed-banner session returned AGAIN and the selected regression passed; an external `LD_PRELOAD` override separately forced ERROR, preserved the same underlying banner diagnostic, and the selected regression passed. Only the second check was fault injection. Each reported three lifecycle-inclusive passes, zero failures, and zero skips; these are not full-suite or cross-platform results.

**Files/symbols:** `src/ssh/SshConnectionPool.cpp` — `authenticate()`, `authenticationFailure()`; `src/ssh/SshConnectionPool.h` — private authentication observation state and test access; `src/ssh/tests/tst_sshconnectionpool.cpp` — the regression above.

### TSH2 — SSH failure advice confused every non-key offer with plain password authentication

**Category/impact:** Incorrect authentication diagnostics. Users could receive advice for an authentication method the server had not offered.

**Context:** SSH distinguishes plain `password` from keyboard-interactive authentication, commonly used for PAM prompts. A server can also offer methods this client does not implement, such as host-based authentication. Not offering public-key authentication does not identify which of these alternatives is available.

**Before-case/root cause:** [INFERENCE, source-derived.] The old failure formatter used one public-key Boolean and treated its false value as a password-only offer. A host-based-only server therefore received password advice even though no supported client method could be attempted. A keyboard-interactive-only server was also described as plain-password-only, despite the authentication selector correctly distinguishing those methods.

**Fix:** The pool now retains the existing `AuthMethods` value type instead of a single public-key Boolean. It accumulates supported methods observed through the method queries already made during the attempt. The formatter distinguishes unsupported-only, keyboard-interactive-only, password-only, and combined password/keyboard-interactive offers. Unsupported-only advice explains the client/server incompatibility; keyboard-interactive advice names the actual method. These cases no longer gain inappropriate key-file or ssh-agent troubleshooting.

**Why this choice/limits:** Accumulation is diagnostic only: authentication still follows the server's current offer, not an earlier offer retained for explanation. No authentication method, credential input, callback shape, or fallback was added. Existing partial-factor explanations remain separate.

**Regression coverage:** `anUnsupportedOnlyServerIsNotDescribedAsPasswordOnly` and `keyboardInteractiveOnlyAdviceNamesTheActualMethod` in `tst_sshconnectionpool` feed real libssh method constants through `methodsFromMask()` and the production formatter. They assert the method distinctions and absence of false password-only or agent advice, with password-only and combined-method cases protecting the remaining branches. They intentionally do not pin an entire replacement paragraph.

**Files/symbols:** `src/ssh/SshConnectionPool.cpp` — `authenticate()`, `authenticationFailure()`; `src/ssh/SshConnectionPool.h` — authentication observation state; `src/ssh/tests/tst_sshconnectionpool.cpp` — both named regressions and updated test description.

### TSH3 — The newly added authentication regression initially used a libssh fixture that could hang

**Category/impact:** Regression-fixture correctness, introduced during this round. This is not an additional baseline product bug.

**Context:** TSH1 needed a real incomplete authentication exchange, without reading user keys, opening a remote SSH connection, or depending on a shared SSH server. The first version tried to obtain that state by calling blocking authentication on an unconnected `ssh_new()` session.

**Before-case/root cause:** Independent source review traced a hang through libssh's failed buffered flush, unchanged service/session termination state, and native polling on an invalid descriptor. The supplied authentication proof records Main's isolated before-case observation: the raw-session call never returned within three seconds and its process ended with timeout exit 124. A second review found another incorrect fixture assumption: an already-failed session can return AGAIN rather than ERROR. Main's native failed-banner observation returned AGAIN, not ERROR.

**Fix:** The raw-session fixture and helper were replaced. The regression supplies a private Unix socket through `SSH_OPTIONS_FD`, explicitly parses an empty configuration in an owned `QTemporaryDir`, and queues an invalid overlong banner plus write-side EOF. It first requires an actual connection error, then accepts the real incomplete ERROR-or-AGAIN result before exercising production authentication on that same failed session. The peer's read half stays open while the client sends its banner to avoid SIGPIPE; caller-owned descriptors close after pool teardown. Includes remain top-level and Unix-guarded. Using the existing `ssh_options_parse_config()` avoids introducing a newer libssh enum dependency. The companion production correction that rejects AGAIN belongs to TSH1, not a separate product finding.

**Why this choice/limits:** A finite connection timeout is a safety cap, not a substitute for constructing the right session state. The fixture does not force a particular native libssh return code, conceal AGAIN as ERROR, or introduce a production test hook. Non-Unix execution explicitly skips it.

**Regression coverage:** The existing `aFailedAuthenticationExchangeDoesNotInventServerMethods` name and meaningful error/no-prompt checks were retained and strengthened against invented agent/key advice. `local://round3-auth-proof.txt` records Main's corrected-fixture focused success in native-AGAIN mode and separately in externally injected-ERROR mode. The throwaway injector is not a committed helper or evidence that native libssh returned ERROR. Broader SSH suites and platform coverage are outside this focused proof.

**Files/symbols:** `src/ssh/tests/tst_sshconnectionpool.cpp` — corrected private transport fixture; `src/ssh/SshConnectionPool.cpp` — the associated incomplete-result guard described under TSH1. Evidence: `local://round3-auth-proof.txt`.

### TSH4 — A failed SSH transport could remain reported as Connected

**Category / impact:** Connection-state correctness and recovery. Direct channel consumers could stop receiving data after transport loss yet keep a cached Connected state, so their next operation reused a dead connection instead of reconnecting.

**Context:** A remote command ending its individual SSH channel is normal and does not end the shared SSH connection. A TCP/SSH transport EOF is different. Channel devices already surfaced fatal read errors and completed their read streams, but the pool’s state was a separate cached value. Higher-level bootstrap code often disconnected the pool itself, masking the gap for those callers; direct users and the live SSH fixture did not necessarily do that.

**Observed before-case:** An owned probe linked to the actual pre-TSH4 library connected only to Main’s private sshd with its explicit key and known-hosts file. It kept an owned raw channel as a native-state witness, then used the existing guarded positive-PID script to stop only its own per-connection SSH process. A real channel-error callback observed native `ssh_is_connected()==0`, and `readChannelFinished` arrived. The probe printed `readFinished=true nativeTransportClosed=true poolStillConnected=true`. This is a deterministic reproduction of stale state, not an explanation of the earlier unexpected EOF in a full parallel run.

**Root cause:** Channel read/write/start errors did not refresh the pool’s cached state. `SshConnectionPool::state()` returned that cache, while callers such as `ensureConnected()` treated Connected as permission to reuse the session. Later channel opens failed on the dead native connection. Updating only an individual device’s error/completion was insufficient to tell other users that the shared transport had ended.

**Final fix:** A private pool helper checks its current native session and changes Connected to Disconnected only when that native transport is actually closed. It does not free channels or discard buffered bytes merely to correct the state; ordinary explicit disconnect/reconnect still performs final cleanup. Normal command/channel EOF leaves a genuinely connected session unchanged. Read-pump completion observes state only after native accesses and payload/completion notifications finish. Write, channel-start, resize and native channel-allocation/open failures also observe the current connection after their native calls return.

**Callback safety:** The read/error paths capture a guarded pool pointer before emitting signals, so a consumer that closes or deletes the channel device cannot prevent the surviving pool from learning of transport loss. The helper checks the current session: a healthy replacement created by a callback is not marked disconnected because of an old error. Failed acquisition captures device generation/current-channel identity across state notification; if an observer reconnects and starts a replacement on the same device, the outer failed attempt does not overwrite or orphan that replacement. No new state notification was added to channel release or destructors, where a half-destroyed device could be visible. Read-pump reentrancy is cleared before the final state notification, and no device members are accessed after a callback may destroy it.

**Why this choice / limits:** Correct observed state without throwing away unread final bytes or treating every ordinary channel EOF as a whole-connection failure. Reuse the existing pool/session ownership and explicit reconnect path; no extra polling timer, retry policy, public RPC, protocol version or compatibility shim. A quiet unused connection cannot be declared dead before native I/O observes loss. The original intermittent transport EOF seen during a prior full parallel run remains unattributed; this finding fixes its demonstrated stale-state consequence, not a guessed cause.

**Regression coverage:** Four real-SSH cases verify normal channel EOF keeps Connected, synchronize a remote drop until retained bytes have reached the client, require one Disconnected transition, preserve those bytes, and execute a real command on the replacement connection. They cover retaining the device, reconnecting directly inside the state observer, closing from the channel-error observer and deleting from the completion observer. A fifth case lets a raw borrowed channel discover native EOF, then triggers failed device acquisition; its state observer reconnects and starts a replacement on that same device, whose actual replacement output must arrive instead of being overwritten by the failed outer request. Each case initializes its trust/connection setup independently, so selecting only these functions does not rely on an earlier test. Main ran all five behavior cases successfully: seven QtTest passes including init/cleanup, zero failures or skips. Both SSH source files also compiled with `CH_HAVE_LIBSSH=0`; that is compile-guard evidence, not a full no-SSH platform runtime claim.

**Files / symbols:** `src/ssh/SshConnectionPool.{h,cpp}` — private state refresh and failed open handling; `src/ssh/SshChannelDevice.cpp` — completion/start/write/resize observations and acquisition guards; `src/ssh/tests/tst_livessh.cpp` — fatal EOF, buffer, close/delete and reentrant replacement regressions; `docs/SPEC.md` §5.3 — channel EOF versus transport loss, readable final bytes and reconnect contract.

**Later diagnosis of the parallel-suite interruption:** TTC5 identifies and fixes a separate test that killed every connection under the shared private listener. Main reproduced that failure with the old terminal-test binary and verified the same concurrent pair after the scoped fix. This does not replace TSH4: its baseline stale-state defect was independently reproduced with one deliberately killed connection and remains covered by five real EOF/reentrant cases. The earlier generic editor-side connection failure is not separately assigned a cause without evidence.

### TSH5 — Pending live-test callbacks could outlive their captured result objects

**Category / impact:** Source-proved native test-fixture lifetime bug on timeout or early return, plus defensive ordering corrections for related observers. This is not a newly observed production crash and not a claim that the current channel-device destructor emits a finished signal.

**Context:** C++ destroys stack objects in reverse declaration order. A callback that captures local result variables by reference is safe only if those variables remain alive until the callback is disconnected or can no longer run. `CodeharbordClient` deliberately completes pending requests from its destructor, so a request that is still pending when a test returns can invoke its callback during teardown.

**Root cause:** In `rpcServerInfoOverSshChannel`, the `CodeharbordClient` was declared before the result value, optional RPC error, and answered flag captured by its request callback. If the wait timed out or an assertion returned while the request remained pending, those captured objects were destroyed first. The client's destructor then called `failAllPending`, dispatching the callback into dead stack state. Reading `CodeharbordClient`'s actual destructor confirms that path; no intentional undefined-behavior experiment was used to manufacture a crash.

**Fix and reason:** Declare captured result state before the client and device that use it. Their teardown can now finish or disconnect callbacks while the state still exists. The same file's sink, direct signal-capture, credential-request, and error-string locals are ordered before their corresponding device or pool. This needs no additional QObject, heap allocation, production API, alias, fallback, or weakened assertion.

**Every corrected group:** The six ordinary sink-based exec/PTY/stderr/large-output/session-teardown cases declare their sink before the device. The buffered-line case puts line and finished state first; the split UTF-8 stderr case puts its acknowledgement flag and sink first. The RPC case puts stderr text and all completion results before device and client. The encrypted-key, config-key, tilde-key, Windows agent-fallback, missing-agent, host-mismatch, PEM child, and password/multi-step branches put callback counters, request lists, or failure strings before the relevant pools. Temporary signal connections already explicitly disconnected after synchronous authentication remain unchanged. The five TSH4 tests already use a shorter-lived QObject receiver and remain unchanged. Main's added pool diagnostic text for the last real channel-open assertion is preserved so future failures carry the existing connection transcript rather than only a generic open error.

**Important evidence boundary:** Initial inspection considered whether `SshChannelDevice` destruction might emit into a sink declared later. The actual destructor stops polling, marks the stream finished, and releases the channel without emitting `readChannelFinished` or `aboutToClose`; it does not call the emitting `closeChannel()` method. That proposed destructor-emission mechanism was therefore rejected. Sink reordering is defensive lifetime-contract hardening, not proof of that rejected mechanism. The pending RPC destructor callback is the confirmed defect.

**Verification:** Main rebuilt and ran the real SSH target concurrently with the corrected terminal reconnect target; both passed. The complete final native run is recorded below. No permanent test intentionally dereferences destroyed objects, inspects declaration text, or copies the teardown implementation. Existing live behavioral assertions are preserved. The lifetime defect itself is established by declaration order and the actual pending-request destructor contract, not by an observed before-fix crash.

**Files:** `src/ssh/tests/tst_livessh.cpp`; callback-lifetime guidance in `docs/DEVELOPMENT.md`.

## Remote service bootstrap and update safety

### TSB1 — A refused nested connection request could still change the active connection's trust context

**Category/impact:** Reentrant connection-state correctness and trust-store selection.

**Context:** `SessionBootstrap` connects to a server and provisions its daemon. Its environment wrapper reads `CH_LIVE_*` configuration, including a known-host file and unattended-trust settings. Qt signals can invoke observers immediately, before the emitting function returns; nested provisioning event loops can also admit another request while the first is active.

**Before-case/root cause:** [INFERENCE, source-derived.] `connectAndWire()` already refused overlapping calls before replacing its remembered target. However, `connectAndWireFromEnvironment()` changed `m_knownHostsPath`, trust-scope flags, and environment-call bookkeeping before reaching that inner guard. An observer of `stateChanged(Connecting)` could therefore submit a nested request that returned false yet left the outer request using the nested request's store or scope bookkeeping.

**Fix:** After the existing disabled-environment fast path, the environment wrapper immediately refuses an in-flight request before reading or applying its configuration or changing scope state. The existing error behavior remains, but a refused request has no configuration effects.

**Why this choice/limits:** This places the guard at the first mutation boundary rather than trying to restore partially changed trust state later. A legitimate environment connection can still select its configured store. The change does not revoke host approvals or redesign trust prompts.

**Regression coverage:** `reentrantEnvironmentConnectCannotChangeTheActiveContext()` calls the real wrapper during Connecting with a different private known-host path. It checks one successful outer connection, a failed nested connection, and preservation of the original store and attended trust policy. Only the existing pool test hook is fake; configuration handling is production code.

**Files/symbols:** `src/app/SessionBootstrap.cpp` — `connectAndWireFromEnvironment()`; `src/app/tests/tst_sessionbootstrap.cpp` — the named regression.

### TSB2 — Provisioning could follow installation-journal or release-marker links outside its own state

**Category/impact:** Filesystem path safety during daemon installation and rollback.

**Context:** Remote provisioning keeps a `.codeharbor-provision` scratch/recovery directory and a `.codeharbor-release` marker beside the installed daemon. Recovery and rollback read these paths before replacing staged files. They must represent the installation's own directory and regular marker file, rather than links or special files.

**Before-case/root cause:** [INFERENCE, source-derived.] Lock-path links and FIFOs were already rejected, but these two paths lacked equivalent checks. A marker symlink passed the regular-file test; marker redirection or rollback copying could overwrite its external target. A scratch symlink containing a complete recovery journal could make recovery act on another directory's plan, including removal of live `dist`. A marker FIFO could block the script.

**Fix:** Once the persistent install lock is held, but before journal recovery, marker copying, or marker writing, the script rejects scratch symlinks and non-directory special files, and marker symlinks and nonregular files. Valid real directories and regular markers retain existing staging, rollback, and usable-service fallback behavior.

**Why this choice/limits:** Validation occurs before the first unsafe use, while keeping ordinary interrupted-install recovery intact. It closes pre-existing path-following mistakes; it does not claim to sandbox a concurrent local writer who can mutate the installation directory or forge contents inside its real journal.

**Regression coverage:** `theProvisionScriptStagesTheArchiveAndRollsBackUnderARealShell()` now includes marker/scratch symlinks and FIFOs. Protected marker targets and a deliberately complete external journal all live within the case's private temporary directory. Assertions require prompt termination and unchanged live daemon, protected marker, and outside journal.

**Files/symbols:** `src/app/SessionBootstrap.cpp` — `remoteProvisionScript()`; `src/app/SessionBootstrap.h` — documented path-type boundary; `src/app/tests/tst_sessionbootstrap.cpp` — extended real-shell regression.

### TSB3 — A remote symlink could bypass the refusal to install at the filesystem root

**Category/impact:** Installation-target safety.

**Context:** Installing the daemon creates lock, staging, and release files beneath a configured remote directory. The client rejects `/` and lexical dot/parent aliases, because the filesystem root is not an acceptable installation directory. The client's path cleanup cannot resolve links on the remote server.

**Before-case/root cause:** [INFERENCE, source-derived.] An absolute remote path pointing through a symlink to `/` survived `QDir::cleanPath()` validation. The generated script lacked a physical-path check, so an account with sufficient permission could create installation state relative to the remote filesystem root.

**Fix:** After `mkdir -p` establishes the chosen directory, the remote script resolves it using `cd` and `pwd -P`. It rejects physical `/` before creating even the installation lock. Resolution errors are fatal under the script's existing `set -e` behavior.

**Why this choice/limits:** Remote physical resolution addresses what client-side lexical normalization cannot know. Links to legitimate non-root installation directories remain supported; there is no blanket ban on directory symlinks.

**Regression coverage:** `provisioningRejectsASymlinkToTheFilesystemRoot()` creates a private symlink to `/`. A Bash DEBUG trap intercepts any `exec` before its redirection, so the unfixed path would terminate with exit 42 before writing a root-level lock. The fixed path must instead exit 1 with the root refusal. The fixture writes only its own symlink and skips unsupported Windows/no-Bash/no-flock environments. No execution result is claimed here.

**Files/symbols:** `src/app/SessionBootstrap.cpp` — `ensureRemoteService()`, `remoteProvisionScript()`; `src/app/SessionBootstrap.h` — target-safety documentation; `src/app/tests/tst_sessionbootstrap.cpp` — the named regression.

### TSB4 — Remote provisioning output could grow client memory without a bound

**Category/impact:** Resource consumption in the GUI process.

**Context:** `runRemoteScript()` waits up to three minutes for remote setup commands. Although the SSH device limits its own retained data, the bootstrap code drains that device into its own report and accumulates stderr diagnostics. A downstream accumulator needs its own limit.

**Before-case/root cause:** [INFERENCE, source-derived.] The stdout `QByteArray collected` and concatenated stderr `QString` grew for the entire command. A noisy login script, Node wrapper, or provisioning command could keep increasing GUI memory; a two-MiB stdout report was accepted in full. The channel's independent backpressure did not bound bytes already moved into bootstrap storage.

**Fix:** Collected stdout is limited to one MiB. Each read requests only the remaining allowance plus one byte to detect overflow. Overflow stops the nested wait, closes the throwaway command channel, and returns an explicit output-limit failure. Only the final 64 Ki UTF-16 characters of stderr diagnostics are retained, keeping the latest error available. Normal progress and diagnostic signals remain unchanged, and cancellation takes precedence over an output-limit result.

**Why this choice/limits:** The command's temporary channel is closed rather than sacrificing the shared connection. This bounds storage owned by `SessionBootstrap`, not arbitrary observers that independently retain every diagnostic signal.

**Regression coverage:** Live case `remoteScriptOutputIsBounded()` uses the canonical isolated SSH fixture and configured Node to emit two MiB on stdout or stderr without creating child files. It checks stdout rejection and bounded retained output, successful stderr completion with a bounded diagnostic tail, and a working real RPC connection after both commands. A test-local `ScriptBootstrap` exposes the existing protected method; no production interface was added. This case requires canonical isolation/preflight and has

**Files/symbols:** `src/app/SessionBootstrap.cpp` — `runRemoteScript()`; `src/app/SessionBootstrap.h` — documented limits; `src/app/tests/tst_livereconnect.cpp` — live regression and test-local subclass.

### TSB5 — A progress path could be mistaken for a successful daemon-install verdict

**Category/impact:** Incorrect upgrade result reporting.

**Context:** Reaching EOF on an SSH command does not itself prove a successful install. Bootstrap therefore looks for its provisioning script's dedicated installed-verdict line, and otherwise inspects whether an older usable service survived.

**Before-case/root cause:** [INFERENCE, source-derived.] The old unrestricted substring check accepted `codeharbor: installed` anywhere in output. For a staged archive path `/srv/codeharbor: installed/archive.tar.gz`, a normal fetching-progress line contained that substring. Even if copying the archive failed and the old service survived, bootstrap could announce Installed rather than reporting failure of the requested upgrade.

**Fix:** Verdict recognition requires the installed prefix, including its entry-path separator, at the beginning of an output line. A path embedded inside a fetching message cannot supply the verdict. Failed updates consequently reach the existing surviving-service inspection and `upgradeFailed()` fallback.

**Why this choice/limits:** This corrects classification of the script's own output without changing provisioning or removing the useful old-service fallback. It does not authenticate arbitrary remote stdout cryptographically.

**Regression coverage:** `failedInstallCannotBorrowItsVerdictFromAProgressLine()` supplies the concrete staged path, command EOF with a copy failure and only the fetching line, and an older surviving release. It checks continued connection usability, an upgrade-failure notification, and no Installed progress notification.

**Files/symbols:** `src/app/SessionBootstrap.cpp` — `ensureRemoteService()`; `src/app/tests/tst_sessionbootstrap.cpp` — the named regression.

### TSB6 — Upgrade documentation described Git detection that the implementation does not perform

**Category/impact:** Documentation-only API clarification; no runtime effect.

**Context / before-case:** [INFERENCE, source comparison.] The `requestRemoteUpgrade()` header comment said a Git checkout was refused, but the implementation does not inspect `.git`. Its actual distinction is whether the selected entry point comes from the source-checkout layout rather than the release `dist/` layout.

**Fix / why this choice:** The comment now says “source-checkout entry point,” documenting the real layout-based rule rather than implying repository detection. No Git inspection, upgrade-policy expansion, or behavior change was introduced. The existing usable-service fallback remains unchanged.

**Regression coverage:** No new runtime test is needed for this comment correction; it does not change executable behavior.

**Files/symbols:** `src/app/SessionBootstrap.h` — `requestRemoteUpgrade()` documentation.

## Application coordination and persisted settings

### TAC1 — Queued pane resizing could lose its final value when a close or split intervened

**Category/impact:** Layout intent ordering and persistence correctness.

**Context:** While a viewer layout is unavailable or loading, `SessionLayouts` queues user gestures for replay. Repeated resize reports can be compressed to their latest value, but splitting or closing a pane changes which children an index path identifies.

**Before-case/root cause:** [INFERENCE, source-derived.] Starting with a three-child branch, queue ratios `[2,1,1]`, close its first pane, and then queue `[3,1]`. `queueWrite()` searched backward across the close and replaced the earlier resize in place. Replay therefore tried a two-value resize against the original three-child branch, rejected it, then performed the close, leaving `[1,1]` rather than the intended `[3,1]`. A textual path had been treated as the same target despite a structural change.

**Fix:** Backward coalescing of continuous reports stops at Split or Close. Consecutive reports from the same uninterrupted gesture can still compress, but a newer report cannot move ahead of a topology-changing action. `RegionState` documentation records this ordering boundary.

**Why this choice/limits:** Retaining structural order preserves the actual user's sequence without changing the queue API or live pane identities. It does not disable useful compression within one continuous gesture.

**Regression coverage:** `queuedRatiosKeepTheirOrderAcrossStructuralGestures` checks the resulting published tree and final persisted wire tree after all three operations. Assertions examine behavior, not copied properties or source text.

**Files/symbols:** `src/app/SessionLayouts.cpp` — `queueWrite()`; `src/app/SessionLayouts.h` — `RegionState` documentation; `src/app/tests/tst_sessionlayouts.cpp` — the named regression.

**Regression maintenance:** The rapid-switch test now drains the actual write sequence and verifies current ownership, split-before-close ordering, the last persisted viewer/terminal trees and final published state. Its old first-intermediate-URL/exact-RPC-count expectation conflicted with the required structural write barrier; it was removed rather than changing the production queue to lose later user intent.

### TAC2 — An older layout reply could erase a split or close during its own notification

**Category/impact:** Reentrant layout editing and stale-response handling.

**Context:** Publishing a changed tree emits a Qt signal. A direct observer can run immediately inside that publication and deliver a pending RPC response before the editing function resumes. Local intent must therefore invalidate older reads before publication, not merely before a later save.

**Before-case/root cause:** [INFERENCE, source-derived.] A split or close published its tree before `persist()` marked an outstanding read superseded. An observer could deliver the earlier `getLayout` response during that signal, restoring the pre-edit tree. The outer method then persisted that restored tree. A terminal split could additionally create a server terminal row after its intended leaf had already disappeared.

**Fix:** Locally authored or structural intent marks older reads superseded before publication or synchronous terminal-row creation. The corrected paths include `saveTreeStamped()`, `splitPaneStamped()`, `closePaneStamped()`, `abandonTerminalMint()`, and successful binding of a newly created terminal row. Existing post-signal lifetime, generation, and readiness checks remain in place; the header documents the earlier invalidation point.

**Why this choice/limits:** The first externally reentrant boundary is the correct place to establish authority. Authoritative load publication does not invalidate its own read, and pane/document generation behavior is unchanged.

**Regression coverage:** `structuralPublicationRetiresOlderRepliesBeforeNotifying` covers viewer and terminal split/close data rows. It dispatches both pending reads from inside tree publication and checks that the actually saved tree still contains the local edit.

**Files/symbols:** `src/app/SessionLayouts.cpp` — the five paths above; `src/app/SessionLayouts.h` — invalidation contract; `src/app/tests/tst_sessionlayouts.cpp` — the named regression.

### TAC3 — Restoring a session during first-server notification could leave both layouts permanently unloaded

**Category/impact:** Connection readiness and initial session restoration.

**Context:** Accepting a server identity enables workspace loading, while layout I/O remains suspended until the new context is ready. A remembered session can be restored from an authoritative workspace list. Direct Qt callbacks can deliver that list sooner than the ordinary asynchronous ordering.

**Before-case/root cause:** [INFERENCE, source-derived.] `setServerId()` issued `workspace.list`. An `acceptedServerIdChanged` observer could deliver its reply before layout readiness opened. Restoration selected the remembered session, and `SessionLayouts::load()` recorded it, but correctly issued no I/O while suspended. `adoptServerIdentity()` resumed an existing selection only on same-server reconnects; first-server adoption therefore left both regions null until another explicit activation.

**Fix:** After enabling layout readiness, `adoptServerIdentity()` refreshes any active selection, whether or not the server ID changed. If ordinary asynchronous first connection has no active session yet, normal activation still performs the initial load without an extra request.

**Why this choice/limits:** This removes an ordering assumption instead of relaxing readiness or fabricating an active row. Terminal readiness still follows the layout-context handoff.

**Regression coverage:** `reentrantFirstWorkspaceRestorationResumesLayoutLoading` delivers the first authoritative workspace list during accepted-identity notification. It verifies restoration while I/O is suspended, then completion of both real `getLayout` requests and population of the selected session.

**Files/symbols:** `src/app/AppController.cpp` — `adoptServerIdentity()` and readiness handoff; `src/app/tests/tst_appcontroller.cpp` — the named regression.

### TAC4 — Controller tests could use native settings instead of private fixture files

**Category/impact:** Test isolation and protection of settings state.

**Context:** `AppController` owns persisted UI/application state. An empty settings path intentionally selects native `QSettings("CodeHarbor", "CodeHarbor")` behavior. `QStandardPaths` test mode does not provide a private per-case registry or CFPreferences store and is not a substitute for explicitly choosing an INI file.

**Before-case/root cause:** [INFERENCE, source-derived.] Most standalone controllers and the shared `ConnectFixture`/`ActiveSessionFixture` used empty paths. Sessions, pane counters, and preferences could therefore leak between cases or processes. Credential-persistence checks enumerated the temporary fixture directory while omitting the native store actually used. An independent review also identified a failure-path gap: an invalid temporary directory could yield an empty path and silently restore native behavior.

**Fix:** Every formerly default standalone controller gets a distinct INI beneath a process-private `QTemporaryDir`. Shared fixtures place controller UI/application settings beneath their own temporary directory. `checkedPrivatePath` checks directory validity and a nonempty absolute path before profiles or controllers are constructed. Existing explicit-directory controller and known-host fixtures likewise assert directory validity before constructing stores. Credential checks now include the controller's actual settings file.

**Why this choice/limits:** Isolation is established before construction and fails closed, rather than hoping an after-construction assertion prevents native access. Helper directories outlive their controllers. Production's native default is unchanged; test mode remains useful only for ancillary consumers.

**Regression coverage:** The existing `tst_appcontroller` cases and both shared fixtures now exercise their real private stores, including credential-persistence assertions against the correct files. This is fixture correction, not a new runtime product feature.

**Files/symbols:** `src/app/tests/tst_appcontroller.cpp` — standalone construction sites, `ConnectFixture`, `ActiveSessionFixture`, `checkedPrivatePath`, and credential-store assertions.

### TAC5 — Live-test callbacks could write into stack objects after their lifetime ended

**Category/impact:** Test-harness object lifetime and teardown safety.

**Context:** Live shell and cold-start tests issue asynchronous RPCs and listen to long-lived bootstrap/device signals. A wait timeout or function return does not necessarily cancel the request or disconnect a signal whose receiver context lives longer than the captured local variable. C++ also destroys members and locals in reverse declaration order.

**Before-case/root cause:** [INFERENCE, source-derived.] Both `RawRpc` helpers captured raw `this`; a timed-out wait could destroy the helper while a reply or transport teardown still held its callback. Live-shell initialization connected bootstrap errors to a local string using the longer-lived test object as context. Cold-start `AppGraph` diagnostic lists were declared after their senders and thus destroyed first; `runExec` had the same sender/captured-storage ordering problem. Live shell also closed its independent `FreshView` only on one successful cleanup path, and its temporary root did not have the required lifetime ordering for borrowers.

**Fix:** `RawRpc` is QObject-backed and callbacks use `QPointer`. Bootstrap diagnostic connections use a local QObject context matching the captured string's lifetime. `AppGraph` diagnostic storage and `runExec` capture storage are declared before their senders. `FreshView` always closes before bootstrap/pool cleanup, and the temporary live-shell root is declared before its borrowing object graph.

**Why this choice/limits:** Callbacks simply stop targeting destroyed helpers; live helpers retain existing results and timing. No retry, RPC-policy change, or cancellation of another consumer's requests is introduced.

**Regression coverage:** The affected consumers are the existing `tst_liveshell` and `tst_coldstart` live workflows. These corrections have no new standalone mock-only regression. Timeout/teardown execution, including ASan where available, belongs to integrated verification; no result is claimed here.

**Files/symbols:** `src/app/tests/tst_liveshell.cpp` — `RawRpc`, initialization diagnostics, `FreshView`, fixture member ordering; `src/app/tests/tst_coldstart.cpp` — `RawRpc`, `AppGraph`, `runExec`.

### TAC6 — Cold-start cleanup could delete recovery copies belonging to other work

**Category/impact:** Destructive test cleanup beyond fixture ownership.

**Context:** The daemon's recovery directory can contain unsaved editor snapshots from more than one window or Dev Session. A cold-start test owns its run-unique remote file and recorded test workspace/terminal identities, not the daemon's entire recovery collection.

**Before-case/root cause:** [INFERENCE, source-derived.] `cleanupTestCase()` recursively deleted `${XDG_DATA_HOME:-$HOME/.local/share}/codeharbor/recovery` after its editor operation. That broad cleanup could remove another window's unsaved recovery data. The test already saves through normal product behavior, so clearing the directory was neither needed nor justified by fixture ownership.

**Fix:** Remove the whole-directory deletion. Remote file cleanup targets only `m_remoteFile`, whose unique name the fixture created. Recorded workspace-row and tmux cleanup remain targeted by their recorded IDs; the product's ordinary save/recovery lifecycle remains responsible for snapshots.

**Why this choice/limits:** Removing an overbroad deletion is safer than inventing a replacement sweep or examining unrelated recovery files. This change does not introduce a new recovery retention policy or claim recovery contents were inspected.

**Regression coverage:** Existing cold-start save and targeted cleanup workflows remain the relevant consumers. No new destructive probe or passing test result is claimed here.

**Files/symbols:** `src/app/tests/tst_coldstart.cpp` — `cleanupTestCase()`, `m_remoteFile` cleanup.

### TAC7 — Live GUI tests needed explicit private settings and browser storage on every platform

**Category/impact:** Cross-platform fixture isolation, including persistent browser data.

**Context:** These tests create controllers, spawn settings-writing and real GUI processes, and construct persistent external WebEngine profiles. Linux XDG configuration variables do not redirect Windows registry or macOS native preference stores. Browser history/cache also have storage locations separate from application configuration.

**Before-case/root cause:** [INFERENCE, source-derived.] Live shell explicitly used native production settings on non-Linux platforms under a “disposable CI host” assumption. Passing only `XDG_CONFIG_HOME` to child processes did not isolate native stores. Named persistent external browser profiles could also reach production data/cache locations, including on Linux where only configuration had been redirected.

**Fix:** Live-shell controller/settings readers now share one explicit absolute INI beneath the fixture's temporary root; its independent reader gets another private INI. The width-writer validates and receives `CH_SETTINGS_PATH`, and the real GUI child receives the same setting. In `main.cpp`, an unset variable preserves native behavior; an explicitly empty or nonabsolute value reports a critical error and exits with failure before WebEngine, `QGuiApplication`, or settings/profile/UI-store construction. A valid path is passed unchanged to both `AppController` and `ServerProfiles`, keeping preferences, UI state, and profiles in the same chosen file.

Browser isolation is explicit too. Both owned live executables establish private Linux `XDG_DATA_HOME` and `XDG_CACHE_HOME` before WebEngine/GUI initialization and pass them to children. On every platform, owned `ViewerProfiles` construction supplies its required `externalStorageRoot`: live shell uses `m_settingsPath + ".webengine"` beneath `m_scratch`; cold start uses `g_configHome + "/webengine"` beneath its main-lifetime `configRoot`; the real GUI startup derives `settingsPath + ".webengine"` for an explicit INI override. The profile owner configures the root's `data` and `cache` children before first page use. Valid private directories and paths must be established before constructing the objects that could otherwise select native storage.

The construction order is part of the fix, not just eventual profile configuration. [INFERENCE, source-derived from Qt 6.9.] A named persistent `QQuickWebEngineProfile` initializes profile storage during construction, so setting private paths afterward is too late to establish that the native profile was never read. For a nonempty root, `ViewerProfiles` first creates an empty-storage-name, off-record profile, sets `root/data` and `root/cache`, sets the normal external storage name, and only then disables off-record mode before applying the existing cookie/cache policies or returning the profile to a page. `ViewerModel::setProfiles()` caches `externalStorageRoot()` so lazy replacement after destruction or explicit unset keeps the same private root instead of silently falling back to native storage. The required constructor has no compatibility overload or default root argument.

All affected constructor consumers were migrated, not only the two live application fixtures: pane-identity and QML-load tests, live-editor tests, terminal-page tests, direct viewer tests, live-viewer tests, and `ViewerModel` lazy recovery supply their respective private roots. Temporary-directory allocation guards run before profile construction and directories outlive their borrowers. `tst_openas.cpp` only registers the type and has no actual profile construction to migrate.

**Why this choice/limits:** Explicit constructor/startup inputs work across platforms without dropping cross-process acceptance coverage or silently falling back to native stores. An empty production browser root still means ordinary native/default storage; an unset `CH_SETTINGS_PATH` still means the normal application default. No additional environment variable or compatibility alias was introduced. The internal profile remains off-record, the external profile remains separate and persistent, arbitrary external HTTP(S) access remains supported, and no privileged bridge is added externally. Roots come from trusted configuration; this is not an ownership, symlink, keychain, or general sandbox guarantee. Linux execution alone cannot establish Windows/macOS isolation. The viewer owner's Qt 6.9 source review also identified a Windows-specific limit: the public off-record-to-persistent transition does not call the helper that registers blob-safe paths. Windows blob behavior remains unobserved, with no private-API or path-spelling workaround added.

**Regression coverage:** Existing live-shell/cold-start consumers now use these private paths, including width-writer and real GUI child startup. Startup acceptance must distinguish unset, explicit absolute, explicitly empty, and relative settings values; invalid supplied values must fail before stores exist. In `tst_viewers`, `profileIsolation` now checks real initialization creates the private `data` child while preserving internal off-record/external persistent separation. `viewerModelRecoversAfterInjectedProfilesDestroyed` retains both internal and external recovery coverage and verifies recovered external initialization uses the private data directory. `internalRequestFailuresReachQml` and live-viewer fixtures also construct explicitly privately rooted profiles.

**Files/symbols:** `src/app/tests/tst_liveshell.cpp` — private INIs, width-writer/GUI environment, browser root, early XDG setup; `src/app/tests/tst_coldstart.cpp` — main-lifetime config root, early XDG setup, browser root; `src/app/main.cpp` — `CH_SETTINGS_PATH` validation and controller/profile construction; `src/viewers/ViewerProfiles.h`, `src/viewers/ViewerProfiles.cpp` — required `externalStorageRoot` constructor input, accessor, and ordered persistent-profile initialization; `src/viewers/ViewerModel.cpp` — `setProfiles()` and root-preserving lazy recovery; `src/viewers/tests/tst_viewers.cpp`, `src/viewers/tests/tst_liveviewers.cpp` — profile isolation/recovery and private construction; `src/qml/tests/tst_paneidentity.cpp`, `src/qml/tests/tst_qmlload.cpp`, `src/editor/tests/tst_liveeditor.cpp`, `src/terminal/tests/tst_terminalpage.cpp` — migrated private-root consumers. These cross-owner helper/caller changes belong to TAC7, not an extra issue label. Supporting viewer-owner source reference: `local://round3-TVW.md`, “TAC7 shared follow-up,” citing [Qt 6.9 profile API](https://raw.githubusercontent.com/qt/qtwebengine/6.9/src/webenginequick/api/qquickwebengineprofile.cpp), [ProfileAdapter](https://raw.githubusercontent.com/qt/qtwebengine/6.9/src/core/profile_adapter.cpp), [ProfileQt](https://raw.githubusercontent.com/qt/qtwebengine/6.9/src/core/profile_qt.cpp), and [preference storage](https://raw.githubusercontent.com/qt/qtwebengine/6.9/src/core/pref_service_adapter.cpp).

### TAC8 — Refreshing a terminal layout could strand a pending pane and block future saves

**Category/impact:** Terminal-layout reconciliation, reentrant editing, and persistence progress.

**Context:** Splitting a terminal publishes a leaf before the server finishes creating the terminal row that will identify it. A refresh may retain the visible cached tree while starting new authoritative reads. It also advances a generation number so replies and deadlines from older terminal-row requests cannot alter the refreshed context.

**Before-case/root cause:** [INFERENCE, source-derived.] Split a terminal while its row creation is pending, then refresh the same session. Refresh retained the cache but retired the old request bookkeeping. A subsequent resize or close applied immediately to that cache and marked the authoritative read superseded. Persistence then refused the unbound leaf. The retired row callback/deadline could no longer settle it, and the authoritative replacement was now ignored. The leaf had no remaining reconciliation path, blocking later terminal writes.

**Fix:** `prepareWrite()` queues terminal gestures when a nonsuperseded authoritative load is still outstanding, the retained cache contains pending leaves, and no current terminal-row creation owns them. It adopts the authoritative tree first, then replays the current gestures using existing stale-target handling. Full authored snapshots retain their existing route and can reconcile their own new rows.

The independent-review follow-up is equally important: the queue condition explicitly requires the read to be **not superseded**. A new split made during refresh publishes its leaf before registering its current-generation row request. Synchronous resize/title observers of that publication must edit the new locally authored tree immediately, not misclassify it as old abandoned pending state. The first TAC8 patch missed this converse case; the `!superseded` condition corrects that introduced gap under the same issue.

**Why this choice/limits:** Only genuinely retired pending state waits for authority. Fully identified trees remain immediately editable, and new locally authored pending state retains its intended synchronous edits. No extra RPC, retry, or pane-generation reset is added.

**Regression coverage:** `refreshQueuesEditsUntilRetiredTerminalMintsAreReconciled` overlaps an old pending split, refresh, and ratio edit; it delivers retired creation and authoritative replies and checks the real two-pane tree plus new ratios is published and saved. `refreshKeepsReentrantEditsOfNewTerminalMints`, with ratio/title data rows, refreshes a bound tree, splits during the read, edits the new split synchronously during publication, and verifies the published/saved tree retains that edit after current creation and older reads settle.

**Files/symbols:** `src/app/SessionLayouts.cpp` — `prepareWrite()` and pending-leaf/load handling; `src/app/tests/tst_sessionlayouts.cpp` — both named regressions. Independent-review counterexample: `local://round3-review-Application.md`, A1.

## Settings, profile, model, and native chrome observers

### TMS1 — A preference-reset observer could delete the settings object before reset finished notifying

**Category/impact:** QObject lifetime safety; potential access after destruction.

**Context:** Qt direct signal connections run their observers synchronously. A preference-change observer can close its owning window and destroy the `AppSettings` sender before `emit` returns. Resetting several preferences therefore cannot assume the settings object survives each notification.

**Before-case/root cause:** [INFERENCE, source-derived.] After removing settings, `resetToDefaults()` emitted `themeChanged()` and then immediately called `groupPalette()` through `this`. If the observer deleted the object, its settings storage was already gone. Equivalent continuations existed after every later named-property signal, including the last viewer-default notification before generic change notifications.

**Fix:** A local `QPointer` detects destruction after each named-property notification and stops further continuation. All settings keys are still removed before the first notification, so teardown cannot leave a partially reset persistent file. The header documents that destruction stops subsequent notifications.

**Why this choice/limits:** The guard handles sender lifetime without changing normal notification ordering or inventing a reset transaction. Observers that deliberately modify preferences retain existing semantics.

**Regression coverage:** `TstAppSettings::resetObserverMayDestroySettings` iterates all seven named change signals, deletes the object from the selected signal, and reopens the isolated INI to check that reset persisted. Existing ordinary reset coverage remains.

**Files/symbols:** `src/app/AppSettings.cpp` — `AppSettings::resetToDefaults()`; `src/app/AppSettings.h` — notification lifetime contract; `src/app/tests/tst_appsettings.cpp` — the named regression.

### TMS2 — Profile-store callbacks could delete the store before a mutation resumed

**Category/impact:** QObject lifetime safety during profile persistence and notifications.

**Context:** Adding, updating, removing, or selecting a server profile persists settings and then emits change notifications. A failed-save observer may close the owner immediately; profile-list observers can do the same. Returning from a synchronous callback does not imply that the emitting store still exists.

**Before-case/root cause:** [INFERENCE, source-derived.] Each mutation called `persist()`, which could synchronously emit `saveFailed`, then continued to notify through a deleted store. Adding the first profile and removing the active profile also emitted `profilesChanged()` followed by `activeIdChanged()` without checking whether the first observer destroyed the sender. The persistence helper itself already released its lock before callbacks and avoided member access after terminal emission; the unsafe continuations were its callers.

**Fix:** `QPointer` guards stop the four mutation paths after destructive persistence callbacks and between paired notifications. `addProfile()` retains its newly created ID in a local value and can return that value without consulting destroyed storage. Header documentation explicitly allows observers to destroy as well as mutate the store.

**Why this choice/limits:** Persistence, merge locking, and last-write-wins behavior are preserved. This adds neither live cross-instance synchronization nor new Windows registry/ACL policy.

**Regression coverage:** `TstServerProfiles::aSaveFailureHandlerMayDestroyTheStore` covers all four mutations using separate private stores whose intended settings pathname is occupied by a directory. `aProfileObserverMayDestroyTheStore` covers addition/removal observer teardown and reopens private INIs to verify committed profiles and selection.

**Files/symbols:** `src/app/ServerProfiles.cpp` — add/update/remove/setActiveId mutation paths around `persist()` and notifications; `src/app/ServerProfiles.h` — observer contract; `src/app/tests/tst_serverprofiles.cpp` — both regressions.

### TMS3 — Session-model notifications could resume on a model that an observer had destroyed

**Category/impact:** Model lifetime safety during targeted updates and resets.

**Context:** The sidebar's `SessionsModel` updates rows as terminal state changes and resets its visible rows when groups or filters change. Qt delivers `dataChanged` and reset notifications synchronously, so their consumers can destroy the model immediately.

**Before-case/root cause:** [INFERENCE, source-derived.] During a two-session terminal-state refresh, an observer could delete the model on the first `dataChanged`. The next loop step read `resetSerial_` through freed `this`. That serial number detected reentrant resets, not destruction. Similar post-reset continuation in `setGroups()` inspected presence after an observer deleted the model, and filter setters could emit property signals after `applyFilters()` triggered destructive reset observers.

**Fix:** Guard reset boundaries and stop group/presence inspection after `endResetModel()` if the owner was destroyed. Guard filter-setter notifications after filter application. In the targeted update loop, test owner survival before reading its reset serial. Existing preparation of all changed rows before signaling and existing reset serial checks are retained.

**Why this choice/limits:** Destruction checks complement, rather than replace, reset-generation checks. The fix does not define a new policy for arbitrary recursive mutations inside Qt's `modelAboutToBeReset`; Qt's internal reset lifecycle remains its own contract.

**Regression coverage:** `TstModels::updateTerminalStateObserverMayDestroyTheModel` changes two rows, deletes the model on the first notification, and checks that only that first signal occurs. `resetObserverMayDestroyTheModel` covers group replacement and both filter setters with direct `modelReset` teardown.

**Files/symbols:** `src/models/SessionsModel.cpp` — `updateTerminalStates()`, `setGroups()`, filter setters and `applyFilters()` continuations; `src/models/tests/tst_models.cpp` — both regressions.

### TMS4 — Native maximize-button callbacks could destroy or replace participants during Windows dispatch

**Category/impact:** Windows native-event and QObject lifetime safety.

**Context:** `WindowChromeNative` connects Win32 maximize-button messages with a QML/native button's hover, down, and click state. Writing a QML property can synchronously run user handlers; those handlers may destroy the button/helper or register a replacement before the native event function resumes.

**Before-case/root cause:** [INFERENCE, source-derived; Windows behavior not runtime-observed here.] `setNativeButtonState()` wrote `nativeHovered` and then dereferenced a possibly cleared `m_button` to write `nativeDown`. During maximize release, a callback could delete the helper before `nativeEventFilter()` invoked clicked. `clear()` also delivered property callbacks before detaching its own registration, allowing recursive teardown against the same participants.

**Fix:** Capture guarded button/helper pointers across property writes and return whether those objects survived and still represent the current registration. Native and surface-event continuations stop when participants did not survive or were replaced. `clear()` detaches the registration before notifying properties. Registration keeps guarded candidate window/button pointers and does not overwrite a newer registration installed by a callback. The private `setNativeButtonState` return contract and all its callers were migrated together.

**Why this choice/limits:** Checking both lifetime and current registration avoids treating a surviving but replaced object as the original dispatch target. No public interface or non-Windows behavior was expanded. The Windows implementation was not compiled or executed by this reporting work; Linux execution cannot establish Win32 correctness.

**Regression coverage:** New `src/app/tests/tst_windowchrome.cpp` contains `registrationObserverMayDestroyButton`, `clearObserverMayDestroyButtonOrHelper`, and `releaseObserverMayDestroyHelper`. They use real `QQuickWindow`/`QQuickItem` instances and synthetic Win32 maximize messages. Non-Windows cases explicitly skip because the production bridge is a no-op there. The dedicated test's build integration belongs to Main; no Windows passing result is claimed here.

**Files/symbols:** `src/app/WindowChromeNative.cpp` — `setNativeButtonState()`, `clear()`, `registerMaximizeButton()`, native/surface-event continuations; `src/app/WindowChromeNative.h` — private helper signature; `src/app/tests/tst_windowchrome.cpp` — new native regression cases.

## tmux session discovery and deletion

### TAG1 — A tmux session name could resolve to an attached client and kill unrelated work

**Category/impact:** Destructive target selection and literal-name correctness.

**Context:** The daemon's `killSession({name})` RPC promises to destroy the session with that exact name, or do nothing when it is absent. tmux target syntax is broader than session names: it can also resolve clients and their attached sessions. Shell quoting does not remove that internal target interpretation.

**Before-case/root cause:** [INFERENCE, source-derived from baseline code and tmux 3.6a; not a runtime-observed 3.6a reproduction.] The old implementation passed `=name` directly to `tmux kill-session`. In upstream `cmd_find_get_session()`, tmux tries a session ID, then `session_find`, then `cmd_find_client` before checking `CMD_FIND_EXACT_SESSION`. Client lookup can match a client's name, full tty path, or tty path without `/dev/`. Thus an absent session name matching an attached client's name could select that client's unrelated session despite `=`. The old blacklist did not and could not establish an exact-session-name guarantee.

**Fix:** Each destructive request first obtains a fresh listing containing `#{session_id} #{session_name}`. It compares names literally, validates the selected server-issued ID as `$` followed by decimal digits, and kills only that ID. An absent literal name authorizes no destructive invocation; a selected session disappearing before the kill remains an idempotent missing-ID result, not a fallback name lookup.

Both ends of each listing record are framed with a fresh random marker. Embedded LF cannot make the first fragment of a longer name appear complete, and a forged continuation cannot supply the unpredictable prefix. CR, tabs, backslashes, and non-ASCII text are neither stripped nor normalized into another name. Unlike display parsing, destructive parsing does not accept CRLF rewriting: malformed or unresolvable output fails closed. The runner's existing eight-MiB output bound remains, and lookup/output failures never fall back to name targeting. The obsolete `requireKillName` blacklist and name/semicolon rewriting were removed. A session literally named `$0` is now selected by its literal listed name and destroyed by its actual listed ID, not automatically by ID zero.

**Why this choice/limits:** Only validated server-issued IDs reach destructive target grammar; RPC signatures and schemas are unchanged. A successful kill adds one bounded lookup process. Missing binary/server remain successful no-ops; other lookup or kill failures remain errors. The lookup and kill are not a transaction across an externally restarted tmux server: IDs are stable within one server lifetime, not across independent restarts. No unsafe fallback or compatibility shim remains.

**Regression coverage:** `remote/test/tmux.test.ts` includes `killSession destroys only the session with the literal requested name`; `an absent session name never reaches tmux's client-name fallback`; `killSession ignores forged and malformed ID records`; `control characters and name fragments cannot authorize another literal name`; and `a session disappearing between lookup and kill remains an idempotent no-op`. Assertions preserve a sentinel across hostile/glob/option/ID/semicolon/backslash/tty/non-ASCII/marker-like inputs, reject forged or fragmented records, and keep the kill pinned to the selected ID. Operational-failure tests distinguish lookup failures from post-lookup kill failures. `remote/test/tmux-live.test.ts` adds `a missing session named like an attached client cannot kill that client's session`, using an event-ready, bounded control-mode client on the fixture's private tmux server and `/dev/null` configuration.

**Files/symbols:** `remote/src/tmux.ts` — `killSession()` and framed destructive listing parser; `remote/test/tmux.test.ts`; `remote/test/tmux-live.test.ts`. Source evidence: [tmux 3.6a target resolution](https://raw.githubusercontent.com/tmux/tmux/3.6a/cmd-find.c) and [argument parsing](https://raw.githubusercontent.com/tmux/tmux/3.6a/cmd-parse.y). No `src/agent` production change was needed for this issue.

## Terminal creation, embedded pages, scaling, and live reconnect

### TTC1 — Terminal resolution preferences did not follow a move between screens

**Category / impact:** Terminal rendering correctness and avoidable rendering cost. A terminal could keep using an inappropriate backing-store resolution after a screen or browser-scale change, even though its preference was supposed to be capped by the actual screen resolution.

**Context:** The embedded terminal uses xterm in a browser page. Its device-pixel ratio controls how many physical pixels the renderer allocates for a logical area. CodeHarbor permits a terminal-specific preference, but that preference must still respect the browser's current native ratio. A window can move between screens with different ratios without any application setting changing.

**Before-case / root cause [INFERENCE]:** `createDevicePixelRatioController().set()` calculated a requested ratio and installed a browser-property getter that retained that calculation. The result was refreshed when `set()` ran again, not when the native screen ratio changed. `TerminalPaneView` reapplies settings at startup and on settings notifications; moving the window is not a settings notification. With a native ratio of 3 and a preference of 2, moving to a screen whose native ratio is 1.25 could leave the terminal reporting 2, above the intended physical-screen cap. Moving in the other direction could leave it unnecessarily capped at the former smaller value. Earlier tests changed their native accessor and then called `set()` again, so they did not describe the missing transition.

**Fix:** The controller retains the preference and observes browser resize events. It reapplies the preference only when the underlying native descriptor's actual value changes. The synthetic resize generated by the controller's own update sees the already-recorded native value, so it does not repeatedly trigger itself. `restore()` removes the observer before restoring the original browser property. The terminal-local `DevicePixelRatioWindow` interface now permits the event-listener methods needed for this observation.

**Why this choice / limits:** This makes the existing preference follow its original screen-cap rule rather than adding a second preference or changing xterm's rendering policy. The listener methods are optional because the data-only/non-browser seams cannot independently expose a changing native getter while the property is overridden; those seams retain their existing fallback behavior. Existing callers need no migration.

**Regression coverage:** The authored `native resize reapplies the preference for an own browser accessor` test and its inherited-accessor variant change native DPR in both directions through a deterministic `EventTarget` fixture without writing settings again. Their assertions also cover listener removal after restoration. These are regression descriptions, not passing-test or actual multi-screen GUI claims.

**Files / symbols:** `src/web/terminal/src/dpr.ts` — `createDevicePixelRatioController`, `DevicePixelRatioWindow`, `set`, `restore`; `src/web/terminal/test/dpr.test.ts`. `src/qml/TerminalPaneView.qml` supplies the existing settings-application context; no new screen-change setting mechanism was added there.

### TTC2 — A terminal page could finish connecting after that page had already gone away

**Category / impact:** Browser-page lifetime and stale asynchronous work. A late terminal connection callback could affect a replacement connection or alter a page that should already have been retired.

**Context:** Terminal startup begins an asynchronous Qt WebChannel handshake before mounting xterm. The bootstrap also keeps a guard identifying an element that is already connecting or connected. Both the browser page and that guard need a lifetime that begins when the handshake starts, not only once the renderer exists.

**Before-case / root cause [INFERENCE]:** `connectTerminal()` installed its `pagehide` teardown listener only inside the eventual WebChannel callback, after mounting. If `pagehide` occurred while the handshake was pending, there was nothing to retire the pending callback. A new connection could then start on the element, after which the old callback could still mount or mutate the DOM. Its missing-bridge failure path could also clear the guard now needed by the replacement connection.

**Fix:** The page-hide listener is installed before constructing `QWebChannel`. Teardown immediately retires a pending callback and releases that attempt's guard and listener. Successful startup hands later cleanup to a closure for the actual host. A callback belonging to a retired attempt returns before examining the channel, changing the DOM, or changing connection bookkeeping. Failed setup also releases the early listener.

**Why this choice / limits:** The connection attempt owns its cleanup from the start of asynchronous acquisition. A replacement attempt is therefore not dependent on whether the former handshake eventually succeeds or fails. This does not introduce automatic reconnection, retry a failed handshake, or replay terminal input.

**Regression coverage:** `page teardown cancels a pending handshake without retiring its replacement` drives the exported bootstrap in JSDOM with delayed channel callbacks. It requires teardown to allow a second handshake, the late first callback to leave the DOM and second guard alone, and the current callback still to handle its own failure. This coverage was authored; no browser execution result is claimed here.

**Files / symbols:** `src/web/terminal/src/index.ts` — `connectTerminal`, its connection guard, WebChannel callback and page-hide cleanup; `src/web/terminal/test/bootstrap.test.ts`.

### TTC3 — Terminal-row caching omitted the Dev Session that owned the row

**Category / impact:** Server-authoritative terminal identity and cross-session isolation. A cached or pending lookup could supply a terminal target from the wrong Dev Session without asking the server to validate the requested ownership.

**Context:** A terminal pane refers to a persisted terminal row inside a Dev Session. `TerminalFactory` resolves that row to the target that a controller will attach to. It caches completed resolutions and lets duplicate pending resolutions share a request. Those optimizations are safe only when their key identifies the same operation that the server was asked to authorize.

**Before-case / root cause [INFERENCE]:** `resolveParamsFor()` sent both Dev Session and row ID to the server, but `resolveTarget()` keyed row requests and `m_resolved` only by the row ID. Legacy resolution also stored a row alias under that bare ID. Resolving `row-A` in session `s1`, then resolving the same row ID in `s2`, could reuse the first answer. If the first request was still pending, the second controller could join it instead of making its own session-scoped request. If the first answer was cached, the second lookup bypassed the server ownership check altogether. A server-side reply-scope check alone cannot repair a path that makes no RPC.

**Fix:** Row and legacy keys now use separate `row:` and `legacy:` namespaces, and both include the owning Dev Session. The session component is length-framed: its length defines where it ends, instead of assuming that a separator never occurs in an opaque ID. Normal row lookups and row aliases learned through legacy backfill use the same scoped rule. The `paneKey` test hook reflects the framed legacy key, and header comments describe the contract. Tests check that different identities do not collide instead of requiring one incidental string spelling.

**Why this choice / limits:** This preserves same-context reuse and the existing public resolution signatures while bringing cache ownership up to the strength of the server request. It neither invents a local terminal identity nor substitutes a cache result for a different session's authorization. Length framing also avoids the collision between, for example, session/row parts containing slash-like separators. The separate WorkspaceDb reply-scope correction remains separately credited; this factory correction is independently necessary.

**Regression coverage:** `rowResolutionRetainsItsDevSessionScope` has `in-flight`, `cached`, `opaque-in-flight` and `opaque-cached` rows using real `WorkspaceDb`/`CodeharbordClient` instances over private local sockets. The assertions require a second session-scoped request, first success resolving only the first controller, and refusal for the second request leaving its target empty. The opaque rows distinguish `(s1/segment, row-A)` from `(s1, segment/row-A)`. `paneKeysAddressOneSlotOfOneDevSession` also covers separator- and prefix-like components without fixing the serialization format. No execution result is asserted.

**Files / symbols:** `src/terminal/TerminalFactory.cpp`, `src/terminal/TerminalFactory.h` — `resolveParamsFor`, `resolveTarget`, `paneKey`, `m_resolved` and pending resolution keys; `src/terminal/tests/tst_terminalfactory.cpp`.

**Integrated caller coverage:** Related strict response-ownership fixture corrections are recorded under TRP2. They preserve old-server cancellation isolation, reentrant new requests, same-row labels in different sessions and the endpoint-quoting regression; they are not new product findings.

### TTC4 — Two terminal browser tests acted before their required browser state was established

**Category / impact:** Regression-test reliability, not a new production behavior change. One test could succeed without exercising its intended routing case, and another could fail for rendering delay rather than clipboard behavior.

**Context:** Native tests send data into the embedded terminal, but xterm processes that data asynchronously. Sending a command or text to the page does not prove that the terminal has already entered the requested mouse mode or rendered the text that a later selection will copy.

**Before-case / root cause [INFERENCE]:** `rightClickSendsNothingToTheRemoteSide` slept for 500 ms after requesting mouse reporting, then checked that no mouse bytes reached the remote side. If the mode was not active yet, that absence did not test the intended right-click rule. `copyShortcutReachesTheSystemClipboard` slept for 300 ms after injecting a marker and selected/copied before proving that the marker was in xterm's buffer. A slow render could therefore look like a broken shortcut.

**Fix:** The mouse test uses the existing renderer diagnostics barrier to require `mouse=vt200` before the experiment. The clipboard test requires its marker in the xterm viewport before selection and copying. Browser errors remain visible. Post-action waits whose purpose is to measure the absence of output remain in place.

**Why this choice / limits:** The fixtures now establish the condition that makes each assertion meaningful rather than relying on a larger arbitrary sleep. No production timeout, retry, mouse-routing or clipboard policy changed.

**Regression coverage:** The corrected cases are the existing `rightClickSendsNothingToTheRemoteSide` and `copyShortcutReachesTheSystemClipboard` tests in the terminal-page fixture. Their new preconditions distinguish browser readiness from the behavior under test. Their execution belongs to Main's isolated terminal-page/browser verification; no run is claimed here.

**Files / symbols:** `src/terminal/tests/tst_terminalpage.cpp` — the two named behavior tests and their diagnostics/viewport barriers. The fixture's separate private `ViewerProfiles` root migration is credited to **TAC7**, not another TTC finding.

### TTC5 — A terminal reconnect test killed other tests' SSH connections

**Category / impact:** Live-test isolation bug. Parallel native runs failed even when the affected SSH behavior worked. The terminal test interrupted unrelated authenticated connections using the same private listener. Existing generated-fixture checks limited this action to the selected private fixture; this is not a claim that it killed an arbitrary production SSH service.

**Context:** Several live targets deliberately use the same disposable SSH server. A reconnect test must make its own connection fail while leaving that listener available for reconnection. It must also leave other tests' connections alone. Validating the listener's identity does not give one test ownership of every connection under it.

**Observed before:** A full 65-target run with two parallel jobs passed 64 targets but failed `tst_livessh` when its last test tried to open a channel. The output showed `tst_liveterminalfactory` running at the same time. Main then ran the still-old compiled `tst_livessh` and `tst_liveterminalfactory` together with the same `-j 2` setting and the same private listener. The SSH target failed during channel 20 of its 25-channel reuse check; the terminal target passed. By contrast, the SSH target paired with `tst_livereconnect`, which does not use the broad kill helper, passed. Earlier full-run SSH failures had appeared at different points, including an unexpected underlying TCP EOF. Their varying location was consistent with when the competing failure injection ran, rather than with the operation being tested.

**Root cause:** `TstLiveTerminalFactory::severTheNetwork()` first carefully validated the generated loopback listener, its private configuration, identity paths, namespaces, and PID. It then used local `pgrep -P` loops to kill every child and grandchild of that listener. Those descendants included other tests' connections. The validation protected the choice of server, but the signal scope was still too broad within that server. The old-binary overlap reproduction demonstrates this defect directly. The earlier generic editor-side authentication failure is not separately attributed to it without matching evidence; TED5 independently fixes that gate's ignored explicit key and missing failure detail.

**Fix and reason:** Keep all generated-fixture validation and existing configuration/command-line matcher gates. Replace descendant enumeration with an exec channel on this test's own authenticated pool. Following the existing reconnect-test pattern, the remote shell resolves its own parent, checks that it is an sshd process, and kills only that parent. The helper waits for this test's intended terminal controller to leave its live state. The listener remains alive for reconnect, and unrelated connections are not selected or signalled. No fixture path is interpolated into the remote command. The obsolete local kill script and unused `QProcess` include are removed. This fixes ownership rather than hiding the problem with locks, serialization, or retries.

**Regression and exercised verification:** The existing unclean-drop test now opens a second independently authenticated connection to the same host, port, user, and pinned host key. It executes a fresh command before the drop, a different fresh command after the intended terminal drops, and another after the intended factory reconnects. That second pool never reconnects, so the later commands prove that the original unrelated connection survived; checking a cached Connected flag alone would not. Existing checks still require the intended connection to fail, reattach to the same tmux session, preserve running work and scrollback, and avoid false notices. Main rebuilt both targets and reran the same previously failing concurrent pair: the setup fixture and both targets passed, 3/3, in 9.55 seconds. This includes the new unrelated-connection assertions. The complete final native run is recorded in the verification section.

**Files:** `src/terminal/tests/tst_liveterminalfactory.cpp`; live-test safety guidance in `docs/DEVELOPMENT.md`.

**Limits:** This live case exercises TerminalFactory reconnect through its pool. It does not newly claim to test SessionBootstrap's retry schedule or its notification RPC. No real user daemon, workspace, listener, or SSH agent was used as the disposable fixture.

## Desktop editor recovery and document ownership

### TED1 — Restored unsaved text was incorrectly treated as the editor's saved baseline

**Category / impact:** Unsaved-document and recovery correctness. Recovered text could later be declared clean even though it had never been written to the file, allowing recovery protection to be cleared and an external reload to replace the edits.

**Context:** The native editor controller knows the file's disk content and revision. Monaco, the browser editor, also needs a baseline so that editing and undo can report whether the buffer differs from disk. Recovery text is an unsaved buffer offered to the user after an earlier interruption; it is not a new disk baseline. The two kinds of content must enter Monaco through different paths.

**Before-case / root cause [INFERENCE]:** `EditorPaneView.restoreRecovered()` emitted `contentLoaded(recovered, loadedRevision)` and then reported the recovered content. The Monaco `contentLoaded` handler assigned that text to `baselineContent` and marked it clean. With disk text A and recovered text B, native content comparison initially knew B was dirty, but the browser now considered B its baseline. Editing B to C and undoing to B could produce `reportDirty(false, r1)`. Because the revision still matched, native code could accept the false clean observation, clear recovery and later accept a disk reread over B.

**Fix:** A host-only `EditorController::restoreRecovery(content, expectedRecoveryContext, expectedRevision)` method replaces the QML caller's synthetic load. It reuses the existing document-scoped `bufferRestored` signal to deliver both recovered RAM and the actual disk baseline/revision. It marks and snapshots the recovered buffer and retains it for page rehydration; it never presents that buffer as newly loaded disk content. Native acceptance also checks that the offer still belongs to the current recovery context and revision. It refuses an offer when there are newer local edits, a pending save, an active load, a read-only buffer, or a missing/not-ready page.

**Why this choice / limits:** Monaco keeps one truthful disk baseline, and recovery remains an explicit user choice. The additional native check prevents a QML offer from bypassing document ownership or a newer edit. This is a host API change, not an expansion of the browser WebChannel surface or a compatibility alias. A permission-only transition to read-only still retains existing RAM and recovery.

**Regression coverage:** `recoveryRestorePreservesTheDiskBaseline` covers the baseline distinction. `recoveryRestoreRejectsSupersededOffers_data` / `recoveryRestoreRejectsSupersededOffers` covers changed or interrupted scope, a changed disk baseline, newer edits, pending save, replacement-page readiness and active loading. The existing real-Monaco case `TstLiveEditor::repeatedSaveAfterAnEditAndRevertReconcilesDirtyState` was extended with recovery-style rehydration, an actual model edit and undo, and an assertion that recovered text remains dirty against revision `r20`. Earlier report observations are copied before later events instead of retaining a mutable array alias. These are authored coverage changes, not live-browser pass claims.

**Files / symbols:** `src/editor/EditorController.h`, `src/editor/EditorController.cpp` — `restoreRecovery`, `bufferRestored` and recovery acceptance; `src/qml/EditorPaneView.qml` — `restoreRecovered`; `src/editor/tests/tst_editorcontroller.cpp`; `src/editor/tests/tst_liveeditor.cpp`. The coordinated stub API migration is in `src/qml/tests/tst_paneidentity.cpp`.

### TED2 — A recovery question remained usable after its document ownership had changed

**Category / impact:** Stale user consent and recovery ownership. A visible Restore action could refer to a different controller or document context from the one that produced the offered snapshot.

**Context:** Recovery records are scoped by server, session and pane, and the controller exposes a recovery-context revision that changes when that ownership is invalidated. QML may hold an offer while reads and page startup complete. A visible question therefore needs to remember the controller, context and disk revision it is asking about; consulting whichever properties happen to be current when the user clicks is insufficient.

**Before-case / root cause [INFERENCE]:** `updateRecoveryScope()` advanced a reload serial and changed the controller's scope, but did not clear `recoveredContent`, `recoveryPending` or the open dialog. Those were primarily reset by file navigation through `start()`. A temporary blank identity, a changed complete context or controller replacement could leave an old offer attached to current UI. Restore used mutable current properties rather than the offer's original owner. Returning to the same textual identity after a gap did not make the former offer valid again.

**Fix:** The offer records its exact controller and recovery-context revision when the snapshot arrives, then records the baseline revision when the ready offer opens. A central clearing path closes and drops the offer on context change, controller replacement, navigation or a new content revision. Restore checks those captured values before calling TED1's native guarded acceptance method. An incomplete-context gap invalidates the old offer permanently; a new offer may subsequently be obtained for the current context.

**Why this choice / limits:** The question is permission to restore one particular snapshot into one particular document state, not permission to restore any later snapshot into any current editor. Invalidating the UI offer does not delete the old on-server record. Choosing Not now likewise continues to preserve that record. Native and QML checks complement each other rather than depending solely on dialog visibility.

**Regression coverage:** The shared `recoveryOffersRetireWithTheirContext_data` / `recoveryOffersRetireWithTheirContext` cases cover blank-and-restored server/session/pane scope and controller replacement. They assert that the former offer closes and cannot restore, while a fresh current offer remains usable. The shared `EditorControllerStub` now exposes dynamic recovery-context notification and the guarded host restore method. Its synthetic content delivery is only a QML observation hook; TED1's native and browser coverage establishes actual baseline behavior. Existing empty/nonempty restoration coverage remains. No tests are reported as passed here.

**Files / symbols:** `src/qml/EditorPaneView.qml` — `updateRecoveryScope`, offer capture/clear/restore handling; `src/editor/EditorController.h`, `src/editor/EditorController.cpp` — guarded `restoreRecovery`; `src/qml/tests/tst_paneidentity.cpp` — `EditorControllerStub` and recovery-offer regressions; related native rejection cases in `src/editor/tests/tst_editorcontroller.cpp`.

### TED3 — A successful save could leave an older owned recovery record behind

**Category / impact:** Recovery-state correctness after saving. A later reopen could offer crash text that the successful main-file save had already superseded.

**Context:** Recovery snapshots are written separately from the real file. A create-only snapshot write may discover an existing record, so the controller performs a bounded read to establish ownership and obtain the revision needed for a guarded update. If a real file save finishes during that chain, recovery clearing waits for the outstanding work to settle. A saved notification also causes the browser to report that its current buffer is clean, advancing the native edit-observation serial.

**Before-case / root cause [INFERENCE]:** The retry chain treated a changed edit serial as reason to stop before completing ownership discovery. If the clean acknowledgment arrived before the failed create was handled, ownership verification could be skipped. If it arrived during the ownership read, the callback could return before learning the existing record's revision and whether it contained owned content. Deferred clear then saw no known snapshot to clear. This was not a failure of the main file write; it was an abandoned cleanup obligation in the separate recovery chain.

**Fix:** When a clear is owed, the controller completes the bounded ownership check even if the text observation that began the snapshot write is now obsolete. It validates the complete recovery envelope before adopting the slot revision and content-presence information. It installs those fields only if a newer slot operation has not already supplied state. Once ownership is known, it abandons the superseded text and honors the deferred clear.

**Why this choice / limits:** Discovering whether an old owned record needs clearing is distinct from permission to rewrite obsolete text. The fix does not revive the old snapshot write. Foreign envelopes are neither adopted nor truncated, and an existing empty/tombstone record does not cause a redundant write. Revision-guarded clearing remains the protection against overwriting a changed record.

**Regression coverage:** `cleanSaveAcknowledgmentClearsAnOwnedSnapshotRetry_data` / `cleanSaveAcknowledgmentClearsAnOwnedSnapshotRetry` supplies six deterministic socket-level rows: acknowledgment before the create mismatch or during the ownership read, each with an owned record, foreign record or tombstone. Assertions require a guarded zero-length clear only for owned stale content, with neither an obsolete rewrite nor a foreign write. Existing overlapping-clear, retry-chain, superseded-report and foreign-envelope cases remain relevant. No execution outcome is claimed.

**Files / symbols:** `src/editor/EditorController.cpp` — recovery ownership-read/retry handling, edit-serial checks and deferred recovery clear; `src/editor/tests/tst_editorcontroller.cpp` — the named data-driven regression.

### TED4 — A replacement daemon could receive a watch ID issued by the former daemon

**Category / impact:** Remote resource ownership and reconnect correctness. Cleanup could cancel a subscription belonging to another pane on a replacement daemon if its opaque watch ID happened to match the former daemon's ID.

**Context:** An editor subscribes to file changes and later sends `file.unwatch` for the returned subscription ID. That ID belongs to the daemon process that issued it; it is not a globally meaningful file identifier. `CodeharbordClient::setTransport()` can replace the connection without first emitting a transport-closed event. Qt signal handlers can also run synchronously: a listener handling a transport-bound signal can complete another bind before later listeners process the outer signal.

**Before-case / root cause [INFERENCE]:** `EditorController::onTransportBound()` retired the pending-watch generation but retained `m_watchSubscriptionId`. Its resubscribe path then called `unwatchCurrent()` through the newly bound client. The old ID was thereby forwarded to a peer that had not issued it. Separately, an outer transport-bound notification could become obsolete while an earlier listener synchronously installed a final transport.

**Fix:** Current transport-bound handling forgets the former subscription ID before resubscribing. The editor also consumes the generation-bearing `transportBound(quint64)` contract and ignores an outer notification whose generation is no longer current after a nested bind. The signal-contract migration is the editor consumer portion of **TRP1**; TED4 specifically records the retained-watch-ID forwarding error.

**Why this choice / limits:** A new binding is not authority to clean up an old binding's opaque resource. Forgetting that ID avoids pretending it is owned on the replacement peer. The separate TUI6 detachment cleanup uses an explicitly captured old client and issuing generation; that narrowly owned resource release is not a reason to forward an ID through a new peer. No additional late-watch bug is claimed from an initial suspicion that did not establish a counterexample.

**Regression coverage:** `reentrantTransportBindKeepsOneCurrentWatch` installs a connection-manager listener before the editor and completes a final private-socket bind inside the earlier signal. It requires exactly one current watch/stat on the final peer and no old-ID unwatch. Controller teardown must unwatch exactly the final peer's actual returned subscription. The fixture uses process/counter-unique local sockets. This is authored coverage, without a test-pass claim.

**Files / symbols:** `src/editor/EditorController.h`, `src/editor/EditorController.cpp` — `onTransportBound`, `subscribeWatch`, `unwatchCurrent`, `m_watchSubscriptionId`; `src/editor/tests/tst_editorcontroller.cpp`. The underlying transport signal contract is credited to TRP1.

### TED5 — The live editor’s independent SSH connection ignored the configured identity and hid its error

**Category / impact:** Live-test correctness and failure diagnosis, not a claimed end-user editor failure. A supported explicit-key fixture could establish its main connection but fail to start the independent shell that makes external edits during reconnect testing.

**Context:** The live reconnect gate needs two TCP sessions: the connection under test, which it deliberately drops, and an independent shell that must survive that drop. Environment-driven bootstrap already honors `CH_LIVE_IDENTITY`. The independent pool instead called `connectToHost(host, port, user)` with no identity argument and replaced any actual failure with only `side pool could not connect`.

**Observed evidence / boundary:** A full native run reported precisely that generic setup failure. Its original cause was not established: Main subsequently verified the owned agent still held the correct fixture key, and an agent-enabled standalone connection succeeded. The report therefore does not attribute that particular occurrence to a missing agent or claim a proven parallel-connection defect.

Main separately established the deterministic source defect with the actual current SSH library, the owned local sshd/key/known-hosts and private HOME: with `SSH_AUTH_SOCK` absent only in the probe child, the old empty-identity call returned `connected=false` and reported no agent/no configured private key. The same connection with the explicit owned fixture key returned `connected=true`. This demonstrates the side connection’s unnecessary dependence on ambient agent state even when the caller supplied a usable identity file.

**Fix:** The independent shell now receives `CH_LIVE_IDENTITY` as its fourth connection argument, matching bootstrap’s existing environment-driven identity choice. An empty value retains the existing ordinary agent/config fallback. The fixture captures the pool’s actual `errorOccurred` message and uses its diagnostic transcript only if no error message was supplied, instead of erasing the reason. Channel errors continue using the existing independent-shell error state. No product authentication fallback, retry, secret prompt, transport timeout or acceptance rule was added.

**Why this choice / limits:** Both fixture connections should use the same explicit credentials. Requiring a second undocumented agent setup makes the reconnect result depend on unrelated launcher state rather than the editor behavior it is meant to exercise. Preserving the error makes any future handshake, trust or network failure distinguishable. This does not establish a root cause for the earlier transient generic failure.

**Verification:** The deterministic before-case is described above. The final verification record separately identifies the complete live reconnect run with the agent removed and the explicit fixture key retained, as well as the full native suite. The live gate still tests real remote edits before, during and after a killed connection, new watch subscriptions, dirty-buffer preservation and externally-modified state; those checks were not replaced with an authentication-only success.

**Files / symbols:** `src/editor/tests/tst_liveeditorreconnect.cpp` — `startSideShell`; `docs/DEVELOPMENT.md` — accurate `CH_LIVE_IDENTITY` contract for environment-driven connections and the independent shell. No new permanent source-copy or forwarding test was added.

## Desktop viewer navigation, directory requests, and profiles

### TVW1 — A directory answer was matched by pathname instead of by the request that asked for it

**Category / impact:** Asynchronous viewer correctness. An obsolete listing or error could settle a newer listing, and two panes requesting the same path could consume one another's replies.

**Context:** The shared `ViewerModel` serves directory views and address-bar probes. A pathname says what was requested, but not which request is being answered. Reloading a directory intentionally asks for a fresh snapshot even when the path is unchanged. Likewise, an earlier request or a request associated with another pane/peer does not become current merely because its path matches.

**Before-case / root cause [INFERENCE]:** `ViewerDirectoryView.requestedPath` and `ownsReply(path)` accepted any pending result for that path. Listing A for `/repo`, explicitly reloading to request B, then receiving A's old success or error could settle B. A second pane asking for `/repo` had the same ambiguity. `ViewerPane` address probes shared this model and needed the same ownership rule. Comments that called same-path requests interchangeable concealed the difference between an older filesystem snapshot/error and the answer to the current operation.

**Fix:** `ViewerModel::newDirectoryRequestId()` creates a host-local opaque token. `listDirectory(path, requestId)` now requires a token, with no default or compatibility overload. Both `directoryListed(path, entries, requestId)` and `directoryError(path, message, requestId)` carry it through every branch, including missing-client synchronous failure and malformed replies. `ViewerDirectoryView` stores `requestedRequestId` before dispatch, matches both path and token, and clears request ownership on reload, success, failure and timeout. An explicit same-path reload retires its predecessor first. `ViewerPane` does the equivalent for `probeRequestId`, including clearing it when abandoning or timing out a probe. Native, shared-QML, Open-as fake and live-viewer callers were migrated to the mandatory API. Contract comments now describe request correlation rather than path interchangeability.

**Why this choice / limits:** Allocation must precede dispatch because an offline/missing-client error may be emitted synchronously before the call returns. A caller must provide a unique nonempty token per dispatch; the model does not mistake UUID spelling validation for request ownership. Tokens are local correlation, not server authorization or a new RPC field. There is no RPC/schema version change, external-profile change or navigation-policy expansion.

**Regression coverage:** `supersededAddressProbesCannotNavigate` covers same-path earlier/peer-token answers that must not navigate, followed by a current answer that may. `theDirectoryListingTakesOnlyTheAnswerToItsOwnRequest` ignores older/peer same-path successes and failures while the current request remains pending, then allows the current answer to settle it. `viewerModelWithoutClientReportsErrors` covers availability of the token at the synchronous consumer; `directoryListingIsSortedDirectoriesFirst` supplies request identities; `tst_liveviewers::registryResolvesLiveRemoteUrls` filters both result signals by the captured token. The `tst_openas` fake uses the required API. These are authored or migrated regressions, not reported passes.

**Files / symbols:** `src/viewers/ViewerModel.h`, `src/viewers/ViewerModel.cpp` — `newDirectoryRequestId`, `listDirectory`, `directoryListed`, `directoryError`; `src/qml/ViewerDirectoryView.qml` — request capture and reply ownership; `src/qml/ViewerPane.qml` — directory/address probes; `src/viewers/tests/tst_viewers.cpp`; `src/viewers/tests/tst_liveviewers.cpp`; `src/qml/tests/tst_paneidentity.cpp`; `src/qml/tests/tst_openas.cpp`.

### TVW2 — Links to a section inside remote HTML were rejected as if they left the document

**Category / impact:** Viewer navigation correctness. An ordinary section link in a remote HTML file could silently do nothing.

**Context:** Remote HTML is displayed through a private opaque browser URL mapped to the remote source file. That internal URL is an implementation detail, not an address to publish in pane history. A link such as `#section` is resolved by Chromium to the current internal URL with a fragment appended; it should remain inside the existing document rather than open a different viewer location.

**Before-case / root cause [INFERENCE]:** `ViewerWebView.onNavigationRequested` allowed only the exact mapped source string. Adding a fragment broke that comparison, so the handler rejected native navigation and emitted the opaque URL through `navigated`. `ViewerPane.openWebNavigation` intentionally drops opaque implementation URLs. Those two policies combined to make the link a no-op.

**Fix:** `ViewerWebView.isInternalDocument(candidate)` compares the destination before its fragment with the nonempty mapped source URL. Native navigation is accepted for that same resource, including its section fragments. Other destinations retain their existing pane-level navigation handling, and the implementation URL is not published to pane history.

**Why this choice / limits:** The additional permission is only same-resource fragment navigation. It does not enable scripting, add a WebChannel, loosen CSP, grant local-file/network access or change external HTTP(S) profile policy. Remote HTML remains in its existing restricted native renderer.

**Regression coverage:** `remoteWebFragmentNavigationStaysInTheDocument` loads remote HTML through a real `ReadFileServer` and activates an anchor with a real pointer click; it does not enable remote HTML scripting to drive the test. Assertions expect the native URL fragment, no pane-level `navigated` signal and no additional source read. This is authored browser regression coverage, not a GUI/browser pass claim.

**Files / symbols:** `src/qml/ViewerWebView.qml` — `isInternalDocument`, `onNavigationRequested`; `src/qml/ViewerPane.qml` — existing `openWebNavigation` boundary; `src/qml/tests/tst_paneidentity.cpp` — the named remote HTML test.

### TVW3 — Internal URL cache comments understated how long resource-owned mappings can remain

**Category / impact:** Documentation accuracy; no runtime behavior change. **Context:** Internal remote-resource URLs can be pinned while a viewer or one of its resources still needs them. **Before-case / root cause [INFERENCE]:** Header comments called `maxEntries` a bound on all retained mappings and described pin count as bounded by pane count, whereas `evictIfNeeded()` excludes pinned entries and Markdown images can hold additional resource-owned pins. **Fix:** The comments now describe an unpinned-only cache bound and resource-owned pin lifetime, explicitly including Markdown images. **Why this choice / limits:** This corrects the explanation rather than changing a working lifetime rule; total map size can exceed the unpinned cache limit. **Regression coverage:** Existing `pinnedInternalUrlSurvivesEviction` and `pinsAreCountedAndReleasable` describe the implementation behavior; no new runtime test or execution claim accompanies this comment-only change. **Files / symbols:** `src/viewers/InternalUrlSchemeHandler.h` — `InternalUrlMap` ownership/cap comments; implementation context `InternalUrlMap::evictIfNeeded`; existing coverage in `src/viewers/tests/tst_viewers.cpp`. The shared `ViewerProfiles` storage-root repair and its cross-caller migrations belong to **TAC7**, not a fourth TVW finding.

## Desktop layout, dialogs, and retained unsaved work

### TUI1 — Agent commands could not reopen a viewer region after its last pane was closed

**Category / impact:** Agent-command and empty-layout correctness. A valid empty viewer region was mistaken for a nonexistent target.

**Context:** A session's viewer layout keeps an empty logical placeholder after its last real pane closes. The placeholder has an empty pane ID because it is not a persisted, addressable viewer. It is nevertheless a valid place to create a new real pane. An explicitly named pane that does not exist is a different case and must remain an error.

**Before-case / root cause [INFERENCE]:** `viewerCommandTarget()` returned the empty string both for the legitimate placeholder and for missing targets. `Main.runViewerCommand()` used string emptiness to reject unnamed `split`, `open` with `newPane:true`, and ordinary `open`, returning `unknown_pane` even though `SessionLayouts` already supported filling the placeholder through its normal creation path.

**Fix:** An explicitly missing named target is represented by `null`. Split/new-pane validation checks membership in the layout rather than requiring a nonempty string. Ordinary open into the placeholder first creates a persisted real viewer leaf through `SessionLayouts`, applies the requested target and returns the actual nonempty pane ID. Explicitly missing names still fail. Inventory continues to omit the unaddressable placeholder, and focus/reload/close do not pretend it is a real pane.

**Why this choice / limits:** The correction uses the existing server-authoritative layout path and preserves the difference between creating a pane in an empty region and operating on a named pane that is absent. It does not invent a special permanent placeholder identity or silently redirect an invalid explicit target.

**Regression coverage:** `tst_qmlload::agentCommandsFillAnEmptiedViewerRegion_data` / `agentCommandsFillAnEmptiedViewerRegion` covers ordinary open, open-new-pane and split through real `Main`, `AppController`, `SessionLayouts` and `ViewerCommandService` objects with an isolated in-memory transport. Assertions cover layout publication, a missing-name negative case, the persisted target URL, a real created ID and the resulting inventory. No execution result is claimed.

**Files / symbols:** `src/qml/Main.qml` — `viewerCommandTarget`, `runViewerCommand`; `src/qml/tests/tst_qmlload.cpp` — the data-driven agent-command regression.

**Integration correction:** The native regression now supplies canonical wire leaves with `type:"leaf"`, rather than a QML-only object missing the required discriminator. Its real AppController accepts a complete schema-10 `server.info` answer before workspace readiness is asserted. Empty-target creation, unknown named-target refusal, real created IDs, persisted URLs and resulting inventory remain the tested behavior; malformed fixture answers were not made valid by relaxing production decoding.

### TUI2 — QML fixtures did not own all of their settings and browser storage

**Category / impact:** Test isolation and safe fixture construction. Persistent settings or browser state could be shared between fixture instances instead of belonging to the instance that created them.

**Context:** A native/QML fixture builds an object graph containing settings, an application controller, browser profiles and an engine. Test-mode defaults alone do not make that state private to one fixture. A temporary root must be valid before these objects are constructed, and must outlive every object that uses it.

**Before-case / root cause [INFERENCE]:** `ShellFixture` constructed `AppController` without an explicit settings path, allowing settings/UI preferences to survive through shared test-mode defaults. The QML fixtures' `ViewerProfiles` instances also lacked owned external-profile roots before the TAC7 constructor cutover. Merely adding a temporary directory without checking allocation would still allow an empty root to select native/default storage.

**Fix:** `ShellFixture` owns a `QTemporaryDir` before the native graph, passes its `ui.ini` explicitly to `AppController`, and passes its `webengine` child to `ViewerProfiles`. `TstPaneIdentity` likewise declares its temporary directory before its profiles and passes that private root. Both use `checkedTemporaryRoot` to require `isValid()` and a nonempty path before constructing any dependent graph/store/profile member. Failure is fatal rather than a fallback to normal storage. Declaration order ensures the root survives engines and profiles.

**Why this choice / limits:** This closes the fixture ownership gap without relying on shared process defaults. The early validity check includes the independent-review correction: a failed private-root allocation must not become permission to open native stores. The actual `ViewerProfiles` implementation and mandatory constructor contract are credited to **TAC7**; this issue records the QML fixture-specific settings fix and caller migration, not a duplicate product-profile finding.

**Regression coverage:** Existing `tst_qmlload` and `tst_paneidentity` suites use the privately rooted graph. Required-constructor compilation and TAC7's profile/storage assertions provide the associated integration boundaries. No fixture suite, repeated-run result or observed profile-storage result is claimed here.

**Files / symbols:** `src/qml/tests/tst_qmlload.cpp` — `ShellFixture`, `checkedTemporaryRoot`, explicit settings/profile arguments; `src/qml/tests/tst_paneidentity.cpp` — temporary-root lifetime and required `ViewerProfiles` construction. Shared profile implementation: `src/viewers/ViewerProfiles.h`, `src/viewers/ViewerProfiles.cpp`, credited to TAC7.

### TUI3 — A newer handler at the same address inherited an old unsaved-close question

**Category / impact:** Stale user consent and viewer lifetime. Accepting a close question about one editor could close replacement content that the question never described.

**Context:** A viewer pane can switch handler without changing its URL, for example through Open-as. A dirty editor asks before closing. The permission represented by that dialog belongs to the particular content being shown when the question opened, not simply to the pane address forever.

**Before-case / root cause [INFERENCE]:** The existing close-dialog retirement ran on URL changes. If a close question was open, an already-pending save completed, and a newer accepted Open-as request replaced the handler at the same URL, `onUrlChanged` did not run. The old dialog could remain open and later close unrelated replacement content. Earlier coverage changed the URL and therefore did not cover this path.

**Fix:** Every accepted content replacement retires the older close confirmation in the shared `allowContentReplacement` gate, including same-address handler replacement. Dirty replacement remains refused. Direct host URL changes keep their existing URL-change retirement path.

**Why this choice / limits:** The dialog's lifetime now follows content replacement rather than one incidental notification that often accompanies it. This does not authorize replacing a dirty document or make a stale affirmative answer apply to a new handler.

**Regression coverage:** `navigationDismissesAnOlderEditorClosePrompt_data` / `navigationDismissesAnOlderEditorClosePrompt` now covers both changed-address replacement and same-address/new-handler replacement. Assertions require the old dialog to close without issuing a close and a later explicit clean close still to work. The cases also carry TUI5's narrow-window geometry checks. No run is claimed.

**Files / symbols:** `src/qml/ViewerPane.qml` — `allowContentReplacement`, `closeEditorDialog`, URL-change retirement; `src/qml/tests/tst_paneidentity.cpp` — `navigationDismissesAnOlderEditorClosePrompt` and its data rows.

### TUI4 — A layout update from the current workspace could destroy an unsaved editor

**Category / impact:** Live unsaved-RAM preservation under server-authoritative layout changes. A second client or authoritative refresh could replace or remove an editor without the local user's consent.

**Context:** The server owns the current viewer layout: which panes exist, their URLs and the split structure. The local UI owns the actual editor objects and their in-memory text. These are related, but not interchangeable. A new authoritative layout is valid authority for the layout; it is not user consent to discard unsaved text already visible in this client. Recovery debouncing is not an adequate substitute for retaining that live buffer.

**Before-case / root cause [INFERENCE]:** On a same-server/session refresh, `ViewerRegion.syncPane()` assigned an authoritative replacement URL even when the current editor was dirty. `sweepPanes()` destroyed a dirty pane whose leaf had been removed. Existing unchanged-tree reconnect coverage did not exercise either change. Simply refusing the new layout would also have been wrong if it meant publishing the old local tree back as though it were authoritative.

**Fix:** `node` remains the current authoritative tree. A separate `displayNode` temporarily keeps the last visible snapshot only while applying authority would replace or remove a dirty document. Existing editor objects, controllers and RAM remain visible and locally focusable. In this **same accepted server/session context**, the editor can still use its normal current-authority Save machinery; this is deliberately different from TUI6's permanently detached old workspace. An accessible, wrapping region warning explains the conflict and how to resolve it.

While display and authority differ, old split/URL/divider reports are suppressed, dividers are disabled, selected-pane persistence is paused, and Main's viewer layout/palette/agent mutation paths are blocked. The held tree is not written back to the server. Local focus remains available so the user can inspect and save the document.

When saving or otherwise becoming clean permits reconciliation, it runs on a deferred turn and checks dirty state again. It adopts the **current** `node`, not the first layout that caused the hold; a newer authoritative update supersedes an earlier candidate. An explicit Close-without-saving discards only the conflicting local document. It never closes a same-ID remote replacement. If other dirty conflicts remain, the local display removes or replaces just the resolved leaf and keeps the others accessible. Once no conflicts remain, it shows the latest authoritative tree.

Local URL reports intentionally do not republish layout. Ordinary local navigation followed by an edit therefore does not manufacture a false remote conflict. Reparenting a held render snapshot also cannot reapply its older URL over a live dirty editor.

The synchronous consent exception is deliberately narrow. Ordinary user-confirmed close and the documented direct agent close use a pane-object-specific marker scoped by `try/finally`. Only that exact pane's synchronous removal is exempt from the dirty hold. URL replacement is never exempt, and no permission survives for a later remote update. This preserves the normal last-pane-close/empty-placeholder workflow rather than retaining a document that the user has just explicitly discarded.

**Why this choice / limits:** Presentation can lag authority to preserve RAM without lying about authority. A saved or explicitly discarded same-context document can then yield to the latest server tree. This is not a local layout fork, a new draft system or a fake active session. Explicit workspace Discard still resets the documents it validly asked about. Unsolicited whole-context retirement is handled separately by TUI6: those editors may remain visible but cannot save to any server.

**Regression coverage:** `remoteLayoutChangesKeepDirtyDocumentsAccessible_data` / `remoteLayoutChangesKeepDirtyDocumentsAccessible` crosses replacement/removal with save/discard and asserts visible retained editors, newest-authority adoption, paused navigation/layout reports, no old-URL report and no backend close of a same-ID replacement. `closingOneRetainedDocumentKeepsOtherDirtyDocumentsVisible` covers two dirty documents, local navigation without tree republishing, one consented discard that preserves the other, a newer authoritative tree and eventual adoption. `consentedDirtyCloseDoesNotBecomeARemoteLayoutHold` covers the explicitly consented last-pane close. Existing reconnect, full-tree rebuild, dirty-navigation, identity and splitter cases remain integration boundaries. These tests were authored or extended; no GUI retention or test-pass result is asserted.

**Files / symbols:** `src/qml/ViewerRegion.qml` — `node`, `displayNode`, `syncPane`, `sweepPanes`, local reporting and deferred reconciliation; `src/qml/ViewerPane.qml` — content replacement and close gate; `src/qml/Main.qml` — layout/palette/agent mutation and selected-pane guards; `src/qml/tests/tst_paneidentity.cpp`. Canonical documentation was updated in `README.md` and `docs/SPEC.md` (including the viewer/workspace behavior described around sections 4.5, 8.1, 8.2 and 8.7); the viewer-skill documentation is credited in the documentation owner's section.

**Integration correction:** Runtime checks caught a vertical sizing cycle in the new retained-layout warning: the rectangle measured its Label’s natural height while the Label used `anchors.fill` on that same rectangle. The Label now has only left/right/top anchors and keeps its natural height. Tests require a positive bounded warning and positive retained editor height, including after real 360/960 window resizes, and reject QML warnings. This corrects an intermediate round implementation, not a second baseline finding.

### TUI5 — The unsaved-close confirmation did not fit narrow windows reliably

**Category / impact:** Confirmation usability. The baseline confirmation could extend outside the real window. Integration also caught a height-dependency cycle introduced while adding wrapping; that intermediate defect was corrected under this same finding, not counted as another baseline bug.

**Context:** A confirmation that discards unsaved text must keep its explanation and buttons reachable. A narrow split pane is not the same thing as a narrow window: this application centers modal dialogs on the global window overlay. The shared `AppDialog.preferredWidth` convention already clamps a requested width to that overlay.

**Before-case / root cause:** [INFERENCE, baseline source.] The viewer confirmation gave its content a fixed 380-pixel width plus dialog padding, so a 360-pixel window could not contain it. Main's later native integration runs observed actual `implicitHeight` binding-loop warnings at widths 120/220 and in the close/retained-layout scenarios after the initial wrapping change. Merely putting the Label in a wrapper did not fix the complete cycle; the final runtime results, not that intermediate source review, are the verification record below.

**Final fix:** The viewer confirmation requests `preferredWidth:420`, measures `contentWidth` from a shared overlay-clamped requested width minus horizontal padding, and sets `contentHeight` to its wrapping text's natural height. The measuring Label is inside a wrapper so the popup's later assignment of actual content geometry does not become the Label's own measurement input. Neither measurement reads the popup's assigned width/height while those values can still depend on implicit size.

The shared `AppDialog` computes implicit height using its stored natural `contentHeight` instead of Basic Dialog's `implicitContentHeight` getter. Qt 6.10's safe-area bookkeeping in that getter can read the same control's implicit height while it is being calculated. The replacement preserves Basic's background/inset, top/bottom padding, conditional header/footer height and spacing terms; it does not set a fixed height, disable wrapping, hide warnings or drop the footer. A single `_clampedPreferredWidth` supplies both the existing conditional width Binding and the viewer's independent content measurement. Default preferred-width-zero sizing remains available to other callers.

**Why this choice / limits:** Reuse the existing global-overlay convention rather than invent pane-relative modal placement. A test that only shrinks a pane inside a wider window cannot demand that the globally centered dialog fit that pane. The corrected tests resize the actual QQuickWindow. The shared sizing correction affects other desktop confirmations too, so the complete native suite is required rather than only the two close tests. Close-without-saving and stale-consent rules are unchanged.

**Regression coverage:** Both `navigationDismissesAnOlderEditorClosePrompt` rows resize the actual window to 360 and require the dialog/content to fit, retain usable positive dimensions and avoid vertical text clipping. The 120/220/960 responsive-viewer rows and all four remote-layout save/discard rows retain their interaction/geometry checks and reject QML engine warnings. Retained panes are also checked at real 360/960 window sizes. No warning allowlist, assertion weakening or test skip was used.

**Files / symbols:** `src/qml/AppDialog.qml` — `_clampedPreferredWidth`, conditional width Binding and natural implicit-height formula; `src/qml/ViewerPane.qml` — `closeEditorDialog`; `src/qml/tests/tst_paneidentity.cpp` — narrow-window/close/retention regressions. Source references: [Qt Basic Dialog](https://raw.githubusercontent.com/qt/qtdeclarative/v6.10.2/src/quickcontrols/basic/Dialog.qml), [QQuickControl](https://raw.githubusercontent.com/qt/qtdeclarative/6.10/src/quicktemplates/qquickcontrol.cpp), and [QQuickPane](https://raw.githubusercontent.com/qt/qtdeclarative/6.10/src/quicktemplates/qquickpane.cpp).

### TUI6 — Retiring an entire workspace destroyed unsaved editors and left stale consent able to affect replacements

**Category / impact:** Unsaved-RAM lifetime, permanent server-capability retirement and user-consent ownership. This is one underlying TUI6 correction spanning desktop QML and the native editor, not an additional TED5 finding.

**Context:** Archiving or deleting the active session, selecting a genuinely different server/session, or retiring a document generation can remove the entire viewer context. The application may meanwhile keep one shared `CodeharbordClient` and bind it to a different peer. Retaining the old browser editor is necessary to preserve the user's visible text, but retaining its old ability to read, save, watch or write recovery through that shared client would be unsafe. A retained editor therefore has two separate lifetimes: its local page and RAM can remain useful, while its authority to use any server must end permanently.

Qt's notifications make ordering important. Signal listeners normally execute synchronously: a setter emitting a signal can immediately enter QML handlers before the setter's caller continues. Clearing authority after publishing a state change would leave those handlers a chance to act with an old capability. Layout and active-session notifications also do not necessarily arrive in the order a UI might assume.

**Before-case / root cause [INFERENCE]:** `ViewerRegion.resetPanesForNewTree()` formerly destroyed the entire cache unconditionally. User confirmation for a deliberate workspace exit did not cover an unsolicited authoritative archive/delete or generation retirement. The actual production sequence adds a critical window: `SessionLayouts` publishes a null viewer tree **before** `AppController.activeSessionChanged`. A fix waiting only for the session ID notification would therefore be too late. TUI4's authoritative-node handling first holds dirty panes when that empty/fallback tree arrives; TUI6 completes the later permanent detachment and local-object retention.

Keeping objects alone would not have been enough. Pending file/recovery callbacks, queued saves, future transport notifications and mutable QML scopes could otherwise reconnect the retained document to a replacement peer. Likewise, a workspace Discard question captured before retirement could remain open while the selected document objects or accepted peer changed. A later affirmative click would then describe neither the current objects nor their current authority.

**Fix — retain the visible documents without pretending they are current authority:** The application keeps its current authoritative tree, current App server/session IDs, accepted peer and readiness truthful. It does not preserve fake active IDs, write the old tree back, add wire fields or introduce a draft/history subsystem. On context retirement, the root keeps only the last visible dirty document objects and prunes clean old panes. Every old native editor is detached synchronously before QML scope/page mutation or deferred destruction, including a clean controller being pruned while another dirty editor is retained.

`ViewerPane.retireServerContext()` invokes the native-first `EditorPaneView.detachFromServer()` wrapper and then freezes the actual handler kind, URL, controller and original recovery identity. Retired objects cannot be relabelled as the new owner's pane, even when the new layout reuses the same pane ID. Repository probes, navigation, host reload, page reload, factory/controller replacement, bridge replacement and scope updates cannot retarget the old document. A later local undo that makes the dirty flag false does **not** silently release that permanently retired page.

The existing Monaco DOM, channel and page adapter remain visible and usable for local focus, copying and editing. Detachment is not implemented by destroying the page or forcing read-only mode. The region warning, pane subtitle and editor banner explain that this is a previous workspace, retained only in local memory, and cannot be saved to any server; they do not imply that reconnecting will restore its authority. This is explicitly different from TUI4, where the same-context document may still save through its current accepted server.

**Fix — permanently sever native document authority before notifying observers:** Host-only `EditorController::detachFromServer()` is idempotent. It first marks authority retired and invalidates load, save, watch, permission and recovery generations and pending operation bookkeeping. A generation number lets a callback establish that it still belongs to the operation that created it; invalidating it makes a late completion unable to resume the retired operation. The reset also removes queued-save and other pending work. The method disconnects transport/notification listeners and clears its borrowed client without destroying that shared client or the page adapter. Only after the capability is gone does it publish state and recovery-context notifications. Recovery-context reset batches its notification until that point, so synchronous observers cannot use the old client while reacting to retirement.

Path, disk baseline/revision, dirty state, the existing read-only verdict, retained RAM and existing on-server recovery data survive. Recovery is advertised unavailable. Scope and recovery-directory setters cannot relabel the retired document. Open, reload and local-report paths cannot initiate server I/O. An actual later native save emits an explicit retired-workspace failure through the still-connected adapter instead of silently waiting or writing through whichever peer the application now uses.

The invalidation covers all pending continuation families, not just the next Save button: open/reload reads; save completion, queued save and conflict-stat paths; permission and reconciliation stats; watch subscription results; recovery existence probes, ranged reads, final stats, ownership reads, retries, writes and clears. Later bound/closed/watch events cannot initialize the document again because the relevant client connections are removed. Local observations and edits cannot recreate that authority.

**Fix — narrowly owned watch release is the only allowed remote cleanup:** Before retirement, established-watch cleanup captures the exact subscription ID, the old `QPointer` client and the transport generation that issued it. After authority has been severed, it may send exactly one `file.unwatch` only while that captured old binding is still current and writable. A successful reply to a watch request that was pending at retirement retains the equivalent captured-client/issuing-generation cleanup capability. It can therefore release a known owned late subscription on the still-live old binding rather than deliberately leaking it. Neither active nor late cleanup reacquires the controller's member client, forwards an ID to a replacement/closed binding, retries as a new document operation, or authorizes any read, write, recovery operation or new watch. Resource release is not renewed editor authority.

**Fix — keep current-owner controls and catalog truthful:** Old cache entries are never created again or assigned current-owner identity while the retired display remains. Layout writes and current-owner viewer mutations stay blocked. Agent list reads use only the current authoritative catalog, and report no current focused pane while the local retired view is shown. A same-ID old editor is not advertised as the replacement owner's document. Selected-pane writeback and programmatic current-owner focus restoration remain blocked; local focus inside the retained page is still allowed.

Local Close always asks about retired text, including after undo has made it appear clean. Acceptance removes only that local document, with no close RPC or URL report for a same-ID remote replacement. With multiple retained editors, closing the first never creates a new-owner pane next to the remaining old editor. The last local close adopts the latest authority and re-arms current-owner focus restoration.

Independent review found that the palette/keyboard close route originally checked for a nonempty active session too early. After archive/delete, header close could work while the palette route failed. `Main.closeActivePane()` now routes a context-retired viewer through `requestPaneClose()` and the retained display target **before** the empty-session gate, then returns unconditionally. It cannot fall through and close the new owner's layout. Terminal and current-authority/MCP guards are unchanged.

**Fix — bind every workspace Discard question to the actual peer and document objects:** Each request gets a fresh modal carrying its own action, selected server/session, layout generation and complete pane/handler/controller/URL/context identity set. Acceptance first requires the currently owned dialog instance and an unchanged captured document set. A newly mounted apparently clean document is also outside an older question: its first dirty notification might still be in flight. A replacement handler, an additional unasked document, an obsolete dialog callback or changed context cannot destroy current RAM or run the former action.

The final consent correction separately captures current `App.acceptedServerId`, not merely a document's frozen original server tag. Captured and current accepted identities must both actually be strings; missing values are not coerced into proof of identity. Captured and current per-document `contextRetired` values must likewise actually be booleans and remain equal. An A-to-B accepted-peer change, A-to-empty change or false-to-true retirement invalidates the older question even if selected server/session and layout generation are unchanged. An explicit, unchanged empty accepted identity is nevertheless a valid offline/local-only state: a fresh question about the same already-retired objects can still discard local RAM and close offline. A normal blank-acceptance reconnect gap alone does not permanently detach a same-context editor.

Valid consent immediately discards the documents it asked about before beginning the asynchronous workspace action. It does not store a standing permission to destroy future edits. If that later action fails, the current view may consequently be recreated clean; consent already authorized discarding the captured RAM. The default remains Cancel, and valid workspace Discard does not require a second document-by-document prompt.

**Why this choice / limits:** Preserving the browser and revoking the server capability solves different halves of the same lifetime problem. It leaves old text accessible without mixing old/new peer identities or allowing stale asynchronous work to acquire new authority. Current catalog/readiness can remain accurate while the visible old local document is deliberately not a current pane. Permanent detachment has no reconnect escape hatch; users must copy needed text and explicitly close/discard the old view. No new save destination, draft store, compatibility alias or broader editor feature is claimed. The previously documented Markdown Save/View Source problem belongs to baseline **DQ2**, not a new finding in this work.

**Regression coverage:** The following native/QML cases were authored or extended; they are coverage descriptions, not passing-test or actual Chromium text/copy claims:

- `retiredWorkspaceKeepsDetachedEditorsUntilLocalDiscard_data` / `retiredWorkspaceKeepsDetachedEditorsUntilLocalDiscard` covers session, server and document-generation retirement plus accepted-peer-notifies-first ordering. It checks visible pane/handler/controller survival and original tags, no probe/reload/scope relabelling/kind switch, confirmation after clean undo, and same-ID current-owner adoption only after local close without a close or URL report.
- `retiredWorkspaceCloseDoesNotMixNewOwnerWithOtherRetainedEditors` starts with two dirty old documents and a clean pane. It checks clean pruning, old-editor detachment, one close preserving the other without creating a new-owner replacement, and adoption after the last close.
- `tst_qmlload::authoritativeArchiveRetainsEditorsBeforeSessionNotification` uses the real `Main`/`AppController`/`SessionLayouts`/`EditorFactory`/`EditorController` graph and a private in-process transport. Its authoritative archive metadata response drives the production tree-clear-before-session-notification order. The retention-across-new-owner row checks native/item survival, stale-dialog rejection, current same-ID catalog and focus truth, blocked open/focus/reload/close/split, unchanged new-owner selected-pane state, explicit detached save failure with no RPC, and local close without authoritative layout change. The `palette-close-without-active-session` row invokes the actual `pane.close.viewer` command immediately after archive, requires local confirmation and old-object destruction after acceptance, and checks empty active selection with no viewer delete/write RPC or layout mutation.
- `detachedDocumentRetainsRAMAcrossLateRepliesAndPeerReplacement_data` / `detachedDocumentRetainsRAMAcrossLateRepliesAndPeerReplacement` covers pending save versus explicit reload, simultaneous permission/recovery work, exactly one established-watch cleanup, retained RAM/baseline delivery, late-reply rejection, unchanged path/revision/dirty/read-only state, idempotence, replacement-peer reconnect with no editor I/O, immutable scope setters, local edits and explicit save refusal through the live adapter.
- `detachedDocumentRejectsLateWatchAndRecoveryContinuations_data` / `detachedDocumentRejectsLateWatchAndRecoveryContinuations` requires exactly one owned cleanup for a late watch success on the same old binding, no operation on a new peer when late bytes belong to the former socket, and no permission/recovery ownership-read continuation that reinitializes the document or starts a read/retry. It does not encode an intentional owned-watch leak.
- `workspaceExitRequiresExplicitDiscard` covers replacement-handler identity, an additional unasked dirty object, changed context, a former dialog's late Discard after a new one opens, safe default Cancel and the existing session/server/settings exit routes. `windowCloseKeepsDirtyEditorsAlive` uses per-request modal ownership. `reconnectRefreshPreservesDirtyEditorAndRetiresOldIntents` now explicitly crosses a valid user-consented discard boundary before its intentional reset branch instead of treating generation change as consent.
- `workspaceDiscardRejectsAcceptedPeerAndRetirementChanges_data` / `workspaceDiscardRejectsAcceptedPeerAndRetirementChanges` keeps selected IDs/generation constant while controlling accepted-identity publication. Rows cover replacement acceptance, cleared acceptance with retirement, a normal cleared-acceptance reconnect gap and retirement alone. Old questions must neither destroy objects nor invoke actions. A fresh close/quit question about already-retired objects with unchanged empty acceptance must still be able to discard locally and close offline.

Actual archive/delete GUI rendering, DOM text retention, copying and current-authority adoption are separate Main-owned verification requirements; these source and regression descriptions do not claim those observations.

**Files / symbols:** `src/qml/ViewerRegion.qml` — `retainRetiredContext`, `resetPanesForNewTree`, `takePane`, `syncPane`, `closeRetainedPane`, `advanceLayoutGeneration`, `focusPane`, retained cache/display and `contextRetired`; `src/qml/ViewerPane.qml` — `retireServerContext`, frozen `kind`, document identity, repository probe/reload/navigation/close guards; `src/qml/Main.qml` — `workspaceDocumentsSnapshot`, `workspaceDocumentsMatch`, fresh workspace-dialog ownership, `closeActivePane`, `requestPaneClose`, viewer catalog/mutation and focus-restoration guards; `src/editor/EditorController.h`, `src/editor/EditorController.cpp` — `detachFromServer` and all retired continuation/cleanup paths; `src/qml/EditorPaneView.qml` — native-first `detachFromServer`, `serverContextRetired`, retained page and banner; `src/editor/tests/tst_editorcontroller.cpp`; `src/qml/tests/tst_paneidentity.cpp`; `src/qml/tests/tst_qmlload.cpp`. Documentation updates cover `README.md`, `docs/SPEC.md` and the documentation owner's viewer-skill changes; native editor contributions and review follow-ups are all credited under this single TUI6 issue.

**Native regression setup and evidence boundary:** The archive scenarios now cross real schema-10 server acceptance, publish complete native wire records, and complete successful UTF-8 read/watch/writable-stat replies before editing. They assert a clean editable baseline, exact nonempty revision and rendered geometry; after retirement they require exactly one cleanup for the original watch, allow further local edits and require detached Save to emit an error with zero RPC. The four captured-consent scenarios also establish real initial acceptance and a successfully loaded native document, but their later accepted-identity publication is deliberately a controlled Main-consumer input simulation at unchanged generation—not proof of a real later bootstrap transition. The separate archive scenarios provide the production retirement-order proof. A real transport is removed before the fresh already-retired offline local-close check. Standalone fixtures that replace an already-started controller explicitly start their new test controller; production does not reopen a document just because a test replaces that seam.

## Mobile connection, key storage, and native theme

### TMC1 — Remembered server profiles lost explicitly saved device keys

**Category / impact.** Credential-reference persistence. A remembered server could lose its selected identity even though the user had explicitly saved that key on the device. This is a reference/ownership correction, not a change to SSH authentication or private-key validation.

**Context.** A mobile user can import a key for the current session, explicitly save an app-owned copy on the device, or register a reference to an externally managed key. A server profile remembers an `identityFile` reference so that reopening the profile can select the key again. The reference is a path, not private-key bytes or a passphrase. These three kinds of ownership matter: removing an external reference must not delete somebody else's file, while deleting an app-owned saved key must not leave a second reference masquerading as another saved copy.

**Before-case / root cause.** [INFERENCE] After importing a key, choosing to save it on the device, and saving a server from `ConnectPage`, reopening that server selected no key. `ConnectPage.formProfile()` obtained the reference from `MobileKeyStore::referenceFor()`, but that method consulted only external-reference bookkeeping. Explicitly saved keys were recorded in `m_savedNames` and `m_memoryKeys`, so the method returned an empty string despite the saved file still existing. Existing assertions expecting that empty value protected the wrong integration contract.

**Fix.** `referenceFor()` now returns the exact app-private saved-key path for an explicitly saved key, the existing external reference for an externally managed key, and nothing durable for a session-only import. `keyInfo.reference` follows the same contract. After the existing readability/platform checks, `registerReference()` recognizes the exact path of an already-known app-owned saved entry and returns its existing name instead of creating an external alias. An external file remains external even when the requested display name collides with a saved key. Missing files are refused during re-adoption. Header and form documentation now describe both durable sources; obsolete assertions were corrected without adding compatibility aliases or another key copy.

**Why this choice / limits.** The profile now records the reference that the existing ownership model already knows how to reopen. It does not change deletion ownership or invent a new storage format. Key material still passes the normal `keyMaterial()`/libssh checks when used. These paths belong to the current device/container: there is no new cross-device, relocated-container, or path-migration guarantee. Profiles still contain neither private bytes nor passphrases.

**Regression coverage.** `savedKeyReferenceRoundTripsWithoutAnAlias` imports and saves a disposable key, writes and reopens a private `ServerProfiles` INI, reopens the key store, re-adopts the recorded path, installs actual material into `SshConnectionPool`, and checks that deleting the saved copy leaves no alias. `savedKeyReferenceRefusesAMissingFile` removes the owned test file and requires refusal with an explanation and no alias. `registerReferenceDoesNotShadowASavedKey` checks that an external collision stays separate and unregistering it preserves both files. Existing leakage checks distinguish a permitted path from forbidden private bytes. These are authored coverage, not a claim of a passing run.

**Files / symbols.** `src/mobile/MobileKeyStore.cpp` and `.h`: `referenceFor()`, `registerReference()`, `keyInfo()`, `keyMaterial()`; `src/mobile/qml/ConnectPage.qml`: `formProfile()`; `src/mobile/tests/tst_mobilekeystore.cpp`.

### TMC2 — Reopening a terminal with a new grid changed the screen but not its controller

**Category / impact.** Terminal geometry consistency. The local terminal screen and the controller responsible for its remote pseudo-terminal could disagree about the number of columns and rows.

**Context.** `MobileTerminalSession` connects a selected pane to a `TerminalController` and a local `VtScreen`. The grid dimensions tell both the local renderer and the remote program how much space is available. Reopening the same pane is not necessarily a new attachment: an existing attachment can remain live while its view acquires new geometry.

**Before-case / root cause.** [INFERENCE] Calling `open()` again for an attached pane with a different positive grid changed the session's stored dimensions and local screen, then returned through the already-attached guard before resizing the controller. A later `resize()` with those dimensions also returned early because session bookkeeping already matched. The remote program could therefore continue drawing for the old grid. A second weakness was that repeated-size suppression compared only session bookkeeping, not a newly replaced controller's actual geometry.

**Fix.** `open()` now uses the existing shared `resize()` path. That path suppresses a repeated size only when both session bookkeeping and controller geometry agree. Local screen resizing still happens before the pseudo-terminal resize. Existing hard size limits remain, and invalid or not-yet-reported dimensions remain ignored. Retargeting continues to replace the controller and discard the previous pane's buffers and terminal modes rather than carrying state across identities. The public contract was updated accordingly.

**Why this choice / limits.** Reusing the established resize path removes two competing ways to update geometry. It also handles a fresh controller whose grid differs even when the session's requested dimensions have not changed. Remote propagation remains the existing `TerminalController::resize` / `applyPtySize` behavior; this change does not introduce a new SSH mechanism.

**Regression coverage.** `reopeningAnIdentityKeepsTheControllerAndScreenGeometryInSync` uses a real session/controller with an owned `FakeChannel`, reopens one identity at a different size, then retargets at the same size and checks screen/controller agreement. `resizeNeverOutgrowsTheGridTheScreenCanHold` and `retargetingDropsThePreviousPanesBuffersAndModes` retain the size-limit and identity boundaries. This is not an observed live SSH window-change result.

**Files / symbols.** `src/mobile/MobileTerminalSession.cpp` and `.h`: `open()`, `resize()`; `src/mobile/tests/tst_mobileterminal.cpp`.

### TMC3 — Key-storage text promised stronger secrecy and erasure than the implementation provides

**Category / impact.** Security communication and unnecessary secret-buffer allocation. The correction makes retention, encryption, and deletion statements truthful without changing the user's opt-in storage choice.

**Context.** Imported keys can remain loaded in memory, explicitly saved keys are written to app-private files, and the SSH pool can retain an installed identity for reconnect. Qt's `QString` and `QByteArray` can share storage, and parsers, QML, and backend code can hold additional copies. Overwriting one writable buffer is not proof that every copy disappeared, and removing a file is not secure flash erasure.

**Before-case / root cause.** [INFERENCE] The header claimed saved key bytes were never held between uses even though `saveKeyOnDevice()` retained imported bytes and connection setup installed a retained pool identity. Other comments implied reliable removal from freed memory. `KeyImportSheet` described every saved file as unencrypted even though already-encrypted input armour is preserved, and its deletion language implied unrecoverable erasure. The save implementation also copied a retained `QByteArray` merely to detach and overwrite that extra copy while the actual retained original stayed live.

**Fix.** The header now explicitly describes loaded-import and pool retention, and limits clearing claims to best-effort overwrites of writable buffers. Implementation comments acknowledge parser/runtime copies. The import sheet explains that CodeHarbor adds no encryption, preserves existing input encryption, and does not save the key passphrase; an unencrypted saved key remains readable to someone who can read app files. Deletion now says CodeHarbor cannot undo it rather than promising secure erasure. QML clearing comments describe removal from undo history, not erasure from memory. The synchronous `QSaveFile` write now borrows the retained const buffer instead of allocating and wiping a pointless additional secret copy.

**Why this choice / limits.** Honest wording preserves the existing feature while avoiding a security promise ordinary Qt containers and filesystem deletion cannot support. File persistence, permissions, atomic commit, backup handling, and explicit opt-in are unchanged. This does not add secure-memory storage, erase guarantees, encryption, or any profile-secret persistence.

**Regression coverage.** No test was added merely to pin incidental warning prose. Shared shell tests no longer require the inaccurate claims that all saved keys are unencrypted or that phone unlocking alone determines readability. They retain the separate unchecked save action, a nonempty `PlainText` warning, and real storage/deletion checks. Existing save/reload/permissions/leakage coverage applies to the unchanged persistence behavior; no execution result is asserted here.

**Files / symbols.** `src/mobile/MobileKeyStore.h` and `.cpp`: retention documentation, `saveKeyOnDevice()`, connection-identity comments; `src/mobile/qml/KeyImportSheet.qml`; `src/mobile/tests/tst_mobileshell.cpp` for coordinated warning-contract updates.

### TMC4 — Retrying a new connection created another remembered profile

**Category / impact.** Profile identity and synchronous-callback safety. A retry from the same form must edit the entry just created, not create a second server profile or attach an old attempt's ID to a different form.

**Context.** The connect form submits metadata and a selected key to `MobileAppController`. Saving the profile precedes starting the connection. Authentication can fail while the form remains visible, and a session-only key selection or pending passphrase interaction must remain intact. Qt signals can invoke connected callbacks immediately, before the original call returns; those callbacks can also retarget the form.

**Before-case / root cause.** [INFERENCE] A new form submitted no profile ID. `connectToServer()` created and stored an ID but returned `void`, so the form did not learn it. Following authentication failure, retrying without leaving the form still submitted no ID and created another profile. The normal activation path that adopts an active profile had not happened.

**Fix.** `connectToServer()` returns the stored `QString` ID, or an empty value when writing the profile is refused. It returns the stored ID even if the later connection cannot start or fails. `ConnectPage` adopts a nonempty result immediately and emits its `connected` signal only for a stored profile. It does not reload the profile: that would unnecessarily clear a session-only key selection and disturb the passphrase flow. The production QML caller and shared C++/QML mock were migrated to the return contract; direct C++ callers can ignore the result without a compatibility shim.

The form snapshots the submitted metadata and selected key before entering the controller. After synchronous callbacks return, it adopts the ID only if the current profile ID, selected key, and every submitted metadata field still match that submission. A callback that has already retargeted or edited the form therefore prevents adoption of the obsolete attempt's result.

**Why this choice / limits.** Returning the ID created by the existing persistence operation ties retries to the correct entry without reconstructing form state. The post-call checks are necessary because a successful profile write can synchronously notify other code; successful persistence alone does not prove that the original form is still current. This does not change authentication policy or persist private material in profiles.

**Regression coverage.** `retryingAConnectionFromTheSameFormDoesNotDuplicateItsProfile` uses the compiled form, real controller, and a private INI without bootstrap/SSH pool, performs two connection actions with an intervening login edit, and requires one ID/profile with the edit visible to a fresh reader. `aRefusedProfileEditIsReportedRatherThanDiallingTheOldOne` also checks returned success/refusal IDs. `aReentrantFormRetargetDoesNotAdoptThePreviousConnectionsProfile` retargets the form inline from real `ServerProfiles::activeIdChanged` and checks that the first attempt's ID is not adopted afterward.

**Files / symbols.** `src/mobile/MobileAppController.cpp` and `.h`: `connectToServer()`; `src/mobile/qml/ConnectPage.qml`; `src/mobile/tests/tst_mobilenav.cpp`; `src/mobile/tests/tst_mobileshell.cpp`: `ConnectionAttemptState.connectToServer` mock.

### TMC5 — Native mobile controls must inherit the app's live theme preference

**Category / impact:** Inconsistent styling and missing preference propagation. Custom mobile panels followed the app's dark/light setting, but native Material controls did not inherit that setting. Flat controls and unfilled text inputs also had a potential text-contrast problem when placed on custom dark surfaces.

**Context:** The C++ mobile application selects Qt's standard Material control style. `MobileTheme` supplies colors for custom panels from the existing settings preference. Qt's native Material controls separately use the inherited `Material.theme` attached property. Selecting the style alone does not connect that property to the app's preference.

**Observed before, with an important distinction:** Main's actual default-backend editor capture showed a light raised Reload button with black text inside the custom dark toolbar. That enabled button's text was readable; the confirmed defect was the mixed light-control/dark-panel appearance and the missing live theme binding. Native connection controls also appeared light within the dark frame. An earlier software-backend capture made the raised Reload background disappear and made that button look black-on-dark. That software rendering result is not evidence that the default-backend enabled raised button was unreadable. Disabled Save text alone is likewise not treated as an enabled-control accessibility failure. The contrast risk for flat and unfilled controls is independently covered by the real consumer regression below.

**Root cause:** `MobileMain.qml` did not bind the root window's `Material.theme` to `MobileTheme.themeName`. The app had two disconnected theme choices: its settings-driven custom palette and Material's default light theme.

**Fix and reason:** Import the standard public `QtQuick.Controls.Material` module and bind the root ApplicationWindow's `Material.theme` directly to the existing singleton's dark/light name. Child native controls inherit the preference and subsequent changes. There is no second stored theme state and no per-control text-color patch. Existing style/plugin setup is reused; no build-system change is needed.

**Coverage and exercised verification:** `nativeControlsRemainReadableAcrossLiveThemeChanges` hosts actual Material Button and TextField controls under the real `MobileMain` window. The fixture does not copy the production theme or foreground binding. Its flat toolbar button and unfilled text input expose native text over a settings-driven custom panel. Across dark → light → dark changes, real window grabs must contain at least 20 glyph pixels per control at contrast of at least 4.5:1; foreground luminance must change in the appropriate direction, and the panel must return to its original appearance. Main built and ran this regression successfully. Main also opened the actual native editor using the default rendering backend after the fix: Reload now had light text on a dark raised button within the dark toolbar, and editor content remained readable. That actual run confirms inherited dark styling; the permanent rendered test supplies live preference-transition evidence.

**Files:** `src/mobile/qml/MobileMain.qml`; `src/mobile/tests/tst_mobileshell.cpp`; the integration description in `docs/DEVELOPMENT.md`. Ownership was explicitly reassigned for this late correction so the earlier mobile Back-navigation work in the same QML file was preserved.

**Limits:** No physical mobile device run is claimed. Exact default Material colors and English UI labels are not the regression contract; readable text and live inheritance are.

## Native mobile viewers and resource lifetime

### TMV1 — An outgoing viewer could release the replacement viewer's resources

**Category / impact.** Viewer resource ownership and delayed object destruction. A replacement image/PDF/Markdown page could lose its current request or cached file because an older page released the same path too late.

**Context.** QML `Loader` replaces viewer objects when the selected pane changes. Removing a child from the Loader does not necessarily destroy its Qt object immediately: final deletion can be deferred. Images and PDF spools are managed by remote path, and the service already uses request generation numbers to reject obsolete replies. Those reply checks do not say which page currently owns a later release-by-path operation.

**Before-case / root cause.** [INFERENCE] The host unloaded an old page and created a replacement requesting the same image or PDF before the old object's destructor ran. That destructor called `forgetImage` or `releasePdf` for the path and removed the replacement's latest request/cache/spool. A Directory page could cause the same overlap through its nested Loader. Its source binding and later `onLoaded` property assignments also lacked an explicit point at which the old nested child relinquished ownership before the new child started.

**Fix.** `PaneHostPage.unloadPane()` synchronously calls an optional child's `deactivate()` before `setSource("")`. Reload, empty selection, and host-destruction cleanup use the retirement boundary. `ViewerDirectoryPage.deactivate()` forwards it to the nested child; nested replacement explicitly unloads/deactivates first and constructs the new child with its final initial target properties. Same-kind navigation retargets the existing child without an intermediate empty-path request.

Image and PDF pages clear their sources, release the captured cached/pending paths they own, and clear their ownership records before destruction. PDF closes its document before deleting its spool. Text stops accepting its pending match and releases displayed lines. Web stops its view but keeps the native navigation policy installed until object teardown, rather than restoring an unsafe default. Markdown stops accepting reads, clears pending link UI and the web overlay, clears image sources, and releases and clears both resolved and pending image bookkeeping. Repeated deactivation and eventual destruction are harmless because ownership has already been cleared; Markdown also has a parent-detach fallback. Host open/title/close handlers capture the loaded item and ignore signals from an outgoing item after replacement.

**Why this choice / limits.** The ownership boundary now occurs synchronously before replacement starts, instead of depending on deferred destructor timing. Captured resource paths prevent cleanup from following a subsequently changed target. This uses the existing single-live-mobile-pane model without a new public resource-leasing API. Service generation checks still govern in-flight RPC replies. Decoder work already running remains Qt-owned and subject to existing image/raster limits. Native web policy is not removed early.

**Regression coverage.** `replacingAPaneRetiresResourceOwnershipBeforeDeferredDeletion_data` / `replacingAPaneRetiresResourceOwnershipBeforeDeferredDeletion` cover image/PDF, pending/cached resources, direct/nested Directory ownership, same-path replacement, and explicit delivery of deferred deletion. `replacingMarkdownRetiresImageOwnershipBeforeDeferredDeletion_data` / its matching test cover Markdown images, stale old replies, and completion of the current replacement. `openingMarkdownFromTheDirectoryUsesOneCurrentRootRead` and `tappingASecondFileRetargetsTheLoadedViewer` retain construction/retargeting checks. These are coverage descriptions, not passing results or real Android/Darwin/QtWebView runtime claims.

**Files / symbols.** `src/mobile/qml/PaneHostPage.qml`: `unloadPane()` and captured-item handlers; `ViewerDirectoryPage.qml`, `ViewerImagePage.qml`, `ViewerPdfPage.qml`, `ViewerTextPage.qml`, `ViewerWebPage.qml`, `ViewerMarkdownPage.qml`: deactivation/ownership cleanup; `src/mobile/tests/tst_mobileshell.cpp`. Markdown's coordinated lifecycle correction belongs to this issue, not an additional TMM label.

**PDF close mechanism:** TMV6 completes the PDF part of this lifecycle boundary. Clearing the QML source alone does not close Qt’s native decoder; the final implementation uses the service’s public-Qt source/close bridge before releasing the file. TMV1’s synchronous old-page deactivation and idempotent captured-path release remain the ownership fix.

### TMV2 — Very short PDF pages multiplied native page objects within one viewport

**Category / impact.** Native item/model resource bounds. Existing image-pixel limits did not prevent a large number of tiny PDF page groups from being active at once.

**Context.** Each visible PDF page brings more than raster pixels: it has native page images, selection objects, link models, and delegates. A document can have valid but extremely wide, short page geometry. At phone width such pages become only a fraction of a display-independent pixel tall, allowing many to fit in one viewport and its cache area.

**Before-case / root cause.** [INFERENCE] A 512-page PDF with 14400-by-1-point pages produced approximately 0.03dp-high page images. The existing 4dp spacing limited the count geometrically, so the old fanout was not literally unbounded, but hundreds of page/model groups could still become active. The separate 16 Mi-pixel and 8192-axis raster limits did not address object count.

**Fix.** Each PDF delegate reserves a page slot at least `MobileTheme.touchTarget` (48dp) high. The aspect-correct raster remains at the top of that slot, preserving the coordinate origin and scale used by selection and links. Existing raster-size limits, zoom handling, invalid-geometry rejection, and external URI-link refusal are unchanged.

**Why this choice / limits.** A minimum slot height bounds the number of native page groups that fit in the live viewport/cache area without distorting the document raster or link geometry. It addresses native object fanout, not just decoded pixels, and does not claim a new total-document page limit.

**Regression coverage.** `pdfWidePagesKeepTheLiveDelegateCountBounded` loads a self-contained 512-page wide PDF, checks a practical live-page-model bound and usable slot height, scrolls to the end, and checks again. `pdfRastersStayBoundedForTallPagesAtMaximumZoom` and `pdfLinksNeverLeaveTheAppButInternalNavigationAndZoomWork` retain independent raster, navigation, and link checks. No executed result is asserted.

**Files / symbols.** `src/mobile/qml/ViewerPdfPage.qml`: PDF page delegate slot geometry; `src/mobile/tests/tst_mobileshell.cpp`.

### TMV3 — A write response without a usable revision was reported as successful

**Category / impact.** Guarded-write API correctness. A successful-looking response could supply an unusable baseline for the consumer's next save.

**Context.** `MobileViewerService.writeFile()` publishes `fileWritten(path, revision)` after a remote write. Revisions let a later save state which file version it is replacing. An empty revision is not a normal successful-write version: it means CREATE-ONLY on the next write.

**Before-case / root cause.** [INFERENCE] The service checked that `revision` was a JSON string but accepted an empty string. A response containing `{ revision: "" }` could therefore announce a successful guarded save while leaving the consumer unable to issue the next guarded edit against the version just written.

**Fix.** Success now requires a nonempty revision; an absent or empty revision follows the existing visible failure path instead of emitting `fileWritten`. The header explicitly states the successful-write contract.

**Why this choice / limits.** Rejecting the invalid response preserves the meaning of success rather than manufacturing a revision. This was a latent facade defect: the native editable page uses the separate `EditorController` implementation, which was not changed by this issue.

**Regression coverage.** `aWriteConfirmedWithoutARevisionIsAFailure` exercises both missing and empty revisions and requires an error instead of success, without pinning incidental error wording.

**Files / symbols.** `src/mobile/MobileViewerService.cpp` and `.h`: `writeFile()`, `fileWritten`; `src/mobile/tests/tst_mobileviewers.cpp`.

### TMV4 — Normal mobile Back and window-close routes bypassed the dirty-editor decision

**Category / impact.** Unsaved editor-buffer protection and consent ownership. Normal user navigation could destroy the latest in-memory edits without asking; a delayed confirmation also had to be prevented from acting on a different document or pane.

**Context.** The native editor already had a Discard/Cancel decision for changing its path or explicitly reloading. Recovery snapshots are delayed, so the latest RAM buffer is not necessarily durable yet. Even a recovery copy is not consent to discard the live buffer. Mobile navigation can originate in the pane header, hardware Back/Alt+Left, or window close, and each must reach the same current editor decision.

**Before-case / root cause.** [INFERENCE] Those shell routes directly called `ctl.back()`. They unloaded the editor without using its existing dirty-document sheet. The protection covered a subset of document changes, not the shell routes that removed the page itself.

**Fix.** `PaneHostPage.requestBack()` delegates to the current child's optional `requestClose()`; pages without that method keep normal navigation. `ViewerEditorPage.requestClose()` uses the existing Discard/Cancel sheet while dirty and emits `closeRequested` immediately only when clean. `MobileMain` routes its shortcut and `onClosing` handling through `window.requestBack()`, which calls the current pane host at Pane stage and otherwise uses controller navigation.

Consent records the exact controller, actual opened path, accepted server, Dev Session, pane, target, and requested action. Document or scope changes retire it. Discard consumes that captured context before performing the action, making stale or duplicate acceptance inert; Cancel preserves RAM. Host handlers also reject an outgoing editor's late close signal after replacement. Accepted discard stops the pending snapshot timer before explicit reload, navigation, or close.

**Why this choice / limits.** The existing document decision now covers every normal Back/window route, and consent is permission for one captured document/context/action rather than a reusable global yes. This does not broaden or redesign forced controller/transport transitions.

**Regression coverage.** `leavingADirtyEditorThroughEveryBackRouteRequiresDiscard_data` / `leavingADirtyEditorThroughEveryBackRouteRequiresDiscard` drive the real shell/editor through header, hardware Back, and window-close routes. Assertions require preservation of latest RAM across context retirement, stale consent, and Cancel; explicit Discard returns to the picker; duplicate acceptance is inert; and a late old-editor close cannot close a replacement terminal. No executed test or native-device result is claimed.

**Files / symbols.** `src/mobile/qml/PaneHostPage.qml`: `requestBack()` and child close handling; `src/mobile/qml/ViewerEditorPage.qml`: `requestClose()`, consent/discard handling; `src/mobile/qml/MobileMain.qml`: `window.requestBack()`, shortcuts, `onClosing`; `src/mobile/tests/tst_mobileshell.cpp`. The coordinated root-window routing belongs here, not to another TMC issue.

**Popup lifecycle and actual UI smoke:** The system-Back regression waits for the real Popup opened/closed lifecycle before sending a second action; a key sent during the rejection transition could be consumed by the outgoing popup. In Main’s isolated live mobile reference app, Alt+Left on an edited real remote file opened the discard question; Cancel retained the exact unsaved text, verified through the real clipboard. A fresh Alt+Left followed by Discard returned to the pane list. The remote file remained byte-for-byte unchanged after both decisions. This is Linux reference-client evidence, not Android/iOS hardware certification.

### TMV5 — A missing read-completeness flag was treated as a complete file

**Category / impact.** Required RPC metadata validation. A client must not announce a complete read when the server supplied no assertion that the payload is complete.

**Context.** The existing read-file response requires a boolean `truncated` field. It tells text, image, and PDF consumers whether the bytes are only a prefix. `remote/src/files.ts` already constructs that field, and `remote/src/rpc-types.ts` already requires it; enforcing it is not a protocol extension.

**Before-case / root cause.** [INFERENCE] `MobileViewerService.decodeRead()` allowed an undefined field and used `toBool(false)`. Otherwise readable bytes without `truncated` could be published as complete text, admitted to the image cache, or written as a PDF spool. The client's own comments already warned against synthesizing completeness.

**Fix.** Every read consumer now requires a boolean flag and forwards the checked value without a default. The public header explicitly says the field is required. Valid test fixtures were updated to include it, including fixtures designed to test a different malformed field, so those cases continue reaching their intended validation branch rather than failing early for unrelated missing metadata.

**Why this choice / limits.** Refusing a malformed response preserves the existing wire contract and avoids inventing information about completeness. There is no schema/version change in this correction.

**Regression coverage.** `aMissingTruncationFlagIsRefused_data` / `aMissingTruncationFlagIsRefused` cover text, image, and PDF consumers, requiring an error, no success announcement, and no image-cache admission. Existing malformed-flag, oversized-read, released-request, and peer-retirement cases remain applicable.

**Files / symbols.** `src/mobile/MobileViewerService.cpp` and `.h`: `decodeRead()` and read-result contract; `src/mobile/tests/tst_mobileviewers.cpp`; existing wire contract in `remote/src/rpc-types.ts` and `remote/src/files.ts`.

### TMV6 — Clearing a PDF source did not close its native decoder before file removal

**Category / impact:** Native document and temporary-file lifetime. A PDF could remain open after its private rendering file had been removed, retain page/model state after leaving the viewer, or attach raster/selection/link work before valid layout. This was a newly confirmed baseline defect during integration, not merely a test timing issue.

**Context:** The native mobile viewer downloads a complete bounded PDF into an owner-private temporary rendering directory because QtQuick PDF accepts a file URL. A QML `PdfDocument` is a wrapper with a native `QPdfDocument` extension. File ownership and decoder ownership are separate: clearing a displayed URL is not necessarily the native decoder’s close operation. The same remote path also produces the same digest filename, so an unchanged URL must be able to reopen after a close.

**Observed before-case:** Main compiled a small owned program using the installed public Qt 6.10.2 APIs and a real generated one-page PDF. It first required native Ready, one page and an actual rendered raster. Setting the QML wrapper source to empty emitted `QML PdfDocument: Cannot open:` and left the native document at status 2 (Ready), page count 1. Calling public `QPdfDocument::close()` then produced status 0 (Null), page count 0. The probe exited successfully only after observing both results. This reproduces the exact incorrect source-empty close assumption; it is not a claim that the whole baseline application was separately rebuilt for that probe.

**Root cause:** The old QML deactivate/reload path cleared `documentSource` through a direct source binding and then released/unlinked the file. Qt’s empty-source setter attempts a load of an empty filename rather than closing the native document. Its unchanged-source early return also means that closing natively and merely reassigning the same URL would not reopen it. Separately, page geometry validation checked PDF dimensions without requiring a positive finite viewport; zero-width initial layouts could yield scale zero and attach native models too soon.

**Final fix:** The existing `MobileViewerService` exposes `bool setPdfDocumentSource(QObject *document, const QUrl &source)` as a QML-callable method. Through public `qmlExtendedObject` and `qobject_cast<QPdfDocument *>`, an empty URL closes the actual native document synchronously, without calling the broken empty-source wrapper setter. A nonempty URL must identify a rendering file currently owned by this exact service. A changed URL goes through the wrapper’s public source property so its resolved source and page rendering remain correct; an unchanged owned URL explicitly invokes native load. Real decode errors still appear through native status/error handling. The code is guarded for builds without Qt PDF; it adds no private Qt header/API, arbitrary local-file loading interface, new service class, remote RPC or schema version.

The service tracks wrapper/source associations. Explicit release revokes the path’s ownership before closing every associated decoder, then removes the file. Peer loss/replacement, borrowed-client destruction and service destruction close all associated documents before deleting the retiring directory, even when QML wrapper deletion is delayed. Cleared associations retain a single destruction connection; wrapper destruction removes the registration. Close iteration captures guarded QObject identities and their original URLs because synchronous native signals can destroy or retarget another wrapper.

**Reentrant cleanup correction caught in review:** Native close emits Null synchronously. An intermediate implementation closed documents while the URL map still authorized them, allowing a Null callback to reopen the same retiring file before the purge removed it. The final purge revokes all retiring URL authority first and moves the old temporary-directory owner into a local variable, keeping files present until old closures finish. Original-URL snapshots prevent the old pass from closing a wrapper retargeted to a new-generation file by a callback; the old directory’s later destruction cannot remove the new directory. A destructor-only shutdown flag rejects new PDF requests/nonempty opens and retires pending PDF publication. This corrects an intermediate implementation under TMV6, not an additional baseline finding.

**QML rendering changes:** Logical document-source changes use the service bridge instead of a direct PdfDocument source binding. Raster, text-selection and link-model documents attach only when viewport and page dimensions are finite and positive; an invalid layout retains a nonzero safe selection scale. Invalid-geometry rasters are hidden and have both document and source cleared: Qt’s page image can otherwise keep an old source and start its fallback file decoder after a source-size change. Valid rendering still uses the same owned absolute source and the native PDF extension. Existing per-page pixel bounds and the separate 48-dp minimum page slot remain.

**Regression coverage:** Real-native tests load and render a valid PDF through the actual QML wrapper, inspect Ready/page counts/page-list models and decoded raster state, reject foreign/decorated/other-service URLs, close while the file still exists, require empty native/view models, reopen an unchanged URL, reload the same digest path, and verify final deactivation/release. Geometry tests force the live viewport width to zero and then restore it, checking actual decoder/selection/link detachment and a real rendered raster after recovery. Service-retirement rows keep two real QML wrappers alive on one file across transport loss, replacement, client destruction and service destruction; both Null callbacks must observe the old file still present and the retiring URL must already be refused. A reconnect-in-close-callback row publishes a new-generation file, retargets the other wrapper, and requires that new document/file to survive and render after old cleanup. These are native behavior checks, not mock callbacks or method-forwarding assertions.

The two new page-list probes were also corrected to traverse real visual children and identify the native ListView plus its page/zoom structure. QObject-only traversal or an incidental object name was not accepted as proof that the actual list existed; render, close/reopen, order and geometry assertions remain unchanged.

**Why this choice / limits:** Use Qt’s public close/load operations through the existing owner of the private files. Keep old files present while native close signals run, but revoke reopening before those signals. This fixes decoder/file order without inventing a global file cache or claiming secure erasure. Already-running raster work remains governed by Qt’s public APIs and existing bounds; Android/iOS runtime behavior is not inferred from Linux execution.

**Files / symbols:** `src/mobile/MobileViewerService.{h,cpp}` — source bridge, active-document associations, release/purge/destructor ordering and shutdown; `src/mobile/qml/ViewerPdfPage.qml` — source transition and positive-geometry model/raster gates; `src/mobile/tests/tst_mobileshell.cpp` — real-native lifecycle/render/reentrancy/geometry checks; `docs/DEVELOPMENT.md` and `docs/SPEC.md` — native decoder-close and file-authority contract. Public source references: [QPdfDocument load/close](https://code.qt.io/cgit/qt/qtwebengine.git/tree/src/pdf/qpdfdocument.cpp?h=6.10.0), [PdfPageImage](https://github.com/qt/qtwebengine/blob/v6.10.0/src/pdfquick/qquickpdfpageimage.cpp).

### TMV7 — Ordinary PDF pages need white paper behind transparent pixels

**Category / impact:** Visible native PDF rendering bug. A normal PDF with black text could display that text against the dark viewer surface rather than against white paper. This is a rendering correction, not a change to document contents or a new dark-mode conversion feature.

**Context:** The mobile PDF page uses Qt's native PDF document and page image. A PDF often describes only its text and drawings; it need not include an instruction that paints white across the page. Its decoded page image can therefore contain transparent pixels. The viewer must supply the paper color underneath those pixels while keeping the area outside the page consistent with the app's theme.

**Observed before:** Main opened a valid 599-byte, one-page PDF in the actual mobile application using the default Qt Quick rendering backend. The PDF contained ordinary black text, no page-sized background fill, and a 400-by-500-point page. The native page reached Ready and the app displayed “Page 1 of 1,” but the text appeared on the dark surface. The captured empty-paper point at screen coordinate (80, 90) was RGB (24, 24, 37), the app's dark background. This was a real rendered page, not an error screen or a mocked image. Existing linked-PDF fixtures had painted their own colored backgrounds and had not covered this common case.

**Root cause:** `ViewerPdfPage.qml` put the transparent native raster directly over `MobileTheme.surfaceDeep`. Unpainted page pixels showed the viewport color.

**Fix and reason:** Add an explicit white rectangle immediately behind each decoded page image. It is visible only for a current, ready native document and a successfully decoded image with positive geometry. Its size follows the image's actual painted width and height, with the same horizontal centering and top alignment. It does not fill the entire delegate or the unused part of a short page's minimum 48-dp slot. PDF ink and paper retain their normal colors; only the surrounding viewport follows the theme. Native document ownership, raster limits, zoom, selection, links, and resource release are not replaced.

**Coverage and exercised verification:** `pdfTransparentPagesPaintWhitePaperWithReadableInk` opens a complete native PDF containing default black text without a background-fill command. Four data rows cover portrait and short-page geometry in dark and light preferences. Real window grabs must show white empty paper and at least 20 dark glyph pixels with measured contrast of at least 4.5:1. Short-page rows also verify that the unused part of the minimum slot remains different from white paper. Main built and ran all four rows successfully, then reopened the same 599-byte PDF in the actual default-backend app. Text was clearly visible on white paper; the same empty-paper point was RGB (255, 255, 255). The area below the page stayed dark, and the page indicator still read “Page 1 of 1.” Main then used the real header Back action to return to the pane list. Separate native-close and spool-release evidence is recorded under TMV6; this visual change does not substitute a new lifecycle implementation.

**Files:** `src/mobile/qml/ViewerPdfPage.qml`; `src/mobile/tests/tst_mobileshell.cpp`; the PDF behavior description in `docs/DEVELOPMENT.md`.

**Limits:** The actual application run was Linux with Qt 6.10.2. Host regressions cover both preferences and two page shapes; no physical Android or iOS run is claimed.

## Native mobile Markdown model and layout

### TMM1 — Markdown filename escapes were not decoded, and angle destinations ended too early

**Category / impact.** Native Markdown destination parsing. Supported Markdown syntax could name the wrong remote file or break an otherwise valid image destination.

**Context.** Markdown can escape ASCII punctuation in link/image destinations. It also supports angle-delimited destinations, inside which parentheses can be literal filename characters. The native parser extracts these destinations before the existing link scheme and image-path safety checks; the page resolves document-relative images against the document's directory and constrains them to the repository.

**Before-case / root cause.** [INFERENCE] `[manual](docs/a\(b\).md)` and `![badge](assets/a\(b\).png)` retained their backslashes when converted into paths. For `![angle](<assets/a)b(c.png>)`, the closing parenthesis inside the angle destination could terminate parsing early. `findDestEnd()` did not respect the angle-delimited spelling, and `destinationOf()` did not remove Markdown punctuation escapes.

**Fix.** Destination matching now tracks the initial angle-delimited form. The new internal `unescapeDestination()` removes Markdown ASCII-punctuation backslash escapes before the existing safety checks, and escaped `>` is not mistaken for an angle terminator. Scanning still uses the existing charged inline-finder allowance. An unterminated angle expression can remain literal instead of having syntax silently discarded; a regression stopped pinning the incidental exact text `"a"` while retaining the meaningful no-link assertion. Header documentation now states that image paths are document-relative and `ImagePathRole` values are Markdown-unescaped.

**Why this choice / limits.** Syntax decoding belongs before path validation: a correctly decoded path can then be checked for traversal, including after percent decoding. This adds no URL schemes, network access, title support, OS URL handling, or wider CommonMark feature promise.

**Regression coverage.** `TstMarkdownModel::destinationEscapesAndAngleParenthesesNameTheActualPath()` covers escaped link/image destinations, parentheses in angle image/web destinations, and refusal of escaped-separator traversal. Existing malformed-angle safety coverage remains relevant.

**Files / symbols.** `src/mobile/MarkdownModel.cpp`: `findDestEnd()`, `destinationOf()`, `unescapeDestination()`; `src/mobile/MarkdownModel.h`: image-path contract; `src/mobile/tests/tst_markdownmodel.cpp`.

### TMM2 — List nesting followed marker offsets instead of the parent's content column

**Category / impact.** Native Markdown list layout and paragraph grouping. A marker being to the right of another marker does not necessarily make it a child of that item.

**Context.** Ordered markers can consume several columns, and a tab after a marker advances to a tab stop rather than contributing one visual column. The parser needs the column where the parent item's content begins to decide whether a later marker or continuation belongs inside it. The model intentionally represents list items as flat rows carrying a nesting level rather than arbitrary nested block containers.

**Before-case / root cause.** [INFERENCE] In `100. parent\n     - child\n  - sibling`, the two-space marker could incorrectly become a child merely because it was right of column zero; the parent's content starts at column five. In `-\tparent\n  separate paragraph`, counting the tab as one character could absorb the under-indented paragraph even though the content starts at column four. Prior list state could also wrongly make a root-indented marker a child.

**Fix.** `listMarkerAt()` counts whitespace after markers in columns using four-column tab stops. The parse stack retains enclosing content columns and removes any parent whose content column the next marker does not reach. Root-indented code is no longer classified as a child just because an earlier list existed, and a pending paragraph does not supply a containing list for an otherwise root-indented marker.

**Why this choice / limits.** Content columns express the actual indentation requirement and work for multi-digit ordered markers and tabs. Existing ordered/task roles, flat item/level representation, four-column tab stops, and the deliberately limited continuation behavior remain. This is not a promise of full CommonMark nested-container parsing.

**Regression coverage.** `listNestingRequiresTheParentsContentColumn()` checks a legitimate five-column child, a shallow root sibling, and a root-indented literal marker. `tabsAfterListMarkersUseColumnStops()` distinguishes a valid tab continuation from an under-indented separate paragraph. No execution result is asserted.

**Files / symbols.** `src/mobile/MarkdownModel.cpp`: `listMarkerAt()`, `MarkdownModel::parse()` list stack; `src/mobile/tests/tst_markdownmodel.cpp`.

### TMM3 — A single Markdown block could create a native object tree for every word

**Category / impact.** Mobile rendering resource bounds. List virtualization limited visible blocks but did not limit the number of children created inside one enormous block.

**Context.** `ViewerMarkdownPage` renders model blocks in a `ListView`. Its former `SpanText` implementation split content into word-sized runs and used repeaters to create native items, text objects, and mouse areas. An accepted document can contain a very large paragraph with no block breaks; parser work budgets and block-level virtualization do not constrain that separate QML object expansion.

**Before-case / root cause.** [INFERENCE] An 8 MiB paragraph of repeated short words remained one model block yet could allocate millions of native items, including for completely unstyled text. The renderer applied the interactive per-piece representation even where no inline interaction was needed.

**Fix.** Blocks without spans now use one plain `Text`. Styled blocks use interactive runs only up to 16,384 UTF-16 units and 256 lexical pieces. Larger or token-heavy blocks fall back to one `Text` containing the complete parsed body, preserving explicit newlines, font size, heading boldness, and color. Long/no-span blocks do not generate runs; token materialization aborts when its limit is reached. Small styled/link blocks retain the existing interactive behavior. The header documents this rendering fallback.

**Why this choice / limits.** The fallback bounds native child-object amplification without truncating the visible text or weakening `PlainText` handling. Oversized blocks deliberately lose inline styling and link actions, not their body content. This is not a bound on the accepted document's total size or every allocation inside Qt's font layout.

**Regression coverage.** `markdownLargeBlocksKeepNativeItemCountsBounded_data` / `markdownLargeBlocksKeepNativeItemCountsBounded` cover large plain and styled blocks, more than 256 lexical pieces, and a small bold/link control exercised by an actual mouse click. The migrated `markdownTextPreservesLinesAndWrapsLongTokens` checks the complete visible `alpha\nbeta` `Text`, actual two-line layout, and height against native font metrics, rather than demanding obsolete separate token children. Its long styled-token width, line-count, and wrapping-height assertions remain.

**Files / symbols.** `src/mobile/qml/ViewerMarkdownPage.qml`: `SpanText`, run/line materialization and plain fallback; `src/mobile/MarkdownModel.h`; `src/mobile/tests/tst_mobileshell.cpp` for coordinated regression additions and representation-contract migration.

**Native integration probes:** Text lookup now walks visual child items and requires the actual native Text/layout/font structure plus full expected content; a model delegate’s unrelated `text` role is not a line-layout result. Item-budget checks also walk real visual descendants rather than QObject-only descendants. Complete fallback text, native two-line preservation, wrapping width/height, item limits, small styled text and real link-click behavior remain asserted.

### TMM4 — A retargeted Markdown page kept the previous document's link offer or web overlay

**Category / impact.** Document-scoped link UI lifetime. A confirmation and overlay offered by an old document should not remain active over a different document.

**Context.** A mobile Markdown page can ask the user to confirm an HTTPS link and then display the linked content in a web overlay. The same page object can be reused for another `remotePath`, so page-object lifetime alone does not identify the document that offered the link.

**Before-case / root cause.** [INFERENCE] `reload()` reset the model, document, and images but left `pendingLink`, `linkSheet`, and `webLoader` intact. After retargeting, the old document's destination could remain offered, or its web overlay could obscure the newly requested document.

**Fix.** Retarget/reload clears the pending destination, closes its confirmation dialog, and tears down the prior web Loader and target before requesting the new document. A stale accepted callback has no old destination left to act on. The coordinated TMV1 deactivation path also retires link and overlay state when the page itself is replaced.

**Why this choice / limits.** The offer now belongs to the document that created it rather than the reusable page object. There is no automatic link opening and no expansion of permitted destinations. Resource-owner cleanup at Loader replacement remains reported under TMV1, not counted again here.

**Regression coverage.** `retargetingMarkdownRetiresItsOfferedLink` requires the dialog to close on retarget and an old accepted callback to remain inert. Actual web-overlay teardown uses the same source path but is not a demonstrated real QtWebView runtime result; a WebView-enabled integrated exercise is separate from the authored dialog regression.

**Files / symbols.** `src/mobile/qml/ViewerMarkdownPage.qml`: `reload()`, `pendingLink`, `linkSheet`, `webLoader`; `src/mobile/tests/tst_mobileshell.cpp`.

### TMM5 — Unicode terminal placement changed when an invisible attribute split the text run

**Category / impact.** Fixed-cell terminal painting. Unicode glyph placement must follow terminal columns rather than incidental batching boundaries or proportional fallback-font advances.

**Context.** The terminal model places characters in fixed cells. `QPainter::drawText` can apply bidirectional ordering, shaping, and font-specific advances to a whole string. The renderer does not visibly render Blink, so adding/removing that attribute should not change the image. A text batch boundary created only by Blink bookkeeping must therefore have no effect on glyph positions.

**Before-case / root cause.** [INFERENCE] Uniform Hebrew `אבג` could be painted as one string, while the same cells with SGR5 on the middle glyph and SGR25 on the last were split into separate strings. Bidi/shaping could then alter order or placement even though the attribute difference was invisible. Fallback-font advances could also move later Unicode glyphs away from their terminal columns.

**Fix.** `MobileTerminalView::paint()` retains contiguous single-width ASCII batching as the fast path. Non-ASCII cells are painted individually at their actual VT columns. Existing wide-cell handling, backgrounds, SGR styling, damage bands, and cursor behavior remain unchanged.

**Why this choice / limits.** ASCII keeps efficient batching where it does not introduce the Unicode ordering/advance problem. Non-ASCII painting follows the terminal's fixed-cell model rather than attempting cross-cell text shaping. This does not alter the terminal parser, session identity, or supported attribute set.

**Regression coverage.** `terminalGlyphPositionsDoNotDependOnInvisibleAttributeBoundaries()` paints both Hebrew variants with the cursor hidden and compares the images. It also requires the glyph image to differ from a blank screen so equality cannot succeed merely because nothing was drawn. The available Qt fallback fonts must supply distinguishable Hebrew glyphs. The coordinated test lives with mobile terminal coverage.

**Files / symbols.** `src/mobile/MobileTerminalView.cpp`: `paint()`; `src/mobile/tests/tst_mobileterminal.cpp`. `MobileTerminalView.h` remains intentionally unchanged.

## Terminal escape-sequence screen state

### TVT1 — Repeating alternate-screen SET overwrote the application's own saved cursor

**Category / impact.** Terminal saved-state correctness. Duplicate activation was already intended to preserve an active alternate screen, but it could still corrupt the application's separately saved cursor and rendition.

**Context.** `CSI ?1049h` saves the primary-screen cursor and enters the alternate screen. Applications running there can use `ESC 7` (DECSC) and `ESC 8` (DECRC) to save and restore their own cursor/rendition in the alternate screen's saved slot. A duplicate 1049 SET should not overwrite that application-owned saved state.

**Before-case / root cause.** [OBSERVED, Main's isolated native-model probe; root-cause explanation from source] On a 10×4 grid, write `primary`, enable 1049, move to row 2 column 3, select red, and issue `ESC 7`. Then move to row 3 column 5, select green, issue another `CSI ?1049h`, and finally `ESC 8 X`. The duplicate SET called `saveCursor()` before `enterAltScreen(true)` noticed the alternate screen was already active and did nothing. That save overwrote the application's alternate-screen saved slot: the baseline put X in the moved green position, while corrected code restored the saved red position. The guard existed after the state-changing operation rather than around the complete activation.

**Fix.** The mode-1049 enable branch now saves and enters only when the alternate screen is not already active. First entry and normal exit retain their existing behavior.

**Why this choice / limits.** The correction makes the existing no-op-on-duplicate-activation rule cover saved cursor state as well as visible content. It is not a repeated RESET correction. Repeated-reset semantics, retained/discarded alternate-grid policy, and switching among modes 47/1047/1049 are unchanged. A simple repeated-SET content probe does not demonstrate this bug; the application's `ESC 7` → duplicate 1049 SET → `ESC 8` sequence is essential.

**Regression coverage and observed evidence.** `TstVt::repeatedAltScreenActivationPreservesSavedCursor` feeds the complete stream both at once and byte by byte, checks X's position/color and cursor, checks for unintended row-3 text, and checks primary-content restoration with subsequent `!` output. Main's exact isolated native-model probe observed baseline X at cursor row 2, column 5 with green `ff00cd00`, versus corrected cursor row 1, column 3 with red `ffcd0000` (the recorded cursor rows are zero-based). The probe used matching baseline/current headers and actual `VtScreen.cpp`, `VtParser.cpp`, `VtTypes.cpp`, and generated MOC against Qt 6.10.2, without mocks or transport. Its three TVT1–3 cases failed on baseline (exit 3) and passed on corrected code (exit 0); this is not a full `tst_vt` passing claim. Evidence: `local://round3-vt-proof.txt`; probe source: `build/dev/.tmp/hunt-r3/vt-probe/probe.cpp`. The earlier simpler repeated-SET-only probe passed both revisions and is not claimed as reproduction.

**Files / symbols.** `src/vt/VtScreen.cpp`: `VtScreen::setMode`, mode 1049 enable branch; `src/vt/tests/tst_vt.cpp`: `repeatedAltScreenActivationPreservesSavedCursor`.

### TVT2 — Restoring a saved cursor kept the intervening program's autowrap setting

**Category / impact.** Terminal state restoration. Cursor restore must restore saved autowrap mode, not let an intervening application's mode determine subsequent output.

**Context.** DEC saved cursor state includes the wrap flag as well as position and rendition: the [DECSC reference](https://vt100.net/docs/vt510-rm/DECSC.html) explicitly lists autowrap/no-autowrap, and [DECRC](https://vt100.net/docs/vt510-rm/DECRC.html) restores that state. This engine also uses its save/restore machinery for 1049 alternate-screen transitions and existing ANSI aliases. A cursor can be visibly at the right margin while carrying deferred wrap: the next printable glyph, rather than the current cursor position, reveals the difference.

**Before-case / root cause.** [INFERENCE] On a five-column screen, printing `abcde` with autowrap enabled, saving, disabling autowrap, moving elsewhere, restoring, and printing X could overwrite the right-margin e instead of starting the next row. Pending wrap was saved, but restore conditioned it on the intervening live autowrap mode. Conversely, saving with autowrap disabled and restoring after another program enabled it could make later primary output wrap unexpectedly. An alternate-screen program changing mode 7 could leak the same setting back into the primary screen. `SavedCursor` simply did not retain DECAWM.

**Fix.** Each existing per-screen saved-cursor slot now stores autowrap. `restoreCursor()` restores that mode before restoring pending wrap, with a short comment explaining the dependency.

**Why this choice / limits.** Restoring the mode first makes the saved deferred-wrap state meaningful again and follows the documented saved-state contract. No public mode/parser interface was added. Restore-without-a-save behavior, pending-wrap invalidation on resize, and the existing lack of origin-mode support remain unchanged.

**Regression coverage and observed evidence.** `TstVt::savedCursorRestoresAutowrap` covers enabled/disabled saved modes, whole/byte-split input, `ESC 7/8`, existing `CSI s/u` aliases, and 1049 round trips; assertions check actual right-margin text, later output location, cursor, and observable autowrap. `savedCursorRestoresDeferredWrap` retains resize/pending-wrap coverage. Separately, the owner records Main's actual isolated native-model observation for the disabled-at-save case: baseline produced wrap enabled, row 1, text `ABCD`; corrected code produced wrap disabled, row 0, text `ABCE`. This is attributed parent-observed before/after evidence, not this reporting worker's execution or a claim that the full suite passed. The recorded probe source is `build/dev/.tmp/hunt-r3/vt-probe/probe.cpp`.

**Files / symbols.** `src/vt/VtScreen.h`: private `SavedCursor`; `src/vt/VtScreen.cpp`: `saveCursor()`, `restoreCursor()`; `src/vt/tests/tst_vt.cpp`.

### TVT3 — Unsupported erase commands changed what the next printed character did

**Category / impact.** Unsupported terminal-command handling. An ignored erase mode must not silently cancel deferred wrap or damage a wide character when the next glyph arrives.

**Context.** ED and EL erase parts of the display or line for supported parameters. Unsupported parameters should be consumed without changing state. Deferred wrap is state even while the visible cursor remains at the right margin: clearing it changes whether the next character overwrites that margin or starts the next row.

**Before-case / root cause.** [INFERENCE] On a five-column screen, printing `abcde`, sending unsupported `CSI 4J` or `CSI 3K`, and then printing X could replace e on the first row instead of putting X at the start of the second. With `abc中` filling the margin, overwriting part of the wide cell could also destroy that character. The default switch branches did not erase anything, but execution still reached unconditional `m_pendingWrap = false`.

**Fix.** `eraseInDisplay()` and `eraseInLine()` return immediately for unsupported parameters. Supported ED 0/1/2/3 and EL 0/1/2 keep their existing erasure and pending-wrap policy.

**Why this choice / limits.** Returning before any state change implements the intended ignored-command behavior instead of special-casing a later glyph. This does not change the set of supported erase operations or their behavior.

**Regression coverage and observed evidence.** `TstVt::unsupportedEraseModesPreserveDeferredWrap` covers `4J`, `999999J`, `3K`, and `999999K`, narrow/wide right-margin text, and whole/byte-split streams. It checks both no immediate damage and the subsequent X position/preserved text, exposing the latent state change. Separately, the owner records Main's isolated native-model observation for unsupported ED 9: baseline yielded `abd` / blank with cursor row 0; corrected code yielded `abc` / `d` with cursor row 1. This is attributed parent-observed evidence, not a full-suite passing claim or execution by this reporting worker. The recorded probe source is `build/dev/.tmp/hunt-r3/vt-probe/probe.cpp`.

**Files / symbols.** `src/vt/VtScreen.cpp`: `eraseInDisplay()`, `eraseInLine()`; `src/vt/tests/tst_vt.cpp`: `unsupportedEraseModesPreserveDeferredWrap`.

### TVT4 — A grid-bounds comment used an unclear misspelled word

**Category / impact and context.** Documentation-only readability correction. The parent changed “gigabantic” to “oversized” in the comment describing terminal grid bounds. **Before-case / root cause:** the comment's misspelling made a simple size constraint harder to read; this was not a runtime defect. **Fix / rationale / limits:** use the ordinary word without changing code, semantics, or any exported API. **Regression coverage:** no runtime test is appropriate for a one-word comment correction, and none is claimed. **Files / symbols:** `src/vt/VtScreen.h`, grid-bounds comment; parent-authored edit recorded by the owner.

## Web bundle build and release tools

### TBR1 — Validate every staged web-bundle file before replacing the working bundle

**Category / impact.** Build-output preservation. [INFERENCE] A terminal or Markdown build could report failure only after deleting the previous usable embedded bundle.

**Context.** The desktop application embeds terminal, Markdown and editor web resources. Each bundle consists of a known set of JavaScript, stylesheet and page files, with source maps when requested. The build scripts first produce a temporary directory, then publish it in place of `dist`. This staging arrangement is intended to preserve the last good resources if production fails.

**Before-case / root cause.** [INFERENCE] The terminal and Markdown scripts ran esbuild and copied `index.html` while still staging, but checked the expected output files only after replacing `dist` and deleting `dist.old`. An entrypoint that stopped importing CSS could therefore let esbuild succeed without producing the required stylesheet. The later file check would fail after the good bundle had already been discarded. A zero-byte page was also accepted because the checks did not require nonempty files. These are source-derived counterexamples, not reported runtime reproductions.

**Fix.** Both scripts now follow the existing editor-builder convention: enumerate the entire required output set, including requested maps; inspect those files inside the staging directory; reject missing or zero-byte outputs; and retain their sizes for subsequent logging. Only a fully validated staging directory enters the existing replacement and rollback sequence.

**Why this choice / limits.** Validation belongs before publication, where a failure can still leave the previous directory untouched. Reusing the editor convention avoids another abstraction or build interface. The change preserves the existing rename rollback; it does not introduce concurrent-build coordination or promise recovery from process termination during filesystem publication.

**Regression coverage.** `tst_web_bundle_rebuild`, implemented by `tests/web_bundle_rebuild.sh`, invokes the real terminal and Markdown builders in a private source copy. Its empty-page and missing-CSS cases require a failed build, byte-identical previous `dist` contents, and no remaining `dist.tmp` or `dist.old`. These assertions exercise produced files and retained bytes, rather than searching script text.

**Files / symbols.** `src/web/terminal/build.mjs` and `src/web/markdown/build.mjs`, staged output validation and publication flow; `tests/web_bundle_rebuild.sh`.

### TBR2 — Rebuild embedded web resources when their TypeScript configuration changes

**Category / impact.** Missing build dependency. [INFERENCE] A configuration-only change could leave the application embedding stale JavaScript, or leave an invalid configuration unnoticed.

**Context.** CMake's `codeharbor_embed_web_bundle()` decides whether the terminal, Markdown and editor bundles need rebuilding. Although esbuild removes TypeScript syntax without performing type checking, it still reads each workspace's `tsconfig.json`; compiler settings can affect emitted JavaScript.

**Before-case / root cause.** [INFERENCE] The common input list included TypeScript and CSS sources, the build script, page, package manifests and root lockfile, but not the workspace's TypeScript configuration. With an existing bundle, changing only `tsconfig.json` did not invalidate that bundle or register a CMake configure dependency. Even malformed configuration could remain unnoticed until another tracked input changed.

**Fix.** The current workspace's `tsconfig.json` is now part of `_chb_inputs`. The existing timestamp comparison and `CMAKE_CONFIGURE_DEPENDS` mechanism therefore cover it for all three web bundles. The canonical development guide now names configuration, build script and manifests among bundle inputs; that documentation work is credited here and under TBR4, not counted as another documentation finding.

**Why this choice / limits.** The fix adds the missing direct input to the established mechanism. It does not replace the project's timestamp-based build rules with content-addressed caching.

**Regression coverage.** `tst_web_bundle_rebuild` establishes a good private terminal bundle, writes invalid JSON to its private `tsconfig.json`, and requires configure to fail instead of accepting the old bundle. The harness also prints captured diagnostics for successful configure calls, while allowing the expected-failure case to inspect its exit result without prematurely terminating the script.

**Files / symbols.** `src/qml/CMakeLists.txt`, `codeharbor_embed_web_bundle()` and `_chb_inputs`; `tests/web_bundle_rebuild.sh`; `docs/DEVELOPMENT.md`, Client build web-bundle description.

### TBR3 — Refuse release publication when preflight checks use uncommitted source

**Category / impact.** Release-integrity boundary. [INFERENCE] Successful preflight could describe a locally repaired checkout rather than the committed source being tagged and pushed.

**Context.** The version-bump helper validates version metadata from an archived committed tree, but configured preflight commands run against the working checkout. Preserving unrelated local work is useful for ordinary version operations; publishing a release additionally requires that the tested source match the source identified by the tag.

**Before-case / root cause.** [INFERENCE] A committed defect plus an unstaged or staged local repair could let `--set <committed-version> --no-commit --push` pass preflight and publish the broken commit. A non-ignored new file absent from the commit could similarly satisfy preflight. A successful preflight command could itself rewrite source without moving `HEAD`. The previous publication check guarded changes to `HEAD`, not differences between the checkout and that commit. No release operation was executed to reproduce these cases.

**Fix.** For `--push`, the helper now requires an empty `git status --porcelain --untracked-files=normal` both before and after preflight. This rejects staged changes, unstaged changes and non-ignored new files at the publication boundary. Ignored build products remain allowed. `--allow-dirty` still permits version edits that the release commit consumes, but cannot waive unrelated dirty or new source for publication. Rejection preserves the user's edits. Archived-version validation, explicit tag target, the `HEAD`-change guard and atomic push are retained. Local tagging without `--push` keeps its prior unrelated-work behavior. The version-bump skill documents the workflow, option distinction, safety rules and regression inventory.

**Why this choice / limits.** Checking both boundaries covers existing local repairs and changes left by preflight itself without discarding work. This is not a hermetic build sandbox: ignored artifacts and external toolchain state remain governed by existing conventions, and an arbitrary preflight command can temporarily change source and restore it before returning.

**Regression coverage.** `tst_bump_version_tag` / `tests/bump_version_tag.sh` uses disposable repositories for unstaged, staged and untracked repairs, plus successful preflight that changes source. Each case requires refusal before creating a local release tag and preservation of relevant file contents. The untracked-file case supplies a preflight that would actually succeed using that file, rather than relying on an unrelated failing command.

**Files / symbols.** `.omp/skills/bump-version/bump.sh`, preflight/publication checks; `.omp/skills/bump-version/SKILL.md`; `tests/bump_version_tag.sh`.

### TBR4 — Refresh installed npm dependencies before rebuilding from a changed lockfile

**Category / impact.** Dependency freshness and test isolation. [INFERENCE] A rebuilt bundle could still contain the previously installed dependency while the tracked lockfile described its replacement.

**Context.** CMake already treats the root npm lockfile as a reason to rebuild embedded web resources. That is only effective if the installed dependency tree also reflects the declared inputs.

**Before-case / root cause.** [INFERENCE] `codeharbor_embed_web_bundle()` ran installation only when the root `node_modules` directory was absent. If dependencies had been installed at version A and the lockfile later changed to version B, CMake could rebuild with the still-installed A. Directory existence was incorrectly serving as proof of installation freshness.

**Fix.** The function now checks npm's existing `node_modules/.package-lock.json` installation record. It runs the existing strict `npm ci` when that record is absent or older than the root lockfile, root manifest or current bundle workspace manifest. Installation errors retain the existing diagnostic, and this path does not rewrite tracked lock metadata. The development guide documents the refresh rule alongside TBR2's expanded build-input list.

The regression fixture also now copies `node_modules` into its disposable source tree instead of symlinking the checkout's real installation. It includes the remote workspace manifest required by the root npm workspace layout. Dependency deletion and `npm ci` in the regression therefore target the private copy, not the developer's installed tree.

**Why this choice / limits.** npm's own installation record supplies the missing freshness signal without adding a dependency manager. The rule follows npm's normal hidden-lock and timestamp conventions. Deliberate manual corruption with no changed declared input is outside this guard. Installs may use npm's ordinary dependency cache and need the same cache or registry availability as an ordinary install.

**Regression coverage.** `tst_web_bundle_rebuild` removes the private installed xterm package and then touches the private lockfile. Reconfiguration must restore the real dependency and emitted output, not merely rerun esbuild against a stale installation.

**Files / symbols.** `src/qml/CMakeLists.txt`, `codeharbor_embed_web_bundle()` installation guard; `tests/web_bundle_rebuild.sh`; `docs/DEVELOPMENT.md`, Client build web-bundle description.

## Public documentation and viewer tool instructions

### TDC1 — Keep the documented service smoke away from the user's real workspace and relay

**Category / impact.** Unsafe operational instructions. [INFERENCE] Following the old development recipe could migrate a real workspace database and interact with the event relay used by an ordinary desktop session.

**Context.** The Remote workspace (Node) section offers short commands for checking the source daemon and bridge. These commands execute application code, not a static inspection tool. `server.info` obtains server identity through the workspace database; opening that database can run migrations. A stdio daemon also starts its control listener. The bridge's default route can connect to or start the account's ordinary shared relay.

**Before-case / root cause.** [INFERENCE] The guide piped a `server.info` request to the daemon and invoked the bridge without isolating either command. With no database override, workspace access falls back to `~/.local/share/codeharbor/codeharbor.sqlite`. Without an explicit relay endpoint, bridge routing follows normal runtime/home rules. The recipe therefore contradicted the guide's later requirement to isolate live tests and could affect a desktop using another installed release.

**Fix.** The replacement recipe runs in a subshell with a fresh, short, owner-private `mktemp` root. It isolates all five resource families: **database, runtime directory, data directory, shell history and tmux namespace**. It also supplies an explicit scratch agent socket. The daemon still executes real `server.info`; the bridge example explicitly uses `--relay` on the scratch endpoint and explains that Ctrl+C stops it. Subshell cleanup removes only the newly created scratch directory. The text explains why `server.info` is not read-only, why ordinary bridge routing may affect a GUI, and why this smoke does not establish hook/client delivery.

**Why this choice / limits.** The recipe preserves a useful real-service smoke while confining its application resources. It does not override `HOME`, change ordinary profile/trust state, replace the canonical SSH fixture/preflight, or claim end-to-end coverage. Actual live suites still require the canonical isolated fixture.

**Regression coverage.** This is a documentation correction, not a new executable test. No smoke was run for this section. Any execution must use newly allocated scratch state; this report makes no service-start or delivery claim.

**Files / symbols.** `docs/DEVELOPMENT.md`, Remote workspace (Node). The documented behavior is grounded in `remote/src/codeharbord.ts` (`server.info` and stdio startup), `remote/src/workspace.ts` (`serverIdentity()`, workspace opening), and `remote/src/bridge.ts` (default proxy routing versus `--relay`).

### TDC2 — Distinguish the release an installer targets from actual connection compatibility

**Category / impact.** Incorrect compatibility guidance; no runtime behavior change.

**Context.** Managed installation selects the server release matching the client. Connection acceptance is a separate decision: the client checks the daemon's advertised RPC schema and required identity/endpoint metadata. The daemon's internal SQLite schema is yet another version, used to migrate its stored workspace.

**Before-case / root cause.** [INFERENCE] README's statement that the client “only drives the release that matches it” incorrectly made exact release equality sound like a connection gate. A manually installed service with another release number can satisfy the actual checks, and a failed managed update intentionally retains a usable older installation.

**Fix.** README now separates matching-release installation/update targets from RPC-and-metadata compatibility. In the current contract, the release is **0.4.3**, advertised `RPC_SCHEMA_VERSION` and the client's minimum RPC schema are **10**, and `server.info.schemaVersion` means that RPC schema—not the SQLite schema. Current workspace database schema is **6**, while the pin column's historical arrival in version **5** remains correctly documented. Neither database version nor exact release-number equality is the client RPC compatibility gate. These related canonical version clarifications are credited to TWS4 and the original code owners, not counted as new TDC findings.

**Why this choice / limits.** The documentation now describes the existing check rather than adding a release-equality restriction. It does not promise arbitrary newer protocols are semantically compatible, remove metadata validation, alter the runtime installation floor, or change managed update/fallback policy. Installation/runtime prerequisites and connection compatibility remain distinct requirements.

**Regression coverage.** No source behavior or test was added for this wording correction. Existing compatibility and bootstrap regressions are the executable coverage.

**Files / symbols.** `README.md`, Updating the server. Relevant implementation: `src/app/AppController.cpp`, `adoptServerIdentity()` and `AppController::kMinimumServerSchemaVersion`; `src/app/SessionBootstrap.cpp`, `defaultRemoteArtifactUrl()` and `keepExistingService()`. Related version documentation is in `docs/SPEC.md` and `docs/DEVELOPMENT.md` under TWS4.

### TDC3 — Explain that source-installation protection follows the selected service layout, not `.git`

**Category / impact.** Misleading installation-safety promise; no installer policy change.

**Context.** A connection profile identifies a remote service root. Bootstrap searches candidate entrypoints and chooses the service layout it finds. A release installation normally exposes `dist/codeharbord.js` directly below that root; source-checkout candidates live below `remote/`.

**Before-case / root cause.** [INFERENCE] README promised that a git checkout was “never overwritten by either path” and that Update server on one “changes nothing.” The implementation does not inspect `.git` for this decision. It prefers the root release-layout entry, and its forced-source guard compares the selected entry against the first release candidate. A directory containing both `.git` and root `dist/codeharbord.js` can therefore be treated as an installation, not a protected source-layout selection.

**Fix.** README now qualifies source-checkout protection by the selected `remote/dist` or `remote/src` entry layout, explicitly says the protection is not based on `.git`, and warns that a root release-layout `dist` remains an installation even when a repository is also present. The dedicated-installation-directory requirement remains. The corresponding bootstrap header comment was corrected by its original owner; that is the same contract clarification, not another documentation finding.

**Why this choice / limits.** Accurate guidance prevents users relying on a guard that does not exist. No `.git` detection was added, no arbitrary mixed directory was declared safe, and the separate bootstrap symlink/canonical-root repairs retain their original attribution.

**Regression coverage.** This change adds no runtime test or behavior. Bootstrap regression coverage belongs to the code-owner repairs.

**Files / symbols.** `README.md`, Updating the server; relevant selection and guard logic in `src/app/SessionBootstrap.cpp`, `entryCandidates()` and `ensureRemoteService()`; coordinated comment correction in `src/app/SessionBootstrap.h`.

### TDC4 — Describe viewer splitting accurately and require current pane IDs

**Category / impact.** Assistant-tool instructions. [INFERENCE] The old description could lead an assistant to assume a split had displayed content in the new pane when it had only created an empty pane.

**Context.** Viewer tools operate on panes in the active development session. A split preserves the existing pane and adds an empty one; opening known content is a separate operation, or can be requested through `viewer_open` with `newPane`. Tool calls must use IDs actually returned by current inventory or pane creation.

**Before-case / root cause.** [INFERENCE] All three shipped viewer skills described `viewer_split` as “leaving both halves as they were.” That did not explain that the second pane is newly empty, rather than a copy of the displayed document. The `.omp` skill also said IDs “are recycled” without distinguishing local counter behavior from legacy or multi-client reuse.

**Fix.** The `.agents`, `.claude` and `.omp` tool tables now say that splitting keeps the existing pane, adds an empty pane and returns the new pane ID. The `.omp` guidance requires using the current list instead of inventing IDs or reusing stale ones. This avoids both the old blanket recycling statement and an unsupported global no-reuse guarantee: the current per-session split counter does not recycle closed labels, but legacy and multi-client states can still reuse labels.

**Why this choice / limits.** An assistant should act on the current real identifier and explicitly open the intended content, not infer identity or duplication from layout. Empty-region placeholder handling is unchanged, and no document-duplication feature is promised. All skills retain ask-before-close guidance. Their additional route-validation and retired-workspace guidance belongs respectively to TIN1 and TUI6, listed below, not extra TDC4 findings.

**Regression coverage.** Existing `tst_sessionlayouts` cases `splitSerializesExactSetLayoutParams` and `splitPersistsAndReloadsByteIdentical`, together with pane-identity UI coverage, are relevant to the underlying behavior. No source-text test was added for this wording correction.

**Files / symbols.** `.agents/skills/codeharbor-viewer/SKILL.md`; `.claude/skills/codeharbor-viewer/SKILL.md`; `.omp/skills/codeharbor-viewer/SKILL.md`. Relevant implementation: `src/app/SessionLayouts.cpp`, `splitPaneStamped()`; `src/qml/Main.qml`, split command result; `remote/src/mcp/server.ts`, viewer tool descriptions.

### TDC5 — Document the named refusal that protects an unsaved editor from agent open or reload

**Category / impact.** Incomplete API documentation; no runtime behavior change.

**Context.** Viewer-control requests return named refusal codes so an assistant can distinguish invalid requests, unavailable panes and protected editor state. An editor containing unsaved changes must not be replaced simply because an agent requests open or reload.

**Before-case / root cause.** [INFERENCE] SPEC section 6.8 listed `bad_request`, `busy`, `timeout`, `not_active_session`, `unknown_pane` and `failed`, but omitted the implemented `unsaved_changes` refusal. Agent open against a dirty editor and agent reload of it already returned this code. The viewer skills recognized it, so the canonical API account disagreed with the implementation and operational guidance.

**Fix.** SPEC now includes `unsaved_changes`, explains protection for open and agent reload, and gives the safe response: save the document or open another pane. Closing is not an acceptable workaround for bypassing that refusal. Agent close still deliberately does not prompt and remains separately documented. User-driven reload has its own RAM-preservation semantics; this correction does not impose a new user-reload restriction.

**Why this choice / limits.** Naming the actual refusal gives consumers a truthful recovery path without changing the protocol or widening scope. No new error code, source behavior or test was introduced by this documentation fix.

**Regression coverage.** Existing QML viewer-command and unsaved-editor behavior coverage is relevant.

**Files / symbols.** `docs/SPEC.md`, section 6.8; implemented catalog in `remote/src/control.ts`, `CONTROL_ERROR_CODES`; dirty-editor open/reload refusal in `src/qml/Main.qml`, `runViewerCommand()`.

**Coordinated canonical updates, credited to their original issue labels rather than additional TDC findings.** The same documentation work also incorporated the following landed contracts. These are attribution notes, not another set of bug IDs or independent runtime-verification claims:

- **TAG1:** `docs/SPEC.md` section 5.2 describes literal session-name lookup in a marked tmux listing, followed by killing only the selected validated `$id`; it no longer claims an `=` exact-name target or the workspace-target separator blacklist.
- **TDA1 / TDA2:** SPEC section 6.8 describes refusing unsupported control versions without serializing the untrusted value. Section 6.3 describes checking device/inode identity again before removing a stale relay ownership lock, so another starter's replacement is not removed.
- **TDA3:** The final accepted cleanup is embedded-newline framing instead of exact JSON property order, plus removal of four English-message regex expectations while retaining bounded error codes and correlation checks. All original Node/C++ source-parity gates and the filename `rpc-mirror.test.ts` remain. SPEC retains that reference and parity claim; DEVELOPMENT retains both-side schema pinning and clarifies that the C++ informational database constant is not the RPC compatibility gate. The broader deletion/rename proposal was rejected and is not delivered work.
- **TIN1 / TIN3:** SPEC and all three viewer skills require a nonempty absolute `CODEHARBOR_CONTROL_SOCKET` without NUL and refusal before connection when it is invalid; no socket guessing. DEVELOPMENT and PLAN distinguish opt-in `CH_LIVE_OMP=1` real-model testing from ordinary tests and `CH_LIVE_SSH`, require environment-provided credentials/private state, and describe disabled tool/extension discovery. They do not claim an ordinary test run contacted a model.
- **TWS1 / TWS2 / TWS3:** SPEC section 11.1 documents safe-integer migration metadata validation while retaining stored version 0, global rather than partial `tmux_target` uniqueness, and removal of every obsolete display-name uniqueness rule, including partial, reversed and redundant indexes.
- **TWS4:** SPEC sections 4.5/11.1 and DEVELOPMENT describe a real ordered migration to current database version **6**, including files already stamped **5**. The reported executed private probe showing `5→5 stillRejected=true` belongs to Main's TWS4 evidence, not a test executed for this report section. The final order removes all removable named obsolete label indexes first, then repairs targets and establishes global target uniqueness only if needed, then rebuilds only for an obsolete table-level address constraint. This order also removes NULL-filtered label rules before temporary target vacating. It does not replay old label deduplication; valid duplicate labels, identities, layouts, metadata and orphans remain. Already globally constrained targets are not rewritten. Documentation covers locked reread under `BEGIN IMMEDIATE`, atomic rollback, foreign-key restoration and current reopen without a migration write lock; distinguishes ordinary every-open lookup indexes from ordered constraints; and warns against lowering a real database's stored version. `WORKSPACE_SCHEMA_VERSION`, SQL seed and informational `WorkspaceDb::kSchemaVersion` are now **6**. Historical version **5** still marks the pin column's arrival; RPC advertised/minimum stays **10**, release stays **0.4.3**.
- **TWS5:** SPEC and DEVELOPMENT explain preservation of unrelated named **same-table** indexes and triggers during terminal-table rebuilding. Their stored SQL definitions are captured, the table is copied/swapped, and indexes are restored before triggers within the same transaction. The copy creates no artificial audit entries, while real target-repair updates may legitimately activate existing update triggers. A named global target index is retained without redundant implicit uniqueness; otherwise the canonical target constraint is restored. Replay or final version-write failure rolls the migration back. This is not a compatibility promise for arbitrary custom columns or other table-level constraints.
- **TFS1 / TFS2:** SPEC sections 8.4/8.5 describe revision tokens as opaque metadata including device/inode identity, not content hashes, and reject dangling-symlink creation redirected through missing-component/`..` traversal. Existing outside-root support and final-check/rename race limitations remain.
- **TVW1 / TVW2:** SPEC section 7.5 describes local opaque request identity for listing/probe replies and same-document HTML fragment navigation without exposing internal implementation URLs. These do not add a public internal scheme or bump RPC.
- **TMC1 / TMC3:** SPEC sections 11.2/12.1 distinguish external durable key references, opt-in app-private saved copies and session-only imports. Saved paths are device/container-specific and can be re-adopted without becoming external aliases. The docs state reconnect memory retention, best-effort buffer overwriting and deletion limits, not secure erasure. Saving preserves the input OpenSSH/PEM encryption; CodeHarbor adds no encryption and never persists the passphrase. TMC4 does not add a new feature/security contract.
- **TBR2 / TBR4:** DEVELOPMENT documents the build-input and installed-lock refresh rules explained in those issue sections. TBR1 was already covered by its publish-only-on-success description. The version-bump skill remains the TBR3 owner's update.
- **TMM3:** DEVELOPMENT documents styled-rendering bounds per block of 16,384 UTF-16 code units and 256 lexical pieces. Fallback keeps the full parsed text in one plain-text item but removes inline styling/link actions; blocks without spans also use one item. This is neither a byte limit nor text truncation.
- **TTC1 / TTC2 / TTC3:** SPEC sections 5.1/5.2 describe the native device-pixel-ratio accessor separately from its lowered retained override, recomputation on screen/zoom resize and restoration/listener cleanup on disposal; retirement of pending WebChannel mounts on `pagehide` and disposal of established hosts; and Dev-Session-scoped terminal-row caches with one pending resolution per key, separate row/legacy names and preserved same-context reconnect. TTC4 changes fixture readiness, not the product contract.
- **TAC7:** SPEC section 11.2 and DEVELOPMENT describe desktop `CH_SETTINGS_PATH` on all platforms as a fresh absolute private INI path shared by `AppSettings`, `UiStateStore` and `ServerProfiles`. Unset preserves native defaults; explicitly empty or relative values fail startup before settings stores/WebEngine initialization. The `.webengine` sibling root supplies separate `data` and `cache` paths before persistence/page use. The isolated profile begins off-record because Qt's named constructor initializes preferences immediately; the privileged internal profile remains off-record. The fixture invocation exports this client-local setting; it is not a remote sshd `SetEnv` setting. `CH_LIVE_KNOWN_HOSTS` remains private. These configured private roots are trusted app/test inputs, **not** an ownership/symlink sandbox, and do not redirect SSH trust, saved keys or all local application data.
- **TMV2 / TMV3 / TMV4 / TMV5:** DEVELOPMENT documents the native PDF list's minimum 48dp slot independently of raster caps; the original issue concerns excessive hundreds of page/text/link groups at extreme aspect ratios, not an unbounded list. README and DEVELOPMENT describe dirty native-editor Discard/Cancel for header, hardware, Alt+Left and window-close Back navigation, with stale consent rejected for replacement context. Forced transport/controller retirement and broader RAM retention are excluded. SPEC section 8.3 requires an explicit boolean `truncated` and a nonempty successful-write revision; missing values cannot certify completeness, and an empty revision is create-only request syntax, not a successful saved revision. TMV1's outgoing-resource deactivation adds no new documented capability.
- **TUI1 / TUI4:** SPEC section 3.3 describes creating a real pane ID when agent open/newPane/split materializes an empty placeholder; inventory does not expose that placeholder as an addressable pane. README and SPEC section 8.2 distinguish authoritative current layout from retained visible dirty-editor state. Same-context conflicting layout/URL changes are deferred, stale layout reports and layout-changing agent/palette actions are blocked, and clean/save or consented close reconciles against current authority without closing a same-ID replacement. Other dirty documents can keep the remaining retained display. Same-context TUI4 documents can still save to their current accepted server.
- **TUI6:** README, SPEC and all three viewer skills separately cover whole-context retirement: the old dirty editor stays visible, copyable and locally editable with original identity, baseline, revision, dirty state and frozen recovery tags, but is permanently detached natively before host identity/scope changes. Current App server/session IDs, readiness, authoritative tree and agent inventory advance truthfully and independently. Clean old panes are pruned; new-owner panes are not created or restamped into the old cache; list returns current authority with blank focus while mutations are paused. Unlike TUI4, retired documents cannot save to **any** server, reload, reconnect, or initiate document/stat/read/write/recovery/new-watch I/O. The sole cleanup exception is unwatching one exactly owned active or late-returning pending watch through the captured original client/issuing transport, only while that same binding is current and writable—never through a replacement peer. Local close cannot RPC-close a current same-ID pane; the last retained close reveals current authority. SPEC section 11.3 excludes detached controllers from transient recovery reauthorization. This is RAM preservation, not drafts, history, durable storage or a mobile extension.
- **TUI6 final consent refinement:** Fresh modal consent captures the exact pane, handler, controller, context and complete dirty-document identity set. It rejects replacement/additional documents and stale modals, resets validated old buffers immediately before asynchronous action, and grants no permission over future objects. SPEC explicitly distinguishes current `App.acceptedServerId` from frozen `documentServerId`, requires matching present-string identities and explicit-boolean `contextRetired`, and invalidates an old question when accepted identity changes A→B/empty or retirement changes false→true even if profile/session/layout generation is unchanged. This is not a universal online requirement: fresh consent with unchanged empty accepted identity can authorize local close/quit of already-retired documents offline.

The coordinated canonical files are `README.md`, `docs/DEVELOPMENT.md`, `docs/SPEC.md`, `docs/PLAN.md`, and the three viewer `SKILL.md` files listed under TDC4. No historical dated report was changed. Documentation changes alone are not executable proof; the completed local build, suite, and runtime evidence is recorded below. No warning-free build, remote CI success, or paid model call is claimed.

## Integrated verification

### Executed builds and suites

The following results were observed after integration. Late source changes were rebuilt before their relevant runs. The final complete native run used the same two-job parallel setting that had exposed the cross-test disconnect; it was not converted to a serial run.

| Check | Observed result | Scope and qualifications |
| --- | --- | --- |
| Native development build | Passed | Full native build completed; later PDF/theme and SSH-fixture changes were rebuilt in their affected targets. |
| Final complete native CTest run | 65 targets passed; 0 failed; 0 target-level skips | `ctest --test-dir build/dev --output-on-failure -j 2`; 194.50 seconds. Qt cases within targets still have the platform/authentication skips listed below. |
| Full remote Node test suite | 538 tests: 537 passed, 0 failed, 1 skipped | 34.11 seconds. The skipped test requires explicit opt-in to a paid model interaction; that opt-in was not enabled. |
| Embedded terminal web suite | 86 passed | Includes native-pixel-ratio tracking and early page retirement. |
| Embedded editor web suite | 57 passed | Includes document ownership and recovery behavior. |
| Embedded Markdown web suite | 36 passed | Includes the existing renderer and navigation behavior. |
| TypeScript checking | All four workspaces passed | Remote daemon and terminal, editor, and Markdown web workspaces. |
| Remote TypeScript build | Passed | Production daemon output rebuilt, not only test entry points. |
| Both registered QML lint targets | Completed; 0 errors, 608 warnings | 551 unqualified-access warnings, 56 missing-property warnings, 1 unresolved Qt `QPolygonF` type warning. See the explanation below. |
| Previously failing concurrent SSH/terminal pair, before TTC5 | Failed | Old compiled terminal helper killed a competing connection during channel 20 of the SSH reuse case. |
| Same concurrent pair after TTC5 and TSH5 | Setup and both targets passed, 3/3 | 9.55 seconds; includes fresh commands on an unrelated connection before the drop, after it, and after intended reconnect. |
| Five new real SSH EOF/reentrant cases | Passed | Four fatal-EOF observer/lifetime rows and failed-open reentrant replacement; 7 Qt records including initialization and cleanup. The final full native suite also includes them. |
| Editor reconnect gate without an SSH agent | Setup and live editor target passed, 2/2 | Explicit private identity supplied; confirms TED5 does not secretly rely on an agent. |
| New native PDF-paper and live-theme cases | Passed | Four actual PDF pixel rows plus one dark → light → dark native-control case; 7 Qt records including initialization and cleanup. |
| PDF-disabled mobile build | `ch_mobile` library compiled successfully | Fresh optional configuration with `CH_HAVE_QTPDF=0`; not a claim of a complete app run with Qt PDF absent from the SDK. |
| libssh-disabled compile checks | Both changed SSH source files compiled | `CH_HAVE_LIBSSH=0`; not a full no-SSH application link or runtime claim. |
| Android navigation policy host gate | Passed within the complete native suite | Exercises the production Java decision class on the host, not installation of platform callbacks on an Android device. |

The web test environment emitted jsdom canvas-support warnings. They did not fail those suites and are not represented as a browser-level rendering proof. Real application checks below provide separate surface evidence.

### Real application and before/after checks

These checks used the actual built programs or narrowly scoped disposable probes, not only copied helper logic or mocked return values:

- **Malformed control input:** a deeply nested unsupported control version caused a baseline stack overflow; the repaired parser returned the bounded `bad_request` refusal and continued handling requests.
- **Workspace and file behavior:** baseline/current probes exercised malformed stored versions, repaired uniqueness constraints, the version-5-to-6 ordered migration, preserved rows and unrelated schema objects, cross-device revision identity, missing path components, reentrant watcher shutdown, and a file that grows after initially empty EOF. Details and remaining filesystem race limits are under TWS and TFS.
- **SSH authentication and transport:** the initial native no-session authentication state was observed as AGAIN, and a narrowly controlled external error probe covered ERROR without adding a production bypass. A real deliberately killed SSH connection demonstrated `readFinished=true`, native transport closed, but cached pool Connected before TSH4; the same probe after the fix reported cached Connected false. Real replacement commands and callback-driven close/delete/reconnect cases passed.
- **Private desktop storage and startup:** actual desktop launches rejected explicitly empty and relative settings paths before stores were constructed. A real external HTTP fixture produced browser history/cookie/visited-link/cache paths under the selected private settings root rather than the normal profile.
- **Desktop HTML navigation:** clicking a real same-resource fragment link in a remote HTML viewer changed the native opaque URL to `#target`, produced DOM hash `#target`, scrolled to the target, and left the target visible. No external navigation fallback was substituted.
- **Unsaved editor retention:** the actual desktop editor loaded a private remote file, accepted an unsaved in-memory edit, then survived a real server-side archive and workspace refresh as the same editor/page. The sidebar reported all sessions archived and the application had no active session; commands routed to the old session were refused as `not_active_session`. The old text remained editable and copyable. Save did not write to the remote file. With no active session, Close Focused Viewer Pane still opened a correctly sized fresh local-discard question; local discard removed the old editor without removing its backend viewer row. The original remote file stayed unchanged. This is separate from the smaller consent-publication fixture described under TUI6.
- **Mobile connection and editing:** the actual mobile form recovered from a refused private port to a successful connection while retaining the existing profile ID. Its external key reference loaded. In a real native editor, Back opened the unsaved-change question, Cancel preserved exact clipboard-visible text, and a later Discard returned to panes without changing the remote file. The existing-profile run does not claim to prove new-form saved-ID adoption or opt-in private-key copying; native regressions cover those branches.
- **Native PDF close:** a real Qt 6.10.2 probe showed that clearing the QML PDF source did not close the native document, whereas native close did. The actual mobile PDF page opened the private one-page PDF; using header Back returned to panes and the owned spool file was removed. Native regressions additionally cover shared wrappers, close callbacks attempting to reopen revoked files, and a new connection created reentrantly while old resources are being closed.
- **PDF paper and mobile styling:** the actual default-backend mobile app displayed the ordinary transparent-background PDF with readable black text on white paper after TMV7, with the surrounding empty viewport still dark. The final native editor displayed inherited dark Material styling after TMC5. Its earlier default-backend raised Reload button was readable but wrongly light-themed; the report does not confuse that with the unsupported software-backend background appearance.
- **Terminal screen state:** separately compiled baseline/current screen probes demonstrated the three VT saved-state/erase-boundary regressions before and after repair, rather than asserting implementation text.

**Your Markdown context-menu observation was explicitly checked.** In the actual internal Markdown viewer, the menu offered applicable Copy and Select All actions, not the inapplicable Save Page or View Page Source actions. Select All followed by Copy reached the real X11 clipboard with the rendered document text. That menu correction was already in this round's baseline from the preceding repair round, so it is not counted again as a new finding here. It was not silently ignored, and a fake Save action was not added.

### Report surface check

The assembled report was opened from its real server-side file in the built desktop application's production Markdown viewer. The rendered document contained all 90 unique finding headings, more than 315,000 characters of text, and the complete changed-file inventory. The report title was visibly rendered, and scrolling the actual Markdown container reached the visibly rendered final `tests/web_bundle_rebuild.sh` inventory entry. No replacement HTML or test-only renderer was injected. This checks that the large deliverable reaches the real surface without truncation; it does not replace the behavioral verification above.

### Lint findings, not a warning-free claim

The final lint output was read completely in bounded chunks, including every missing-property and unresolved-type warning. Most warnings concern the existing runtime-provided `app`/`mobile` context objects, nested component IDs, or members reached through generic `QObject`, `QQuickItem`, or Loader types. The unresolved `QPolygonF` warning comes from the Qt PDF geometry property's declarative metadata. The warnings were not suppressed or converted into claimed passes. No additional runtime defect was established from them in this round; actual QML loading, viewer interaction, PDF raster/selection behavior, dialogs, and the full mobile shell passed their exercised host checks. A warning count is not proof that each unexecuted platform branch is correct.

### Integration failures were repaired, not hidden

Earlier complete runs exposed real popup height/width dependency cycles, viewer-retention geometry, native PDF close and delegate lookup problems, obsolete test wire-layout/acceptance fixtures, the editor side connection's ignored identity and lost diagnostic, stale SSH Connected state, and the terminal fixture's broad connection kill. Their fixes and evidence are included under their individual labels. The final application dialog fix preserves Qt Basic's background/inset/padding/header/footer sizing rules while measuring content without a cycle; a wrapper alone was not represented as the completed fix.

The cross-language RPC parity file and all 15 of its protocol-parity gates remain. The native protocol-parity gate remains as well. TDA3 removes incidental English/property-order expectations without deleting those meaningful contract checks. QML warning assertions were not relaxed to hide dialog failures. No meaningful failing behavior was narrowed away merely to obtain a green suite.

## Explicit limits and remaining uncertainty

- This was a broad source audit and exercised Linux repair round, not a mathematical proof that no bugs remain.
- Windows native chrome callbacks, Windows named-pipe agent behavior, macOS platform behavior, and physical Android/iOS lifecycle and rendering were not executed here. Windows-only native chrome cases and the Windows agent case report their platform skips. Source fixes and host tests must not be described as device certification.
- The SSH fixture does not provide a real account password or the server configuration required for successful public-key-plus-password and keyboard-interactive-only authentication. Those two live cases skip with their stated prerequisites; no password was changed to manufacture coverage.
- Native mobile WebView was unavailable in this host build. Host navigation-policy checks do not prove Android/Darwin callback installation, redirect handling, or native WebView ownership-loss behavior on a device.
- The paid-model live test was intentionally not opted into. Its safer opt-in/isolation contract was checked without charging for a model interaction.
- The PDF-disabled result is a mobile-library build, and the no-libssh result is source compilation. Neither is overstated as a complete optional-platform runtime.
- The earlier generic editor-side connection failure was not independently assigned a root cause. TED5 fixes a demonstrated configuration/diagnostic defect, and the explicit-key, no-agent live gate passed. TTC5 directly reproduces and fixes the overlapping SSH/terminal failure; that evidence does not retroactively prove the cause of every earlier generic error.
- File revision checks remain subject to their documented final-check/publication race; adding device/inode identity does not turn metadata into a content hash or establish an unlimited transaction across arbitrary external writers.
- The actual native mobile form run used an existing profile and an external key reference. New-form ID adoption and opt-in key storage are covered by their native regressions, not falsely claimed as manually exercised UI branches.

## Complete changed-file inventory

The following is the explicit set of files owned by this round, including the report itself. Generated bundles, dependency installations, build products, disposable probes, private keys, databases, settings, and screenshots are not part of the source commit.


- `.agents/skills/codeharbor-viewer/SKILL.md`
- `.claude/skills/codeharbor-viewer/SKILL.md`
- `.omp/skills/bump-version/SKILL.md`
- `.omp/skills/bump-version/bump.sh`
- `.omp/skills/codeharbor-viewer/SKILL.md`
- `README.md`
- `docs/DEVELOPMENT.md`
- `docs/PLAN.md`
- `docs/SPEC.md`
- `docs/bug-hunt-2026-11-05-round3.md`
- `remote/sql/schema.sql`
- `remote/src/bridge.ts`
- `remote/src/control-client.ts`
- `remote/src/control.ts`
- `remote/src/files.ts`
- `remote/src/mcp/server.ts`
- `remote/src/tmux.ts`
- `remote/src/workspace.ts`
- `remote/test/bridge.test.ts`
- `remote/test/control.test.ts`
- `remote/test/files.test.ts`
- `remote/test/mcp.test.ts`
- `remote/test/oh-my-pi-live.test.ts`
- `remote/test/rpc-mirror.test.ts`
- `remote/test/tmux-live.test.ts`
- `remote/test/tmux.test.ts`
- `remote/test/viewctl.test.ts`
- `remote/test/workspace.test.ts`
- `src/app/AppController.cpp`
- `src/app/AppSettings.cpp`
- `src/app/AppSettings.h`
- `src/app/ServerProfiles.cpp`
- `src/app/ServerProfiles.h`
- `src/app/SessionBootstrap.cpp`
- `src/app/SessionBootstrap.h`
- `src/app/SessionLayouts.cpp`
- `src/app/SessionLayouts.h`
- `src/app/WindowChromeNative.cpp`
- `src/app/WindowChromeNative.h`
- `src/app/main.cpp`
- `src/app/tests/CMakeLists.txt`
- `src/app/tests/tst_appcontroller.cpp`
- `src/app/tests/tst_appsettings.cpp`
- `src/app/tests/tst_coldstart.cpp`
- `src/app/tests/tst_livereconnect.cpp`
- `src/app/tests/tst_liveshell.cpp`
- `src/app/tests/tst_serverprofiles.cpp`
- `src/app/tests/tst_sessionbootstrap.cpp`
- `src/app/tests/tst_sessionlayouts.cpp`
- `src/app/tests/tst_windowchrome.cpp`
- `src/editor/EditorController.cpp`
- `src/editor/EditorController.h`
- `src/editor/tests/tst_editorcontroller.cpp`
- `src/editor/tests/tst_liveeditor.cpp`
- `src/editor/tests/tst_liveeditorreconnect.cpp`
- `src/mobile/MarkdownModel.cpp`
- `src/mobile/MarkdownModel.h`
- `src/mobile/MobileAppController.cpp`
- `src/mobile/MobileAppController.h`
- `src/mobile/MobileKeyStore.cpp`
- `src/mobile/MobileKeyStore.h`
- `src/mobile/MobileTerminalSession.cpp`
- `src/mobile/MobileTerminalSession.h`
- `src/mobile/MobileTerminalView.cpp`
- `src/mobile/MobileViewerService.cpp`
- `src/mobile/MobileViewerService.h`
- `src/mobile/qml/ConnectPage.qml`
- `src/mobile/qml/KeyImportSheet.qml`
- `src/mobile/qml/MobileMain.qml`
- `src/mobile/qml/PaneHostPage.qml`
- `src/mobile/qml/ViewerDirectoryPage.qml`
- `src/mobile/qml/ViewerEditorPage.qml`
- `src/mobile/qml/ViewerImagePage.qml`
- `src/mobile/qml/ViewerMarkdownPage.qml`
- `src/mobile/qml/ViewerPdfPage.qml`
- `src/mobile/qml/ViewerTextPage.qml`
- `src/mobile/qml/ViewerWebPage.qml`
- `src/mobile/tests/CMakeLists.txt`
- `src/mobile/tests/tst_markdownmodel.cpp`
- `src/mobile/tests/tst_mobilekeystore.cpp`
- `src/mobile/tests/tst_mobilenav.cpp`
- `src/mobile/tests/tst_mobileshell.cpp`
- `src/mobile/tests/tst_mobileterminal.cpp`
- `src/mobile/tests/tst_mobileviewers.cpp`
- `src/models/SessionsModel.cpp`
- `src/models/tests/tst_models.cpp`
- `src/persistence/WorkspaceDb.cpp`
- `src/persistence/WorkspaceDb.h`
- `src/persistence/tests/tst_workspacedb.cpp`
- `src/qml/AppDialog.qml`
- `src/qml/CMakeLists.txt`
- `src/qml/EditorPaneView.qml`
- `src/qml/Main.qml`
- `src/qml/ViewerDirectoryView.qml`
- `src/qml/ViewerPane.qml`
- `src/qml/ViewerRegion.qml`
- `src/qml/ViewerWebView.qml`
- `src/qml/tests/tst_openas.cpp`
- `src/qml/tests/tst_paneidentity.cpp`
- `src/qml/tests/tst_qmlload.cpp`
- `src/remote/CodeharbordClient.cpp`
- `src/remote/CodeharbordClient.h`
- `src/remote/tests/tst_rpcclient.cpp`
- `src/ssh/SshChannelDevice.cpp`
- `src/ssh/SshConnectionPool.cpp`
- `src/ssh/SshConnectionPool.h`
- `src/ssh/tests/tst_livessh.cpp`
- `src/ssh/tests/tst_sshconnectionpool.cpp`
- `src/terminal/TerminalFactory.cpp`
- `src/terminal/TerminalFactory.h`
- `src/terminal/tests/tst_liveterminalfactory.cpp`
- `src/terminal/tests/tst_terminalfactory.cpp`
- `src/terminal/tests/tst_terminalpage.cpp`
- `src/viewers/InternalUrlSchemeHandler.h`
- `src/viewers/ViewerModel.cpp`
- `src/viewers/ViewerModel.h`
- `src/viewers/ViewerProfiles.cpp`
- `src/viewers/ViewerProfiles.h`
- `src/viewers/tests/tst_liveviewers.cpp`
- `src/viewers/tests/tst_viewers.cpp`
- `src/vt/VtScreen.cpp`
- `src/vt/VtScreen.h`
- `src/vt/tests/tst_vt.cpp`
- `src/web/markdown/build.mjs`
- `src/web/terminal/build.mjs`
- `src/web/terminal/src/dpr.ts`
- `src/web/terminal/src/index.ts`
- `src/web/terminal/test/bootstrap.test.ts`
- `src/web/terminal/test/dpr.test.ts`
- `tests/bump_version_tag.sh`
- `tests/web_bundle_rebuild.sh`
